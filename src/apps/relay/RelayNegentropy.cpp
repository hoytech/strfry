#include <negentropy.h>
#include <negentropy/storage/Vector.h>
#include <negentropy/storage/BTreeLMDB.h>

#include "RelayServer.h"
#include "QueryScheduler.h"


struct NegentropyViews {
    struct MemoryView {
        std::string initialMsg;
        negentropy::storage::Vector storageVector;
        std::vector<uint64_t> levIds;
        uint64_t startTime = hoytech::curr_time_us();
    };

    struct StatelessView {
        Subscription sub;
    };

    using UserView = std::variant<MemoryView, StatelessView>;

    using ConnViews = flat_hash_map<SubId, UserView>;
    flat_hash_map<uint64_t, ConnViews> conns; // connId -> subId -> UserView

    bool addMemoryView(uint64_t connId, const SubId &subId, const std::string &initialMsg) {
        {
            auto *existing = findView(connId, subId);
            if (existing) removeView(connId, subId);
        }

        auto res = conns.try_emplace(connId);
        auto &connViews = res.first->second;

        if (connViews.size() >= cfg().relay__maxSubsPerConnection) {
            return false;
        }

        connViews.try_emplace(subId, UserView{ MemoryView{ initialMsg, } });

        return true;
    }

    bool addStatelessView(uint64_t connId, const SubId &subId, Subscription &&sub) {
        {
            auto *existing = findView(connId, subId);
            if (existing) removeView(connId, subId);
        }

        auto res = conns.try_emplace(connId);
        auto &connViews = res.first->second;

        if (connViews.size() >= cfg().relay__maxSubsPerConnection) {
            return false;
        }

        connViews.try_emplace(subId, UserView{ StatelessView{ std::move(sub), } });

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


    auto handleReconcile = [&](uint64_t connId, const SubId &subId, negentropy::StorageBase &storage, const std::string &msg) {
        std::string resp;

        try {
            Negentropy ne(storage, 500'000);
            resp = ne.reconcile(msg);
        } catch (std::exception &e) {
            LI << "[" << connId << "] Error parsing negentropy initial message: " << e.what();

            sendToConn(connId, tao::json::to_string(tao::json::value::array({
                "NEG-ERR",
                subId.str(),
                "PROTOCOL-ERROR"
            })));

            views.removeView(connId, subId);
            return;
        }

        sendToConn(connId, tao::json::to_string(tao::json::value::array({
            "NEG-MSG",
            subId.str(),
            to_hex(resp)
        })));
    };


    queries.ensureExists = false;

    queries.onEventBatch = [&](lmdb::txn &txn, const auto &sub, const std::vector<uint64_t> &levIds){
        auto *userView = views.findView(sub.connId, sub.subId);
        if (!userView) return;

        auto *view = std::get_if<NegentropyViews::MemoryView>(userView);
        if (!view) throw herr("bad variant, expected memory view");

        for (auto levId : levIds) {
            view->levIds.push_back(levId);
        }
    };

    queries.onComplete = [&](lmdb::txn &txn, Subscription &sub){
        auto *userView = views.findView(sub.connId, sub.subId);
        if (!userView) return;

        auto *view = std::get_if<NegentropyViews::MemoryView>(userView);
        if (!view) throw herr("bad variant, expected memory view");

        LI << "[" << sub.connId << "] Negentropy query matched " << view->levIds.size() << " events in "
           << (hoytech::curr_time_us() - view->startTime) << "us";

        if (view->levIds.size() > cfg().relay__negentropy__maxSyncEvents) {
            LI << "[" << sub.connId << "] Negentropy query size exceeded " << cfg().relay__negentropy__maxSyncEvents;

            sendToConn(sub.connId, tao::json::to_string(tao::json::value::array({
                "NEG-ERR",
                sub.subId.str(),
                "RESULTS_TOO_BIG",
                cfg().relay__negentropy__maxSyncEvents
            })));

            views.removeView(sub.connId, sub.subId);
            return;
        }

        std::sort(view->levIds.begin(), view->levIds.end());

        for (auto levId : view->levIds) {
            try {
                auto ev = lookupEventByLevId(txn, levId);
                view->storageVector.insert(packed.created_at(), packed.id().substr(0, view->ne.idSize));
            } catch (std::exception &) {
                // levId was deleted when query was paused
            }
        }

        view->levIds.clear();
        view->levIds.shrink_to_fit();

        view->storageVector.seal();

        handleReconcile(sub.connId, sub.subId, view->storageVector, view->initialMsg);

        view->initialMsg = "";
    };


    while(1) {
        auto newMsgs = queries.running.empty() ? thr.inbox.pop_all() : thr.inbox.pop_all_no_wait();

        auto txn = env.txn_ro();

        for (auto &newMsg : newMsgs) {
            if (auto msg = std::get_if<MsgNegentropy::NegOpen>(&newMsg.msg)) {
                auto connId = msg->sub.connId;
                auto subId = msg->sub.subId;

                if (msg->sub.filterGroup.isFullDbQuery()) {
                    negentropy::storage::BTreeLMDB storage(txn, negentropyDbi, 0);
                    handleReconcile(connId, subId, storage, msg->negPayload);

                    if (!views.addStatelessView(connId, subId, std::move(msg->sub))) {
                        queries.removeSub(connId, subId);
                        sendNoticeError(connId, std::string("too many concurrent NEG requests"));
                    }
                } else {
                    if (!queries.addSub(txn, std::move(msg->sub))) {
                        sendNoticeError(connId, std::string("too many concurrent REQs"));
                    }

                    if (!views.addMemoryView(connId, subId, msg->negPayload)) {
                        queries.removeSub(connId, subId);
                        sendNoticeError(connId, std::string("too many concurrent NEG requests"));
                    }

                    queries.process(txn);
                }
            } else if (auto msg = std::get_if<MsgNegentropy::NegMsg>(&newMsg.msg)) {
                auto *userView = views.findView(msg->connId, msg->subId);
                if (!userView) {
                    sendToConn(msg->connId, tao::json::to_string(tao::json::value::array({
                        "NEG-ERR",
                        msg->subId.str(),
                        "CLOSED"
                    })));

                    continue;
                }

                if (auto *view = std::get_if<NegentropyViews::MemoryView>(userView)) {
                    if (!view->storageVector.sealed) {
                        sendNoticeError(msg->connId, "negentropy error: got NEG-MSG before NEG-OPEN complete");
                        continue;
                    }
                    handleReconcile(msg->connId, msg->subId, view->storageVector, msg->negPayload);
                } else if (std::get_if<NegentropyViews::StatelessView>(userView)) {
                    negentropy::storage::BTreeLMDB storage(txn, negentropyDbi, 0);
                    handleReconcile(msg->connId, msg->subId, storage, msg->negPayload);
                }
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
