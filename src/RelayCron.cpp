#include "RelayServer.h"


void RelayServer::cleanupOldEvents() {
    struct EventDel {
        uint64_t nodeId;
        uint64_t deletedNodeId;
    };

    std::vector<EventDel> expiredEvents;

    {
        auto txn = env.txn_ro();

        auto mostRecent = getMostRecentEventId(txn);
        uint64_t cutoff = hoytech::curr_time_s() - cfg().events__ephemeralEventsLifetimeSeconds;
        uint64_t currKind = 20'000;

        while (currKind < 30'000) {
            uint64_t numRecs = 0;

            env.generic_foreachFull(txn, env.dbi_Event__kind, makeKey_Uint64Uint64(currKind, 0), lmdb::to_sv<uint64_t>(0), [&](auto k, auto v) {
                numRecs++;
                ParsedKey_Uint64Uint64 parsedKey(k);
                currKind = parsedKey.n1;

                if (currKind >= 30'000) return false;

                if (parsedKey.n2 > cutoff) {
                    currKind++;
                    return false;
                }

                uint64_t nodeId = lmdb::from_sv<uint64_t>(v);

                if (nodeId != mostRecent) { // prevent nodeId re-use
                    expiredEvents.emplace_back(nodeId, 0);
                }

                return true;
            });

            if (numRecs == 0) break;
        }
    }

    if (expiredEvents.size() > 0) {
        LI << "Deleting " << expiredEvents.size() << " old events";

        auto txn = env.txn_rw();

        quadrable::Quadrable qdb;
        qdb.init(txn);
        qdb.checkout("events");

        auto changes = qdb.change();

        for (auto &e : expiredEvents) {
            auto view = env.lookup_Event(txn, e.nodeId);
            if (!view) throw herr("missing event from index, corrupt DB?");
            changes.del(flatEventToQuadrableKey(view->flat_nested()), &e.deletedNodeId);
        }

        changes.apply(txn);

        for (auto &e : expiredEvents) {
            if (e.deletedNodeId) env.delete_Event(txn, e.nodeId);
        }

        txn.commit();
    }
}
