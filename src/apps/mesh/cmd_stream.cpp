#include <docopt.h>
#include <tao/json.hpp>
#include <hoytech/file_change_monitor.h>

#include "golpe.h"

#include "EventStreamer.h"
#include "WriterPipeline.h"
#include "PluginEventSifter.h"
#include "events.h"


static const char USAGE[] =
R"(
    Usage:
      stream <url> [--dir=<dir>]

    Options:
      --dir=<dir>   Direction: down, up, or both [default: down]
)";



void cmd_stream(const std::vector<std::string> &subArgs) {
    std::map<std::string, docopt::value> args = docopt::docopt(USAGE, subArgs, true, "");

    std::string url = args["<url>"].asString();
    std::string dir = args["--dir"] ? args["--dir"].asString() : "down";

    if (dir != "up" && dir != "down" && dir != "both") throw herr("invalid direction: ", dir, ". Should be one of up/down/both");

    flat_hash_set<std::string> downloadedIds;
    EventStreamer streamer(url, dir);
    WriterPipeline writer;
    Decompressor decomp;
    PluginEventSifter writePolicyPlugin;

    streamer.onEvent = [&](tao::json::value &&evJson, const WSConnection &ws) {
        std::string okMsg;
        auto res = writePolicyPlugin.acceptEvent(cfg().relay__writePolicy__plugin, evJson, hoytech::curr_time_s(), EventSourceType::Stream, ws.remoteAddr, okMsg);
        if (res == PluginEventSifterResult::Accept) {
            downloadedIds.emplace(from_hex(evJson.at("id").get_string()));
            writer.write({ std::move(evJson), EventSourceType::Stream, url });
        } else {
            LI << "[" << ws.remoteAddr << "] write policy blocked event from " << url << " : " << evJson.at("id").get_string() << " -> " << okMsg;
        }
    };


    uint64_t currEventId;

    {
        auto txn = env.txn_ro();
        currEventId = getMostRecentLevId(txn);
    }

    std::unique_ptr<hoytech::file_change_monitor> dbChangeWatcher;

    if (dir == "up" || dir == "both") {
        dbChangeWatcher = std::make_unique<hoytech::file_change_monitor>(dbDir + "/data.mdb");

        dbChangeWatcher->setDebounce(100);

        dbChangeWatcher->run([&](){
            auto txn = env.txn_ro();

            env.foreach_Event(txn, [&](auto &ev){
                currEventId = ev.primaryKeyId;

                auto id = std::string(sv(ev.flat_nested()->id()));
                if (downloadedIds.find(id) != downloadedIds.end()) {
                    downloadedIds.erase(id);
                    return true;
                }

                std::string msg = std::string("[\"EVENT\",");
                msg += getEventJson(txn, decomp, ev.primaryKeyId);
                msg += "]";

                auto msgPtr = std::make_shared<std::string>(std::move(msg));

                streamer.sendEvent(msgPtr);

                return true;
            }, false, currEventId + 1);

            streamer.trigger();
        });
    }


    streamer.run();
}
