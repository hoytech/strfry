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


static const char USAGE[] =
R"(
    Usage:
      sync <url> [--filter=<filter>] [--dir=<dir>]

    Options:
      --filter=<filter>  Nostr filter (either single filter object or array of filters)
      --dir=<dir>        Direction: both, down, up, none [default: both]
)";



void cmd_sync(const std::vector<std::string> &subArgs) {
    std::map<std::string, docopt::value> args = docopt::docopt(USAGE, subArgs, true, "");

    std::string url = args["<url>"].asString();

    std::string filterStr;
    if (args["--filter"]) filterStr = args["--filter"].asString();
    else filterStr = "{}";
    std::string dir = args["--dir"] ? args["--dir"].asString() : "both";
    if (dir != "both" && dir != "up" && dir != "down" && dir != "none") throw herr("invalid direction: ", dir, ". Should be one of both/up/down/none");

    const uint64_t idSize = 16;
    const bool doUp = dir == "both" || dir == "up";
    const bool doDown = dir == "both" || dir == "down";


    tao::json::value filter = tao::json::from_string(filterStr);


    Negentropy ne(idSize);

    {
        DBQuery query(filter);
        Decompressor decomp;

        auto txn = env.txn_ro();

        uint64_t numEvents = 0;

        while (1) {
            bool complete = query.process(txn, [&](const auto &sub, uint64_t levId, std::string_view eventPayload){
                auto ev = lookupEventByLevId(txn, levId);
                ne.addItem(ev.flat_nested()->created_at(), sv(ev.flat_nested()->id()).substr(0, ne.idSize));

                numEvents++;
            });

            if (complete) break;
        }

        LI << "Filter matches " << numEvents << " events";
    }

    ne.seal();



    WriterPipeline writer;
    WSConnection ws(url);
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


    const uint64_t highWaterUp = 100, lowWaterUp = 50;
    const uint64_t batchSizeDown = 50;
    uint64_t inFlightUp = 0, inFlightDown = 0;
    std::vector<std::string> have, need;
    bool syncDone = false;
    uint64_t totalHaves = 0, totalNeeds = 0;
    Decompressor decomp;

    ws.onMessage = [&](auto msgStr, uWS::OpCode opCode, size_t compressedSize){
        try {
            tao::json::value msg = tao::json::from_string(msgStr);

            if (msg.at(0) == "NEG-MSG") {
                uint64_t origHaves = have.size(), origNeeds = need.size();

                auto neMsg = ne.reconcile(from_hex(msg.at(2).get_string()), have, need);

                totalHaves += have.size() - origHaves;
                totalNeeds += need.size() - origNeeds;

                if (!doUp) have.clear();
                if (!doDown) need.clear();

                if (neMsg.size() == 0) {
                    syncDone = true;
                    LI << "Set reconcile complete. Have " << totalHaves << " need " << totalNeeds;
                } else {
                    ws.send(tao::json::to_string(tao::json::value::array({
                        "NEG-MSG",
                        "N",
                        to_hex(neMsg),
                    })));
                }
            } else if (msg.at(0) == "OK") {
                inFlightUp--;

                if (!msg.at(2).get_boolean()) {
                    LW << "Unable to upload event " << msg.at(1).get_string() << ": " << msg.at(3).get_string();
                }
            } else if (msg.at(0) == "EVENT") {
                writer.inbox.push_move({ std::move(msg.at(2)), EventSourceType::Sync, url });
            } else if (msg.at(0) == "EOSE") {
                inFlightDown = 0;
            }
        } catch (std::exception &e) {
            LE << "Error processing websocket message: " << e.what();
            LW << "MSG: " << msgStr;
        }

        if (doUp && have.size() > 0 && inFlightUp < lowWaterUp) {
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

        if (doDown && need.size() > 0 && inFlightDown == 0) {
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

            inFlightDown = 1;
        }

        if (syncDone && have.size() == 0 && need.size() == 0 && inFlightUp == 0 && inFlightDown == 0) {
            if (doDown) writer.flush();
            ::exit(0);
        }
    };

    ws.run();
}
