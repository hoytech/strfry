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
      stories
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

    Decompressor decomp;
    auto txn = env.txn_ro();

    flat_hash_map<Bytes32, EventInfo> eventInfoCache;
    std::vector<FilteredEvent> output;

    uint64_t limit = 10000;
    uint64_t timeWindow = 86400*2;
    uint64_t threshold = 10;

    uint64_t now = hoytech::curr_time_s();

    env.generic_foreachFull(txn, env.dbi_Event__created_at, lmdb::to_sv<uint64_t>(MAX_U64), lmdb::to_sv<uint64_t>(MAX_U64), [&](auto k, auto v) {
        if (output.size() > limit) return false;

        auto ev = lookupEventByLevId(txn, lmdb::from_sv<uint64_t>(v));
        PackedEventView packed(ev.buf);

        auto kind = packed.kind();
        auto id = packed.id();

        if (kind == 1) {
            bool foundETag = false;
            packed.foreachTag([&](char tagName, std::string_view tagVal){
                if (tagName == 'e') {
                    auto tagEventId = tagVal;
                    eventInfoCache.emplace(tagEventId, EventInfo{});
                    eventInfoCache[tagEventId].comments++;
                    foundETag = true;
                }
                return true;
            });
            if (foundETag) return true; // not root event

            eventInfoCache.emplace(id, EventInfo{});
            auto &eventInfo = eventInfoCache[id];

            if (eventInfo.reactions < threshold) return true;

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

    std::sort(output.begin(), output.end(), [](const auto &a, const auto &b){
        return a.info.reactions > b.info.reactions;
    });

    for (const auto &o : output) {
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
