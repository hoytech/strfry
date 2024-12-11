#include <docopt.h>
#include <algorithm>

#include "golpe.h"

#include "Decompressor.h"
#include "events.h"
#include "Bytes32.h"
#include "WebData.h"


static const char USAGE[] =
R"(
    Usage:
      stories [--top=<top>] [--days=<days>] [--oddbean]
)";


struct EventInfo {
    uint64_t comments = 0;
    uint64_t reactions = 0;
};

struct FilteredEvent {
    uint64_t levId;
    Bytes32 id;
    uint64_t created_at;

    EventInfo info;
};

void cmd_stories(const std::vector<std::string> &subArgs) {
    std::map<std::string, docopt::value> args = docopt::docopt(USAGE, subArgs, true, "");

    uint64_t top = 10;
    if (args["--top"]) top = args["--top"].asLong();
    uint64_t days = 2;
    if (args["--days"]) days = args["--days"].asLong();
    bool oddbeanOnly = args["--oddbean"].asBool();

    uint64_t eventLimit = 1000;
    uint64_t scanLimit = 100000;
    uint64_t timeWindow = 86400*days;
    uint64_t threshold = 10;

    Decompressor decomp;
    auto txn = env.txn_ro();

    flat_hash_map<Bytes32, EventInfo> eventInfoCache;
    std::vector<FilteredEvent> output;

    uint64_t now = hoytech::curr_time_s();

    env.generic_foreachFull(txn, env.dbi_Event__created_at, lmdb::to_sv<uint64_t>(MAX_U64), lmdb::to_sv<uint64_t>(MAX_U64), [&](auto k, auto v) {
        if (output.size() > eventLimit) return false;
        if (scanLimit == 0) return false;
        scanLimit--;

        auto ev = lookupEventByLevId(txn, lmdb::from_sv<uint64_t>(v));
        PackedEventView packed(ev.buf);

        auto kind = packed.kind();
        auto id = packed.id();

        if (kind == 1) {
            bool foundETag = false;
            bool isOddbeanTopic = false;
            packed.foreachTag([&](char tagName, std::string_view tagVal){
                if (tagName == 'e') {
                    auto tagEventId = tagVal;
                    eventInfoCache.emplace(tagEventId, EventInfo{});
                    eventInfoCache[tagEventId].comments++;
                    foundETag = true;
                }
                if (tagName == 't' && tagVal == "oddbean") isOddbeanTopic = true;
                return true;
            });
            if (foundETag) return true; // not root event

            eventInfoCache.emplace(id, EventInfo{});
            auto &eventInfo = eventInfoCache[id];

            if (oddbeanOnly) {
                if (!isOddbeanTopic) return true;

                // Filter out posts from oddbot clients
                tao::json::value json = tao::json::from_string(getEventJson(txn, decomp, ev.primaryKeyId));
                const auto &tags = json.at("tags").get_array();
                for (const auto &t : tags) {
                    const auto &tArr = t.get_array();
                    if (tArr.at(0) == "client" && tArr.size() >= 2 && tArr.at(1) == "oddbot") return true;
                }
            } else {
                if (eventInfo.reactions < threshold) return true;
            }

            output.emplace_back(FilteredEvent{ev.primaryKeyId, id, packed.created_at(), eventInfo});
        } else if (kind == 7) {
            std::optional<std::string_view> lastETag;
            packed.foreachTag([&](char tagName, std::string_view tagVal){
                if (tagName == 'e') lastETag = tagVal;
                return true;
            });

            if (lastETag) {
                auto tagEventId = *lastETag;
                eventInfoCache.emplace(tagEventId, EventInfo{});
                eventInfoCache[tagEventId].reactions++;
            }
        }

        return true;
    }, true);

    output.erase(std::remove_if(output.begin(), output.end(), [&](const auto &o){
        return o.created_at < (now - timeWindow);
    }), output.end());

    if (!oddbeanOnly) {
        std::sort(output.begin(), output.end(), [](const auto &a, const auto &b){
            return a.info.reactions > b.info.reactions;
        });
    }

    for (const auto &o : output) {
        if (top == 0) break;
        top--;

        tao::json::value ev = tao::json::from_string(getEventJson(txn, decomp, o.levId));

        std::string content = ev.at("content").get_string();
        auto firstUrl = stripUrls(content);
        if (content.size() > 100) content = content.substr(0, 100-3) + "...";

        tao::json::value encoded = tao::json::value({
            { "summary", content },
            { "url", firstUrl },
            { "id", ev.at("id") },
            { "timestamp", ev.at("created_at") },
            { "reactions", o.info.reactions },
            { "comments", o.info.comments },
        });

        std::cout << tao::json::to_string(encoded) << "\n";
    }
}
