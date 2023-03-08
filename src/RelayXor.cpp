#include "RelayServer.h"
#include "DBQuery.h"
#include "QueryScheduler.h"
#include "xor.h"


struct XorViews {
    struct UserView {
        XorView v;
        std::string initialQuery;
    };

    using ConnViews = flat_hash_map<SubId, UserView>;
    flat_hash_map<uint64_t, ConnViews> conns; // connId -> subId -> XorView

    bool addView(uint64_t connId, const SubId &subId, uint64_t idSize, const std::string &initialQuery) {
        {
            auto *existing = findView(connId, subId);
            if (existing) removeView(connId, subId);
        }

        auto res = conns.try_emplace(connId);
        auto &connViews = res.first->second;

        if (connViews.size() >= cfg().relay__maxSubsPerConnection) {
            return false;
        }

        connViews.try_emplace(subId, UserView{ XorView(idSize), initialQuery });

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


void RelayServer::runXor(ThreadPool<MsgXor>::Thread &thr) {
    QueryScheduler queries;
    XorViews views;

    queries.onEventBatch = [&](lmdb::txn &txn, const auto &sub, const std::vector<uint64_t> &levIds){
        auto *view = views.findView(sub.connId, sub.subId);
        if (!view) return;

        for (auto levId : levIds) {
            auto ev = lookupEventByLevId(txn, levId);
            view->v.addElem(ev.flat_nested()->created_at(), sv(ev.flat_nested()->id()).substr(0, view->v.idSize));
        }
    };

    queries.onComplete = [&](Subscription &sub){
        auto *view = views.findView(sub.connId, sub.subId);
        if (!view) return;

        view->v.finalise();

        std::vector<std::string> haveIds, needIds;
        auto resp = view->v.reconcile(view->initialQuery, haveIds, needIds);

        sendToConn(sub.connId, tao::json::to_string(tao::json::value::array({
            "XOR-RES",
            sub.subId.str(),
            to_hex(resp),
            // FIXME: haveIds
        })));

        view->initialQuery = "";
    };

    while(1) {
        auto newMsgs = queries.running.empty() ? thr.inbox.pop_all() : thr.inbox.pop_all_no_wait();

        auto txn = env.txn_ro();

        for (auto &newMsg : newMsgs) {
            if (auto msg = std::get_if<MsgXor::NewView>(&newMsg.msg)) {
                auto connId = msg->sub.connId;
                auto subId = msg->sub.subId;

                if (!queries.addSub(txn, std::move(msg->sub))) {
                    sendNoticeError(connId, std::string("too many concurrent REQs"));
                }

                if (!views.addView(connId, subId, msg->idSize, msg->query)) {
                    queries.removeSub(connId, subId);
                    sendNoticeError(connId, std::string("too many concurrent XORs"));
                }

                queries.process(txn);
            } else if (auto msg = std::get_if<MsgXor::QueryView>(&newMsg.msg)) {
                (void)msg;
                //...
            } else if (auto msg = std::get_if<MsgXor::RemoveView>(&newMsg.msg)) {
                queries.removeSub(msg->connId, msg->subId);
                views.removeView(msg->connId, msg->subId);
            } else if (auto msg = std::get_if<MsgXor::CloseConn>(&newMsg.msg)) {
                queries.closeConn(msg->connId);
                views.closeConn(msg->connId);
            }
        }

        queries.process(txn);

        txn.abort();
    }
}
