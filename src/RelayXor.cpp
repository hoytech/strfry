#include "RelayServer.h"
#include "DBQuery.h"
#include "QueryScheduler.h"
#include "transport.h"


struct XorViews {
    struct Elem {
        char data[5 * 8];

        Elem() {
            memset(data, '\0', sizeof(data));
        }

        Elem(uint64_t created, std::string_view id, uint64_t idSize) {
            memset(data, '\0', sizeof(data));
            data[3] = (created >> (4*8)) & 0xFF;
            data[4] = (created >> (3*8)) & 0xFF;
            data[5] = (created >> (2*8)) & 0xFF;
            data[6] = (created >> (1*8)) & 0xFF;
            data[7] = (created >> (0*8)) & 0xFF;
            memcpy(data + 8, id.data(), idSize);
        }

        Elem(std::string_view id) {
            memset(data, '\0', sizeof(data));
            memcpy(data + 3, id.data(), id.size());
        }

        std::string_view getCompare(uint64_t idSize) const {
            return std::string_view(data + 3, idSize + 5);
        }

        std::string_view getId(uint64_t idSize) const {
            return std::string_view(data + 8, idSize);
        }

        bool isZero() {
            uint64_t *ours = reinterpret_cast<uint64_t*>(data + 8);
            return ours[0] == 0 && ours[1] == 0 && ours[2] == 0 && ours[3] == 0;
        }

        void doXor(const Elem &other) {
            uint64_t *ours = reinterpret_cast<uint64_t*>(data + 8);
            const uint64_t *theirs = reinterpret_cast<const uint64_t*>(other.data + 8);

            ours[0] ^= theirs[0];
            ours[1] ^= theirs[1];
            ours[2] ^= theirs[2];
            ours[3] ^= theirs[3];
        }
    };

    struct View {
        uint64_t connId;
        SubId subId;
        uint64_t idSize;
        std::string initialQuery;

        std::vector<Elem> elems;
        bool ready = false;

        View(uint64_t connId, SubId subId, uint64_t idSize, const std::string &initialQuery) : connId(connId), subId(subId), idSize(idSize), initialQuery(initialQuery) {
            if (idSize < 8 || idSize > 32) throw herr("idSize out of range");
        }

        void addElem(uint64_t createdAt, std::string_view id) {
            elems.emplace_back(createdAt, id, idSize);
        }

        void finalise() {
            std::reverse(elems.begin(), elems.end()); // pushed in approximately descending order, so hopefully this speeds up the sort

            std::sort(elems.begin(), elems.end(), [&](const auto &a, const auto &b) { return a.getCompare(idSize) < b.getCompare(idSize); });

            ready = true;

            handleQuery(initialQuery);
            initialQuery = "";
        }

        void handleQuery(std::string_view query) { // FIXME: this can throw
            std::string output;
            std::vector<Elem> idsToSend;

            auto cmp = [&](const auto &a, const auto &b){ return a.getCompare(idSize) < b.getCompare(idSize); };

            while (query.size()) {
                uint64_t lowerLength = decodeVarInt(query);
                if (lowerLength > idSize + 5) throw herr("lower too long");
                Elem lowerKey(getBytes(query, lowerLength));

                uint64_t upperLength = decodeVarInt(query);
                if (upperLength > idSize + 5) throw herr("upper too long");
                Elem upperKey(getBytes(query, upperLength));

                std::string xorSet = getBytes(query, idSize);

                auto lower = std::lower_bound(elems.begin(), elems.end(), lowerKey, cmp);
                auto upper = std::upper_bound(elems.begin(), elems.end(), upperKey, cmp);

                Elem myXorSet;
                for (auto i = lower; i < upper; ++i) myXorSet.doXor(*i);
            }

            /*
            sendToConn(sub.connId, tao::json::to_string(tao::json::value::array({
                "XOR-RES",
                sub.subId.str(),
                to_hex(view->xorRange(0, view->elems.size())),
                view->elems.size(),
            })));
            */
        }

        std::string xorRange(uint64_t start, uint64_t len) {
            Elem output;

            for (uint64_t i = 0; i < len; i++) {
                output.doXor(elems[i]);
            }

            return std::string(output.getId(idSize));
        }
    };

    using ConnViews = flat_hash_map<SubId, View>;
    flat_hash_map<uint64_t, ConnViews> conns; // connId -> subId -> View

    bool addView(uint64_t connId, const SubId &subId, uint64_t idSize, const std::string &query) {
        {
            auto *existing = findView(connId, subId);
            if (existing) removeView(connId, subId);
        }

        auto res = conns.try_emplace(connId);
        auto &connViews = res.first->second;

        if (connViews.size() >= cfg().relay__maxSubsPerConnection) {
            return false;
        }

        connViews.try_emplace(subId, connId, subId, idSize, query);

        return true;
    }

    View *findView(uint64_t connId, const SubId &subId) {
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
            view->addElem(ev.flat_nested()->created_at(), sv(ev.flat_nested()->id()).substr(0, view->idSize));
        }
    };

    queries.onComplete = [&](Subscription &sub){
        auto *view = views.findView(sub.connId, sub.subId);
        if (!view) return;

        view->finalise();
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
