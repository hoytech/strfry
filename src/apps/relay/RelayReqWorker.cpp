#include "RelayServer.h"
#include "QueryScheduler.h"
#include "DMFilter.h"


void RelayServer::runReqWorker(ThreadPool<MsgReqWorker>::Thread &thr) {
    Decompressor decomp;
    QueryScheduler queries;
    flat_hash_map<uint64_t, Bytes32> connIdToAuthedPubkey;

    queries.onEvent = [&](lmdb::txn &txn, const auto &sub, uint64_t levId, std::string_view eventPayload){
        if (sub.countOnly) return;
        auto it = connIdToAuthedPubkey.find(sub.connId);
        Bytes32 subscriberAuthedPubkey = it == connIdToAuthedPubkey.end() ? Bytes32() : it->second;
        if (!DMFilter::shouldSendToSubscriber(eventPayload, subscriberAuthedPubkey)) {
            return; 
        }
        
        sendEvent(sub.connId, sub.subId, decodeEventPayload(txn, decomp, eventPayload, nullptr, nullptr));
    };

    queries.onComplete = [&](lmdb::txn &, Subscription &sub, uint64_t total){
        if (sub.countOnly) {
            bool limited = false;

            if (total > cfg().relay__maxFilterLimitCount) {
                total = cfg().relay__maxFilterLimitCount;
                limited = true;
            }

            tao::json::value countBody = tao::json::value({
                { "count", total },
            });

            if (limited) countBody["limited"] = true;

            sendToConn(sub.connId, tao::json::to_string(tao::json::value::array({ "COUNT", sub.subId.str(), countBody })));
        } else {
            PROM_INC_RELAY_MSG("EOSE");
            sendToConn(sub.connId, tao::json::to_string(tao::json::value::array({ "EOSE", sub.subId.str() })));
            tpReqMonitor.dispatch(sub.connId, MsgReqMonitor{MsgReqMonitor::NewSub{std::move(sub)}});
        }
    };

    while(1) {
        auto newMsgs = queries.running.empty() ? thr.inbox.pop_all() : thr.inbox.pop_all_no_wait();

        auto txn = env.txn_ro();

        for (auto &newMsg : newMsgs) {
            if (auto msg = std::get_if<MsgReqWorker::NewSub>(&newMsg.msg)) {
                auto connId = msg->sub.connId;

                if (!queries.addSub(txn, std::move(msg->sub))) {
                    sendNoticeError(connId, std::string("too many concurrent REQs"));
                }

                queries.process(txn);
            } else if (auto msg = std::get_if<MsgReqWorker::SetAuth>(&newMsg.msg)) {
                connIdToAuthedPubkey[msg->connId] = msg->authed;
            } else if (auto msg = std::get_if<MsgReqWorker::RemoveSub>(&newMsg.msg)) {
                queries.removeSub(msg->connId, msg->subId);
                tpReqMonitor.dispatch(msg->connId, MsgReqMonitor{MsgReqMonitor::RemoveSub{msg->connId, msg->subId}});
            } else if (auto msg = std::get_if<MsgReqWorker::CloseConn>(&newMsg.msg)) {
                connIdToAuthedPubkey.erase(msg->connId);
                queries.closeConn(msg->connId);
                tpReqMonitor.dispatch(msg->connId, MsgReqMonitor{MsgReqMonitor::CloseConn{msg->connId}});
            }
        }

        queries.process(txn);

        txn.abort();
    }
}
