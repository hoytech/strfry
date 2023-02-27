#include <quadrable.h>
#include <quadrable/transport.h>

#include "RelayServer.h"
#include "DBQuery.h"


void RelayServer::runYesstr(ThreadPool<MsgYesstr>::Thread &thr) {
    auto qdb = getQdbInstance();

    struct SyncState {
        quadrable::MemStore m;
    };

    struct SyncStateCollection {
        RelayServer *server;
        quadrable::Quadrable *qdb;
        flat_hash_map<uint64_t, flat_hash_map<uint64_t, SyncState>> conns; // connId -> reqId -> SyncState

        SyncStateCollection(RelayServer *server_, quadrable::Quadrable *qdb_) : server(server_), qdb(qdb_) {}

        SyncState *lookup(uint64_t connId, uint64_t reqId) {
            if (!conns.contains(connId)) return nullptr;
            if (!conns[connId].contains(reqId)) return nullptr;
            return &conns[connId][reqId];
        }

        SyncState *newRequest(lmdb::txn &txn, uint64_t connId, uint64_t reqId, std::string_view filterStr) {
            if (!conns.contains(connId)) conns.try_emplace(connId);
            if (conns[connId].contains(reqId)) {
                LI << "Client tried to re-use reqId for new filter, ignoring";
                return &conns[connId][reqId];
            }
            conns[connId].try_emplace(reqId);
            auto &s = conns[connId][reqId];

            LI << "Yesstr sync. filter: '" << filterStr << "'";

            if (filterStr == "{}") {
                qdb->checkout("events");
                uint64_t nodeId = qdb->getHeadNodeId(txn);

                qdb->withMemStore(s.m, [&]{
                    qdb->writeToMemStore = true;
                    qdb->checkout(nodeId);
                });
            } else {
                // FIXME: The following blocks the whole thread for the query duration. Should interleave it
                // with other requests like RelayReqWorker does.

                std::vector<uint64_t> levIds;
                DBQuery query(tao::json::from_string(filterStr));

                while (1) {
                    bool complete = query.process(txn, [&](const auto &sub, uint64_t levId, std::string_view){
                        levIds.push_back(levId);
                    }, MAX_U64, cfg().relay__logging__dbScanPerf);

                    if (complete) break;
                }

                LI << "Filter matched " << levIds.size() << " local events";

                qdb->withMemStore(s.m, [&]{
                    qdb->writeToMemStore = true;
                    qdb->checkout();

                    auto changes = qdb->change();

                    for (auto levId : levIds) {
                        changes.putReuse(txn, levId);
                    }

                    changes.apply(txn);
                });
            }

            return &s;
        }


        void handleRequest(lmdb::txn &txn, uint64_t connId, uint64_t reqId, std::string_view filterStr, std::string_view reqsEncoded) {
            SyncState *s = lookup(connId, reqId);

            if (!s) s = newRequest(txn, connId, reqId, filterStr);

            auto reqs = quadrable::transport::decodeSyncRequests(reqsEncoded);

            quadrable::SyncResponses resps;

            qdb->withMemStore(s->m, [&]{
                qdb->writeToMemStore = true;
                resps = qdb->handleSyncRequests(txn, qdb->getHeadNodeId(txn), reqs, 100'000);
            });

            std::string respsEncoded = quadrable::transport::encodeSyncResponses(resps);

            flatbuffers::FlatBufferBuilder builder;

            auto respOffset = Yesstr::CreateResponse(builder,
                reqId,
                Yesstr::ResponsePayload::ResponsePayload_ResponseSync,
                Yesstr::CreateResponseSync(builder,
                    builder.CreateVector((uint8_t*)respsEncoded.data(), respsEncoded.size())
                ).Union()
            );

            builder.Finish(respOffset);

            std::string respMsg = std::string("Y") + std::string(reinterpret_cast<char*>(builder.GetBufferPointer()), builder.GetSize());
            server->sendToConnBinary(connId, std::move(respMsg));
        }

        void closeConn(uint64_t connId) {
            conns.erase(connId);
        }
    };

    SyncStateCollection states(this, &qdb);


    while(1) {
        auto newMsgs = thr.inbox.pop_all();

        auto txn = env.txn_ro();

        for (auto &newMsg : newMsgs) {
            if (auto msg = std::get_if<MsgYesstr::SyncRequest>(&newMsg.msg)) {
                const auto *req = parseYesstrRequest(msg->yesstrMessage); // validated by ingester
                const auto *reqSync = req->payload_as<Yesstr::RequestSync>();

                try {
                    states.handleRequest(txn, msg->connId, req->requestId(), sv(reqSync->filter()), sv(reqSync->reqsEncoded()));
                } catch (std::exception &e) {
                    sendNoticeError(msg->connId, std::string("yesstr failure: ") + e.what());
                }
            } else if (auto msg = std::get_if<MsgYesstr::CloseConn>(&newMsg.msg)) {
                states.closeConn(msg->connId);
            }
        }
    }
}
