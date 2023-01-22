#include "RelayServer.h"
#include "DBScan.h"



struct ActiveQueries : NonCopyable {
    using ConnQueries = std::map<SubId, DBScanQuery*>;
    std::map<uint64_t, ConnQueries> conns; // connId -> subId -> DBScanQuery*
    std::deque<DBScanQuery*> running;

    void addSub(lmdb::txn &txn, Subscription &&sub) {
        sub.latestEventId = getMostRecentLevId(txn);

        {
            auto *existing = findQuery(sub.connId, sub.subId);
            if (existing) removeSub(sub.connId, sub.subId);
        }

        auto res = conns.try_emplace(sub.connId);
        auto &connQueries = res.first->second;

        DBScanQuery *q = new DBScanQuery(sub);

        connQueries.try_emplace(q->sub.subId, q);
        running.push_front(q);
    }

    DBScanQuery *findQuery(uint64_t connId, const SubId &subId) {
        auto f1 = conns.find(connId);
        if (f1 == conns.end()) return nullptr;

        auto f2 = f1->second.find(subId);
        if (f2 == f1->second.end()) return nullptr;

        return f2->second;
    }

    void removeSub(uint64_t connId, const SubId &subId) {
        auto *query = findQuery(connId, subId);
        if (!query) return;
        query->dead = true;
        conns[connId].erase(subId);
        if (conns[connId].empty()) conns.erase(connId);
    }

    void closeConn(uint64_t connId) {
        auto f1 = conns.find(connId);
        if (f1 == conns.end()) return;

        for (auto &[k, v] : f1->second) v->dead = true;

        conns.erase(connId);
    }

    void process(RelayServer *server, lmdb::txn &txn) {
        if (running.empty()) return;

        DBScanQuery *q = running.front();
        running.pop_front();

        if (q->dead) {
            delete q;
            return;
        }

        bool complete = q->process(txn, cfg().relay__queryTimesliceBudgetMicroseconds, cfg().relay__logging__dbScanPerf, [&](const auto &sub, uint64_t levId){
            server->sendEvent(sub.connId, sub.subId, getEventJson(txn, levId));
        });

        if (complete) {
            auto connId = q->sub.connId;

            server->sendToConn(connId, tao::json::to_string(tao::json::value::array({ "EOSE", q->sub.subId.str() })));
            removeSub(connId, q->sub.subId);

            server->tpReqMonitor.dispatch(connId, MsgReqMonitor{MsgReqMonitor::NewSub{std::move(q->sub)}});

            delete q;
        } else {
            running.push_back(q);
        }
    }
};


void RelayServer::runReqWorker(ThreadPool<MsgReqWorker>::Thread &thr) {
    ActiveQueries queries;

    while(1) {
        auto newMsgs = queries.running.empty() ? thr.inbox.pop_all() : thr.inbox.pop_all_no_wait();

        auto txn = env.txn_ro();

        for (auto &newMsg : newMsgs) {
            if (auto msg = std::get_if<MsgReqWorker::NewSub>(&newMsg.msg)) {
                queries.addSub(txn, std::move(msg->sub));
                queries.process(this, txn);
            } else if (auto msg = std::get_if<MsgReqWorker::RemoveSub>(&newMsg.msg)) {
                queries.removeSub(msg->connId, msg->subId);
                tpReqMonitor.dispatch(msg->connId, MsgReqMonitor{MsgReqMonitor::RemoveSub{msg->connId, msg->subId}});
            } else if (auto msg = std::get_if<MsgReqWorker::CloseConn>(&newMsg.msg)) {
                queries.closeConn(msg->connId);
                tpReqMonitor.dispatch(msg->connId, MsgReqMonitor{MsgReqMonitor::CloseConn{msg->connId}});
            }
        }

        queries.process(this, txn);

        txn.abort();
    }
}
