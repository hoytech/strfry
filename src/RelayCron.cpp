#include <hoytech/timer.h>

#include "RelayServer.h"

#include "gc.h"


void RelayServer::runCron() {
    auto qdb = getQdbInstance();

    hoytech::timer cron;

    cron.setupCb = []{ setThreadName("cron"); };


    // Delete ephemeral events

    cron.repeat(10 * 1'000'000UL, [&]{
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
    });

    // Garbage collect quadrable nodes

    cron.repeat(60 * 60 * 1'000'000UL, [&]{
        quadrableGarbageCollect(qdb, 1);
    });

    cron.run();

    while (1) std::this_thread::sleep_for(std::chrono::seconds(1'000'000));
}
