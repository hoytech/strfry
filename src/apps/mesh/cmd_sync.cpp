#include <docopt.h>
#include <tao/json.hpp>
#include <Negentropy.h>

#include "golpe.h"

#include "WriterPipeline.h"
#include "Subscription.h"
#include "WSConnection.h"
#include "DBQuery.h"
#include "filters.h"
#include "events.h"
#include "PluginEventSifter.h"


static const char USAGE[] =
R"(
    Usage:
      sync <url> [--filter=<filter>] [--dir=<dir>] [--frame-size-limit=<frame-size-limit>]

    Options:
      --filter=<filter>  Nostr filter (either single filter object or array of filters)
      --dir=<dir>        Direction: both, down, up, none [default: both]
      --frame-size-limit=<frame-size-limit>  Limit outgoing negentropy message size (default 60k, 0 for no limit)
)";



void cmd_sync(const std::vector<std::string> &subArgs) {
    std::map<std::string, docopt::value> args = docopt::docopt(USAGE, subArgs, true, "");

    std::string url = args["<url>"].asString();

    std::string filterStr;
    if (args["--filter"]) filterStr = args["--filter"].asString();
    else filterStr = "{}";
    std::string dir = args["--dir"] ? args["--dir"].asString() : "both";
    if (dir != "both" && dir != "up" && dir != "down" && dir != "none") throw herr("invalid direction: ", dir, ". Should be one of both/up/down/none");

    uint64_t frameSizeLimit = 60'000; // default frame limit is 128k. Halve that (hex encoding) and subtract a bit (JSON msg overhead)
    if (args["--frame-size-limit"]) frameSizeLimit = args["--frame-size-limit"].asLong();

    const uint64_t idSize = 16;
    const bool doUp = dir == "both" || dir == "up";
    const bool doDown = dir == "both" || dir == "down";


    tao::json::value filter = tao::json::from_string(filterStr);


    Negentropy ne(idSize, frameSizeLimit);

    {
        DBQuery query(filter);
        Decompressor decomp;

        auto txn = env.txn_ro();

        uint64_t numEvents = 0;
        std::vector<uint64_t> levIds;

        while (1) {
            bool complete = query.process(txn, [&](const auto &sub, uint64_t levId){
                levIds.push_back(levId);
                numEvents++;
            });

            if (complete) break;
        }

        std::sort(levIds.begin(), levIds.end());

        for (auto levId : levIds) {
            auto ev = lookupEventByLevId(txn, levId);
            PackedEventView packed(ev.buf);
            ne.addItem(packed.created_at(), packed.id().substr(0, ne.idSize));
        }

        LI << "Filter matches " << numEvents << " events";
    }

    ne.seal();



    WriterPipeline writer;
    WSConnection ws(url);
    PluginEventSifter writePolicyPlugin;


    ws.reconnect = false;

    ws.onConnect = [&]{
        auto neMsg = to_hex(ne.initiate());
        ws.send(tao::json::to_string(tao::json::value::array({
            "NEG-OPEN",
            "N",
            filter,
            idSize,
            neMsg,
        })));
    };

    auto doExit = [&](int status){
        if (doDown) writer.flush();
        ::exit(status);
    };

    ws.onDisconnect = ws.onError = [&]{
        doExit(1);
    };


    const uint64_t highWaterUp = 100, lowWaterUp = 50;
    const uint64_t batchSizeDown = 50;
    uint64_t inFlightUp = 0;
    bool inFlightDown = false; // bool because we can't count on getting every EVENT we request (might've been deleted mid-query)
    std::vector<std::string> have, need;
    bool syncDone = false;
    uint64_t totalHaves = 0, totalNeeds = 0;
    Decompressor decomp;

    ws.onMessage = [&](auto msgStr, uWS::OpCode opCode, size_t compressedSize){
        try {
            tao::json::value msg = tao::json::from_string(msgStr);

            if (msg.at(0) == "NEG-MSG") {
                uint64_t origHaves = have.size(), origNeeds = need.size();

                std::optional<std::string> neMsg;

                try {
                    neMsg = ne.reconcile(from_hex(msg.at(2).get_string()), have, need);
                } catch (std::exception &e) {
                    LE << "Unable to parse negentropy message from relay: " << e.what();
                    doExit(1);
                }

                totalHaves += have.size() - origHaves;
                totalNeeds += need.size() - origNeeds;

                if (!doUp) have.clear();
                if (!doDown) need.clear();

                if (neMsg) {
                    ws.send(tao::json::to_string(tao::json::value::array({
                        "NEG-MSG",
                        "N",
                        to_hex(*neMsg),
                    })));
                } else {
                    syncDone = true;
                    LI << "Set reconcile complete. Have " << totalHaves << " need " << totalNeeds;

                    ws.send(tao::json::to_string(tao::json::value::array({
                        "NEG-CLOSE",
                        "N",
                    })));
                }
            } else if (msg.at(0) == "OK") {
                inFlightUp--;

                if (!msg.at(2).get_boolean()) {
                    LW << "Unable to upload event " << msg.at(1).get_string() << ": " << msg.at(3).get_string();
                }
            } else if (msg.at(0) == "EVENT") {
                if (msg.get_array().size() < 3) throw herr("array too short");
                auto &evJson = msg.at(2);

                std::string okMsg;
                auto res = writePolicyPlugin.acceptEvent(cfg().relay__writePolicy__plugin, evJson, EventSourceType::Sync, ws.remoteAddr, okMsg);
                if (res == PluginEventSifterResult::Accept) {
                    writer.write({ std::move(evJson), });
                } else {
                    if (okMsg.size()) LI << "[" << ws.remoteAddr << "] write policy blocked event " << evJson.at("id").get_string() << ": " << okMsg;
                }
            } else if (msg.at(0) == "EOSE") {
                inFlightDown = false;
                writer.wait();
            } else if (msg.at(0) == "NEG-ERR") {
                LE << "Got NEG-ERR response from relay: " << msg;
                doExit(1);
            } else {
                LW << "Unexpected message from relay: " << msg;
            }
        } catch (std::exception &e) {
            LE << "Error processing websocket message: " << e.what();
            LW << "MSG: " << msgStr;
        }

        if (doUp && have.size() > 0 && inFlightUp <= lowWaterUp) {
            auto txn = env.txn_ro();

            uint64_t numSent = 0;

            while (have.size() > 0 && inFlightUp < highWaterUp) {
                auto id = std::move(have.back());
                have.pop_back();

                auto ev = lookupEventById(txn, id);
                if (!ev) {
                    LW << "Couldn't upload event because not found (deleted?)";
                    continue;
                }

                std::string sendEventMsg = "[\"EVENT\",";
                sendEventMsg += getEventJson(txn, decomp, ev->primaryKeyId);
                sendEventMsg += "]";
                ws.send(sendEventMsg);

                numSent++;
                inFlightUp++;
            }

            if (numSent > 0) LI << "UP: " << numSent << " events (" << have.size() << " remaining)";
        }

        if (doDown && need.size() > 0 && !inFlightDown) {
            tao::json::value ids = tao::json::empty_array;

            while (need.size() > 0 && ids.get_array().size() < batchSizeDown) {
                ids.emplace_back(to_hex(need.back()));
                need.pop_back();
            }

            LI << "DOWN: " << ids.get_array().size() << " events (" << need.size() << " remaining)";

            ws.send(tao::json::to_string(tao::json::value::array({
                "REQ",
                "R",
                tao::json::value({
                    { "ids", std::move(ids) }
                }),
            })));

            inFlightDown = true;
        }

        if (syncDone && have.size() == 0 && need.size() == 0 && inFlightUp == 0 && !inFlightDown) {
            doExit(0);
        }
    };

    ws.run();
}
