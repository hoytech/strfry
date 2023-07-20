#include <Negentropy.h>

#include "RelayServer.h"
#include "DBQuery.h"
#include "QueryScheduler.h"


struct NegentropyViews {
    struct UserView {
        Negentropy ne;
        std::string initialMsg;
        uint64_t startTime = hoytech::curr_time_us();
    };

    using ConnViews = flat_hash_map<SubId, UserView>;
    flat_hash_map<uint64_t, ConnViews> conns; // connId -> subId -> Negentropy

    bool addView(uint64_t connId, const SubId &subId, uint64_t idSize, const std::string &initialMsg) {
        {
            auto *existing = findView(connId, subId);
            if (existing) removeView(connId, subId);
        }

        auto res = conns.try_emplace(connId);
        auto &connViews = res.first->second;

        if (connViews.size() >= cfg().relay__maxSubsPerConnection) {
            return false;
        }

        connViews.try_emplace(subId, UserView{ Negentropy(idSize), initialMsg });

        return true;
    }

    UserView *findView(uint64_t connId, const SubId &subId) {
        auto f1 = conns.find(connId);
        if (f1 == conns.end()) return nullptr;

        auto f2 = f1->second.find(subId);
        if (f2 == f1->second.end()) return nullptr;

        return &f2->second;
    }

    void removeView(uint64_t connId, const SubId &subId) {
        auto *view = findView(connId, subId);
        if (!view) return;
        conns[connId].erase(subId);
        if (conns[connId].empty()) conns.erase(connId);
    }

    void closeConn(uint64_t connId) {
        auto f1 = conns.find(connId);
        if (f1 == conns.end()) return;

        conns.erase(connId);
    }
};


void RelayServer::runNegentropy(ThreadPool<MsgNegentropy>::Thread &thr) {
    QueryScheduler queries;
    NegentropyViews views;

    queries.onEventBatch = [&](lmdb::txn &txn, const auto &sub, const std::vector<uint64_t> &levIds){
        auto *view = views.findView(sub.connId, sub.subId);
        if (!view) return;

        for (auto levId : levIds) {
            auto ev = lookupEventByLevId(txn, levId);
            view->ne.addItem(ev.flat_nested()->created_at(), sv(ev.flat_nested()->id()).substr(0, view->ne.idSize));
        }
    };

    queries.onComplete = [&](Subscription &sub){
        auto *view = views.findView(sub.connId, sub.subId);
        if (!view) return;

        LI << "[" << sub.connId << "] Negentropy query matched " << view->ne.items.size() << " events in "
           << (hoytech::curr_time_us() - view->startTime) << "us";

        if (view->ne.items.size() > cfg().relay__negentropy__maxSyncEvents) {
            LI << "[" << sub.connId << "] Negentropy query size exceeeded " << cfg().relay__negentropy__maxSyncEvents;

            sendToConn(sub.connId, tao::json::to_string(tao::json::value::array({
                "NEG-ERR",
                sub.subId.str(),
                "RESULTS_TOO_BIG",
                cfg().relay__negentropy__maxSyncEvents
            })));

            views.removeView(sub.connId, sub.subId);
            return;
        }

        view->ne.seal();

        auto resp = view->ne.reconcile(view->initialMsg);
        view->initialMsg = "";

        sendToConn(sub.connId, tao::json::to_string(tao::json::value::array({
            "NEG-MSG",
            sub.subId.str(),
            to_hex(resp)
        })));
    };

    while(1) {
        auto newMsgs = queries.running.empty() ? thr.inbox.pop_all() : thr.inbox.pop_all_no_wait();

        auto txn = env.txn_ro();

        for (auto &newMsg : newMsgs) {
            if (auto msg = std::get_if<MsgNegentropy::NegOpen>(&newMsg.msg)) {
                auto connId = msg->sub.connId;
                auto subId = msg->sub.subId;

                if (!queries.addSub(txn, std::move(msg->sub))) {
                    sendNoticeError(connId, std::string("too many concurrent REQs"));
                }

                if (!views.addView(connId, subId, msg->idSize, msg->negPayload)) {
                    queries.removeSub(connId, subId);
                    sendNoticeError(connId, std::string("too many concurrent NEG requests"));
                }

                queries.process(txn);
            } else if (auto msg = std::get_if<MsgNegentropy::NegMsg>(&newMsg.msg)) {
                auto *view = views.findView(msg->connId, msg->subId);
                if (!view) {
                    sendToConn(msg->connId, tao::json::to_string(tao::json::value::array({
                        "NEG-ERR",
                        msg->subId.str(),
                        "CLOSED"
                    })));

                    return;
                }

                if (!view->ne.sealed) {
                    sendNoticeError(msg->connId, "negentropy error: got NEG-MSG before NEG-OPEN complete");
                    return;
                }

                auto resp = view->ne.reconcile(msg->negPayload);

                sendToConn(msg->connId, tao::json::to_string(tao::json::value::array({
                    "NEG-MSG",
                    msg->subId.str(),
                    to_hex(resp)
                })));
            } else if (auto msg = std::get_if<MsgNegentropy::NegClose>(&newMsg.msg)) {
                queries.removeSub(msg->connId, msg->subId);
                views.removeView(msg->connId, msg->subId);
            } else if (auto msg = std::get_if<MsgNegentropy::CloseConn>(&newMsg.msg)) {
                queries.closeConn(msg->connId);
                views.closeConn(msg->connId);
            }
        }

        queries.process(txn);

        txn.abort();
    }
}
