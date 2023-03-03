#pragma once

#include "DBQuery.h"


struct QueryScheduler : NonCopyable {
    std::function<void(lmdb::txn &txn, const Subscription &sub, uint64_t levId, std::string_view eventPayload)> onEvent;
    std::function<void(lmdb::txn &txn, const Subscription &sub, const std::vector<uint64_t> &levIds)> onEventBatch;
    std::function<void(Subscription &sub)> onComplete;

    using ConnQueries = flat_hash_map<SubId, DBQuery*>;
    flat_hash_map<uint64_t, ConnQueries> conns; // connId -> subId -> DBQuery*
    std::deque<DBQuery*> running;
    std::vector<uint64_t> levIdBatch;

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

    void process(lmdb::txn &txn) {
        if (running.empty()) return;

        DBQuery *q = running.front();
        running.pop_front();

        if (q->dead) {
            delete q;
            return;
        }

        bool complete = q->process(txn, [&](const auto &sub, uint64_t levId, std::string_view eventPayload){
            if (onEvent) onEvent(txn, sub, levId, eventPayload);
            if (onEventBatch) levIdBatch.push_back(levId);
        }, cfg().relay__queryTimesliceBudgetMicroseconds, cfg().relay__logging__dbScanPerf);

        if (onEventBatch) {
            onEventBatch(txn, q->sub, levIdBatch);
            levIdBatch.clear();
        }

        if (complete) {
            auto connId = q->sub.connId;
            removeSub(connId, q->sub.subId);

            if (onComplete) onComplete(q->sub);

            delete q;
        } else {
            running.push_back(q);
        }
    }
};
