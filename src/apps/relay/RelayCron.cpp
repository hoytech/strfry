#include <hoytech/timer.h>

#include "RelayServer.h"


void RelayServer::runCron() {
    hoytech::timer cron;

    cron.setupCb = []{ setThreadName("cron"); };


    // Delete expired events

    cron.repeat(9 * 1'000'000UL, [&]{
        std::vector<uint64_t> expiredLevIds;
        uint64_t numEphemeral = 0;
        uint64_t numExpired = 0;

        {
            auto txn = env.txn_ro();

            auto mostRecent = getMostRecentLevId(txn);
            uint64_t now = hoytech::curr_time_s();
            uint64_t ephemeralCutoff = now - cfg().events__ephemeralEventsLifetimeSeconds;

            env.generic_foreachFull(txn, env.dbi_Event__expiration, lmdb::to_sv<uint64_t>(0), lmdb::to_sv<uint64_t>(0), [&](auto k, auto v) {
                auto expiration = lmdb::from_sv<uint64_t>(k);
                auto levId =  lmdb::from_sv<uint64_t>(v);

                if (expiration > now) return false;
                if (levId == mostRecent) return true; // don't delete because it could cause levId re-use

                if (expiration == 1) { // Ephemeral event
                    auto view = env.lookup_Event(txn, levId);
                    if (!view) throw herr("missing event from index, corrupt DB?");
                    uint64_t created = PackedEventView(view->buf).created_at();

                    if (created <= ephemeralCutoff) {
                        numEphemeral++;
                        expiredLevIds.emplace_back(levId);
                    }
                } else {
                    numExpired++;
                    expiredLevIds.emplace_back(levId);
                }

                return true;
            });
        }

        if (expiredLevIds.size() > 0) {
            auto txn = env.txn_rw();
            NegentropyFilterCache neFilterCache;

            uint64_t numDeleted = deleteEvents(txn, neFilterCache, expiredLevIds);

            txn.commit();

            if (numDeleted) LI << "Deleted " << numDeleted << " events (ephemeral=" << numEphemeral << " expired=" << numExpired << ")";
        }
    });



    // NIP-62: Delete events for vanished pubkeys

    cron.repeat(30 * 1'000'000UL, [&]{
        if (!cfg().relay__nip62__enabled) return;

        struct VanishEntry {
            std::string pubkey;
            uint64_t vanishTs;
        };

        std::vector<VanishEntry> vanishEntries;
        std::vector<uint64_t> vanishLevIds;
        const uint64_t batchLimit = 10000;

        {
            auto txn = env.txn_ro();

            // Collect all vanish entries
            {
                auto cursor = lmdb::cursor::open(txn, env.dbi_VanishPubkey);
                std::string_view k, v;
                if (cursor.get(k, v, MDB_FIRST)) {
                    do {
                        if (k.size() == 32 && v.size() == sizeof(uint64_t)) {
                            vanishEntries.push_back({std::string(k), lmdb::from_sv<uint64_t>(v)});
                        }
                    } while (cursor.get(k, v, MDB_NEXT));
                }
            }

            for (auto &entry : vanishEntries) {
                // Scan pubkey index for events authored by this pubkey
                auto searchPrefix = std::string(entry.pubkey);
                auto startKey = makeKey_StringUint64(searchPrefix, 0);

                env.generic_foreachFull(txn, env.dbi_Event__pubkey, startKey, lmdb::to_sv<uint64_t>(0), [&](auto k, auto v) {
                    if (!k.starts_with(searchPrefix)) return false;

                    auto levId = lmdb::from_sv<uint64_t>(v);
                    auto ev = env.lookup_Event(txn, levId);
                    if (!ev) return true;

                    PackedEventView packed(ev->buf);

                    // Skip kind 62 events (preserve bookkeeping)
                    if (packed.kind() == 62) return true;

                    if (packed.created_at() <= entry.vanishTs) {
                        vanishLevIds.push_back(levId);
                        if (vanishLevIds.size() >= batchLimit) {
                            return false;
                        }
                    }

                    return true;
                });

                // Scan tag index for gift wraps (kind 1059) addressed to this pubkey
                if (vanishLevIds.size() < batchLimit) {
                    auto tagPrefix = std::string("p") + entry.pubkey;
                    auto tagStartKey = makeKey_StringUint64(tagPrefix, 0);

                    env.generic_foreachFull(txn, env.dbi_Event__tag, tagStartKey, lmdb::to_sv<uint64_t>(0), [&](auto k, auto v) {
                        if (k.size() != tagPrefix.size() + 8 || !k.starts_with(tagPrefix)) return false;

                        auto levId = lmdb::from_sv<uint64_t>(v);
                        auto ev = env.lookup_Event(txn, levId);
                        if (!ev) return true;

                        PackedEventView packed(ev->buf);

                        // Delete all gift wraps (kind 1059) addressed to this pubkey (spec says ALL, no timestamp qualifier)
                        if (packed.kind() == 1059) {
                            vanishLevIds.push_back(levId);
                            if (vanishLevIds.size() >= batchLimit) {
                                return false;
                            }
                        }

                        return true;
                    });
                }

            }
        }

        if (vanishLevIds.size() > 0) {
            auto txn = env.txn_rw();
            NegentropyFilterCache neFilterCache;

            uint64_t numDeleted = deleteEvents(txn, neFilterCache, vanishLevIds);

            txn.commit();

            if (numDeleted) LI << "NIP-62 vanish: deleted " << numDeleted << " events";
        }
    });


    cron.run();

    while (1) std::this_thread::sleep_for(std::chrono::seconds(1'000'000));
}
