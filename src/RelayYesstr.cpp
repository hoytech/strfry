#include <quadrable.h>
#include <quadrable/transport.h>

#include "RelayServer.h"
#include "DBScan.h"


void RelayServer::runYesstr(ThreadPool<MsgYesstr>::Thread &thr) {
    quadrable::Quadrable qdb;
    {
        auto txn = env.txn_ro();
        qdb.init(txn);
    }


    struct SyncState {
        quadrable::MemStore m;
    };

    struct SyncStateCollection {
        RelayServer *server;
        quadrable::Quadrable *qdb;
        std::map<uint64_t, std::map<uint64_t, SyncState>> conns; // connId -> reqId -> SyncState

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

                LI << "Yesstr sync: Running filter: " << filterStr;

                std::vector<uint64_t> quadEventIds;
                auto filterGroup = NostrFilterGroup::unwrapped(tao::json::from_string(filterStr));
                Subscription sub(1, "junkSub", filterGroup);
                DBScanQuery query(sub);

                while (1) {
                    bool complete = query.process(txn, MAX_U64, [&](const auto &sub, uint64_t quadId){
                        quadEventIds.push_back(quadId);
                    });

                    if (complete) break;
                }

                LI << "Filter matched " << quadEventIds.size() << " local events";

                qdb->withMemStore(s.m, [&]{
                    qdb->writeToMemStore = true;
                    qdb->checkout();

                    auto changes = qdb->change();

                    for (auto id : quadEventIds) {
                        changes.putReuse(txn, id);
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
                LI << "ZZZ NODE " << qdb->getHeadNodeId(txn);
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

                states.handleRequest(txn, msg->connId, req->requestId(), sv(reqSync->filter()), sv(reqSync->reqsEncoded()));
            } else if (auto msg = std::get_if<MsgYesstr::CloseConn>(&newMsg.msg)) {
                states.closeConn(msg->connId);
            }
        }
    }
}
