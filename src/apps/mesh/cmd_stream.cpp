#include <docopt.h>
#include <tao/json.hpp>
#include <hoytech/protected_queue.h>
#include <hoytech/file_change_monitor.h>

#include "golpe.h"

#include "WriterPipeline.h"
#include "Subscription.h"
#include "WSConnection.h"
#include "events.h"
#include "PluginWritePolicy.h"


static const char USAGE[] =
R"(
    Usage:
      stream <url> [--dir=<dir>]

    Options:
      --dir=<dir>   Direction: down, up, or both [default: down]
)";



struct EventStreamer {
    std::string url;
    WSConnection ws;
    std::string dir;
    tao::json::value filter;

    std::function<void(tao::json::value &&, const WSConnection &ws)> onEvent;

  private:

    hoytech::protected_queue<std::shared_ptr<std::string>> inbox;

  public:

    EventStreamer(const std::string &url, const std::string &dir, tao::json::value filter = tao::json::empty_object) : url(url), ws(url), dir(dir), filter(filter) {
        filter["limit"] = 0;
    }

    void sendEvent(std::shared_ptr<std::string> msg) {
        inbox.push_move(std::move(msg));
    }

    void trigger() {
        ws.trigger();
    }

    void run() {
        ws.onConnect = [&]{
            if (dir == "down" || dir == "both") {
                auto encoded = tao::json::to_string(tao::json::value::array({ "REQ", "X", filter }));
                ws.send(encoded);
            }
        };

        ws.onMessage = [&](auto msg, uWS::OpCode, size_t){
            auto origJson = tao::json::from_string(msg);

            if (origJson.is_array()) {
                if (origJson.get_array().size() < 2) throw herr("array too short");

                auto &msgType = origJson.get_array().at(0);
                if (msgType == "EOSE") {
                    return;
                } else if (msgType == "NOTICE") {
                    LW << "NOTICE message from " << url << " : " << tao::json::to_string(origJson);
                    return;
                } else if (msgType == "OK") {
                    if (!origJson.get_array().at(2).get_boolean()) {
                        LW << "Event not written by " << url << " : " << origJson;
                    }
                } else if (msgType == "EVENT") {
                    if (dir == "down" || dir == "both") {
                        if (origJson.get_array().size() < 3) throw herr("array too short");
                        auto &evJson = origJson.at(2);

                        // FIXME: validate that the event actually matches provided filter?
                        if (onEvent) onEvent(std::move(evJson), ws);
                    } else {
                        LW << "Unexpected EVENT from " << url;
                    }
                } else {
                    throw herr("unexpected first element");
                }
            } else {
                throw herr("unexpected message");
            }
        };

        ws.onTrigger = [&]{
            auto msgs = inbox.pop_all();
            for (auto &msg : msgs) ws.send(*msg);
        };

        ws.run();
    }
};



void cmd_stream(const std::vector<std::string> &subArgs) {
    std::map<std::string, docopt::value> args = docopt::docopt(USAGE, subArgs, true, "");

    std::string url = args["<url>"].asString();
    std::string dir = args["--dir"] ? args["--dir"].asString() : "down";

    if (dir != "up" && dir != "down" && dir != "both") throw herr("invalid direction: ", dir, ". Should be one of up/down/both");

    flat_hash_set<std::string> downloadedIds;
    EventStreamer streamer(url, dir);
    WriterPipeline writer;
    Decompressor decomp;
    PluginWritePolicy writePolicy;

    streamer.onEvent = [&](tao::json::value &&evJson, const WSConnection &ws) {
        std::string okMsg;
        auto res = writePolicy.acceptEvent(evJson, hoytech::curr_time_s(), EventSourceType::Stream, ws.remoteAddr, okMsg);
        if (res == WritePolicyResult::Accept) {
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
