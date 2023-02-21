#include "RelayServer.h"
#include "DBQuery.h"



struct ActiveQueries : NonCopyable {
    Decompressor decomp;
    using ConnQueries = flat_hash_map<SubId, DBQuery*>;
    flat_hash_map<uint64_t, ConnQueries> conns; // connId -> subId -> DBQuery*
    std::deque<DBQuery*> running;

    bool addSub(lmdb::txn &txn, Subscription &&sub) {
        sub.latestEventId = getMostRecentLevId(txn);

        {
            auto *existing = findQuery(sub.connId, sub.subId);
            if (existing) removeSub(sub.connId, sub.subId);
        }

        auto res = conns.try_emplace(sub.connId);
        auto &connQueries = res.first->second;

        if (connQueries.size() >= cfg().relay__maxSubsPerConnection) {
            return false;
        }

        DBQuery *q = new DBQuery(sub);

        connQueries.try_emplace(q->sub.subId, q);
        running.push_front(q);

        return true;
    }

    DBQuery *findQuery(uint64_t connId, const SubId &subId) {
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

        DBQuery *q = running.front();
        running.pop_front();

        if (q->dead) {
            delete q;
            return;
        }

        auto cursor = lmdb::cursor::open(txn, env.dbi_EventPayload);

        bool complete = q->process(txn, [&](const auto &sub, uint64_t levId){
            std::string_view key = lmdb::to_sv<uint64_t>(levId), val;
            if (!cursor.get(key, val, MDB_SET_KEY)) throw herr("couldn't find event in EventPayload, corrupted DB?");
            server->sendEvent(sub.connId, sub.subId, decodeEventPayload(txn, decomp, val, nullptr, nullptr));
        }, cfg().relay__queryTimesliceBudgetMicroseconds, cfg().relay__logging__dbScanPerf);

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
                auto connId = msg->sub.connId;

                if (!queries.addSub(txn, std::move(msg->sub))) {
                    sendNoticeError(connId, std::string("too many concurrent REQs"));
                }

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
