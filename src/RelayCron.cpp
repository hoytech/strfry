#include "RelayServer.h"

#include "gc.h"


void RelayServer::cleanupOldEvents() {
    std::vector<uint64_t> expiredLevIds;

    {
        auto txn = env.txn_ro();

        auto mostRecent = getMostRecentLevId(txn);
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

                uint64_t levId = lmdb::from_sv<uint64_t>(v);

                if (levId != mostRecent) { // prevent levId re-use
                    expiredLevIds.emplace_back(levId);
                }

                return true;
            });

            if (numRecs == 0) break;
        }
    }

    if (expiredLevIds.size() > 0) {
        auto qdb = getQdbInstance();

        auto txn = env.txn_rw();

        uint64_t numDeleted = 0;
        auto changes = qdb.change();

        for (auto levId : expiredLevIds) {
            auto view = env.lookup_Event(txn, levId);
            if (!view) continue; // Deleted in between transactions
            deleteEvent(txn, changes, *view);
            numDeleted++;
        }

        changes.apply(txn);

        txn.commit();

        if (numDeleted) LI << "Deleted " << numDeleted << " ephemeral events";
    }
}

void RelayServer::garbageCollect() {
    auto qdb = getQdbInstance();

    quadrableGarbageCollect(qdb, 1);
}
