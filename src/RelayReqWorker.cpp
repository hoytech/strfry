#include "RelayServer.h"
#include "DBQuery.h"
#include "QueryScheduler.h"


void RelayServer::runReqWorker(ThreadPool<MsgReqWorker>::Thread &thr) {
    Decompressor decomp;
    QueryScheduler queries;

    queries.onEvent = [&](lmdb::txn &txn, const auto &sub, uint64_t levId, std::string_view eventPayload){
        sendEvent(sub.connId, sub.subId, decodeEventPayload(txn, decomp, eventPayload, nullptr, nullptr));
    };

    queries.onComplete = [&](Subscription &sub){
        sendToConn(sub.connId, tao::json::to_string(tao::json::value::array({ "EOSE", sub.subId.str() })));
        tpReqMonitor.dispatch(sub.connId, MsgReqMonitor{MsgReqMonitor::NewSub{std::move(sub)}});
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
            } else if (auto msg = std::get_if<MsgReqWorker::RemoveSub>(&newMsg.msg)) {
                queries.removeSub(msg->connId, msg->subId);
                tpReqMonitor.dispatch(msg->connId, MsgReqMonitor{MsgReqMonitor::RemoveSub{msg->connId, msg->subId}});
            } else if (auto msg = std::get_if<MsgReqWorker::CloseConn>(&newMsg.msg)) {
                queries.closeConn(msg->connId);
                tpReqMonitor.dispatch(msg->connId, MsgReqMonitor{MsgReqMonitor::CloseConn{msg->connId}});
            }
        }

        queries.process(txn);

        txn.abort();
    }
}
