#include <docopt.h>
#include <tao/json.hpp>

#include <quadrable.h>
#include <quadrable/transport.h>

#include "golpe.h"

#include "WriterPipeline.h"
#include "Subscription.h"
#include "WSConnection.h"
#include "DBScan.h"
#include "filters.h"
#include "events.h"
#include "yesstr.h"


static const char USAGE[] =
R"(
    Usage:
      sync <url> [--filter=<filter>] [--dir=<dir>]

    Options:
      --filter=<filter>  Nostr filter (either single filter object or array of filters)
      --dir=<dir>        Direction: down, up, or both [default: down]
)";


struct SyncController {
    quadrable::Quadrable *qdb;
    WSConnection *ws;

    quadrable::Quadrable::Sync sync;
    quadrable::MemStore m;

    uint64_t ourNodeId = 0;
    quadrable::SyncRequests reqs;
    bool sentFirstReq = false;

    SyncController(quadrable::Quadrable *qdb_, WSConnection *ws_) : qdb(qdb_), ws(ws_), sync(qdb_) { }

    void init(lmdb::txn &txn) {
        qdb->withMemStore(m, [&]{
            qdb->writeToMemStore = true;
            ourNodeId = qdb->getHeadNodeId(txn);
            sync.init(txn, ourNodeId);
        });
    }

    bool sendRequests(lmdb::txn &txn, const std::string &filterStr) {
        qdb->withMemStore(m, [&]{
            qdb->writeToMemStore = true;
            reqs = sync.getReqs(txn, 10'000);
        });

        if (reqs.size() == 0) return false;

        std::string reqsEncoded = quadrable::transport::encodeSyncRequests(reqs);

        flatbuffers::FlatBufferBuilder builder;

        auto reqOffset = Yesstr::CreateRequest(builder,
            123,
            Yesstr::RequestPayload::RequestPayload_RequestSync,
            Yesstr::CreateRequestSync(builder,
                (filterStr.size() && !sentFirstReq) ? builder.CreateString(filterStr) : 0,
                builder.CreateVector((uint8_t*)reqsEncoded.data(), reqsEncoded.size())
            ).Union()
        );

        builder.Finish(reqOffset);

        std::string reqMsg = std::string("Y") + std::string(reinterpret_cast<char*>(builder.GetBufferPointer()), builder.GetSize());
        size_t compressedSize;
        ws->send(reqMsg, uWS::OpCode::BINARY, &compressedSize);
        LI << "SEND size=" << reqMsg.size() << " compressed=" << compressedSize;

        sentFirstReq = true;

        return true;
    }

    void handleResponses(lmdb::txn &txn, std::string_view msg) {
        verifyYesstrResponse(msg);
        const auto *resp = parseYesstrResponse(msg);
        const auto *respSync = resp->payload_as_ResponseSync();

        auto resps = quadrable::transport::decodeSyncResponses(sv(respSync->respsEncoded()));

        qdb->withMemStore(m, [&]{
            qdb->writeToMemStore = true;
            sync.addResps(txn, reqs, resps);
        });
    }

    void finish(lmdb::txn &txn, std::function<void(std::string_view)> onNewLeaf, std::function<void(std::string_view)> onMissingLeaf) {
        qdb->withMemStore(m, [&]{
            qdb->writeToMemStore = true;

            sync.diff(txn, ourNodeId, sync.nodeIdShadow, [&](auto dt, const auto &node){
                if (dt == quadrable::Quadrable::DiffType::Added) {
                    // node exists only on the provider-side
                    LI << "NEW LEAF: " << node.leafVal();
                    onNewLeaf(node.leafVal());
                } else if (dt == quadrable::Quadrable::DiffType::Deleted) {
                    // node exists only on the syncer-side
                    LI << "MISSING LEAF: " << node.leafVal();
                    onMissingLeaf(node.leafVal());
                } else if (dt == quadrable::Quadrable::DiffType::Changed) {
                    // nodes differ. node is the one on the provider-side
                }
            });
        });
    }
};



void cmd_sync(const std::vector<std::string> &subArgs) {
    std::map<std::string, docopt::value> args = docopt::docopt(USAGE, subArgs, true, "");

    std::string url = args["<url>"].asString();

    std::string filterStr;
    if (args["--filter"]) filterStr = args["--filter"].asString();
    else filterStr = "{}";

    std::string dir = args["--dir"] ? args["--dir"].asString() : "down";
    if (dir != "up" && dir != "down" && dir != "both") throw herr("invalid direction: ", dir, ". Should be one of up/down/both");
    if (dir != "down") throw herr("only down currently supported"); // FIXME


    std::unique_ptr<SyncController> controller;
    WriterPipeline writer;
    WSConnection ws(url);
    auto qdb = getQdbInstance();


    ws.reconnect = false;


    if (filterStr.size()) {
        std::vector<uint64_t> levIds;

        tao::json::value filterJson;

        try {
            filterJson = tao::json::from_string(filterStr);
        } catch (std::exception &e) {
            LE << "Couldn't parse filter JSON: " << filterStr;
            ::exit(1);
        }

        auto filterGroup = NostrFilterGroup::unwrapped(filterJson);

        Subscription sub(1, "junkSub", filterGroup);

        DBScanQuery query(sub);
        auto txn = env.txn_ro();

        while (1) {
            bool complete = query.process(txn, MAX_U64, false, [&](const auto &sub, uint64_t levId){
                levIds.push_back(levId);
            });

            if (complete) break;
        }

        LI << "Filter matched " << levIds.size() << " local events";

        controller = std::make_unique<SyncController>(&qdb, &ws);

        qdb.withMemStore(controller->m, [&]{
            qdb.writeToMemStore = true;
            qdb.checkout();

            auto changes = qdb.change();

            for (auto levId : levIds) {
                changes.putReuse(txn, levId);
            }

            changes.apply(txn);
        });

        controller->init(txn);
    } else {
        auto txn = env.txn_ro();

        controller = std::make_unique<SyncController>(&qdb, &ws);
        controller->init(txn);
    }



    ws.onConnect = [&]{
        auto txn = env.txn_ro();

        controller->sendRequests(txn, filterStr);
    };

    ws.onMessage = [&](auto msg, uWS::OpCode opCode, size_t compressedSize){
        auto txn = env.txn_ro();

        if (!controller) {
            LW << "No sync active, ignoring message";
            return;
        }

        if (opCode == uWS::OpCode::TEXT) {
            LW << "Unexpected non-yesstr message from relay: " << msg;
            return;
        }

        LI << "RECV size=" << msg.size() << " compressed=" << compressedSize;
        controller->handleResponses(txn, msg);

        if (!controller->sendRequests(txn, filterStr)) {
            LI << "Syncing done, writing/sending events";
            controller->finish(txn,
                [&](std::string_view newLeaf){
                    // FIXME: relay could crash client here by sending invalid JSON
                    writer.inbox.push_move(WriterPipelineInput{ tao::json::from_string(std::string(newLeaf)), EventSourceType::Sync, url });
                },
                [&](std::string_view){
                }
            );

            writer.flush();
            ::exit(0);
        }
    };




    ws.run();
}
