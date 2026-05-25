#include <iostream>

#include <docopt.h>
#include "golpe.h"
#include "events.h"
#include "Decompressor.h"
#include "search/LmdbSearchProvider.h"
#include <tao/json.hpp>


static const char USAGE[] =
R"(
    Rebuild search index from scratch.
    Drops existing search index and re-indexes all events from EventPayload.

    Usage:
      search_reindex [--batch-size=<n>] [--restart] [--from-levid=<n>]

    Options:
      --batch-size=<n>  Number of events to index per batch [default: 1000]
      --restart         Discard any in-progress rebuild and start from levId 1
      --from-levid=<n>  Re-index from levId <n> onward without clearing the
                        existing index and without updating SearchState.
                        Use for validating fixes against known-bad ranges
                        without a full rebuild. Mutually exclusive with --restart.
)";


void cmd_search_reindex(const std::vector<std::string> &subArgs) {
    std::map<std::string, docopt::value> args = docopt::docopt(USAGE, subArgs, true, "");

    uint64_t batchSize = args["--batch-size"].asLong();

    std::cout << "Rebuilding search index...\n";

    // Check if search is enabled
    if (!cfg().relay__search__enabled) {
        std::cerr << "Error: Search is not enabled (relay.search.enabled = false)\n";
        return;
    }

    if (cfg().relay__search__backend != "lmdb") {
        std::cerr << "Error: LMDB search backend not configured (relay.search.backend != \"lmdb\")\n";
        return;
    }

    // Create LMDB search provider
    LmdbSearchProvider provider;

    auto persistSearchState = [](lmdb::txn &txn, uint64_t lastIndexedLevId, uint64_t indexVersion) {
        auto stateView = env.lookup_SearchState(txn, 1);
        if (!stateView) {
            env.insert_SearchState(txn, lastIndexedLevId, indexVersion);
            return;
        }

        defaultDb::environment::Updates_SearchState upd;
        upd.lastIndexedLevId = lastIndexedLevId;
        if (stateView->indexVersion() != indexVersion) {
            upd.indexVersion = indexVersion;
        }
        env.update_SearchState(txn, *stateView, upd);
    };

    bool restart = args["--restart"].asBool();
    bool partial = static_cast<bool>(args["--from-levid"]);

    if (restart && partial) {
        std::cerr << "Error: --restart and --from-levid are mutually exclusive\n";
        return;
    }

    // Stale-index guard: if the on-disk indexVersion doesn't match the binary's
    // kIndexVersion and SearchIndex has any data, the storage format is
    // incompatible (e.g. v1 host-endian postings vs. v2 big-endian). Mixing
    // formats would silently corrupt the dup tree. Force the operator to
    // re-run with --restart, which clears the index before rebuilding.
    if (!restart) {
        auto txn = env.txn_ro();
        auto stateView = env.lookup_SearchState(txn, 1);
        if (stateView && stateView->indexVersion() != 0 &&
            stateView->indexVersion() != LmdbSearchProvider::kIndexVersion) {
            std::cerr << "Error: search index is stale (stored version "
                      << stateView->indexVersion() << ", expected "
                      << LmdbSearchProvider::kIndexVersion
                      << "). Re-run with --restart to clear and rebuild.\n";
            return;
        }
        // indexVersion == 0 indicates an in-progress rebuild from a previous
        // strfry version; if any SearchIndex data exists, it's in the old
        // format and must be cleared.
        if (stateView && stateView->indexVersion() == 0) {
            auto cursor = lmdb::cursor::open(txn, env.dbi_SearchIndex);
            std::string_view k, v;
            if (cursor.get(k, v, MDB_FIRST)) {
                std::cerr << "Error: an in-progress rebuild from a previous "
                          << "strfry version was found. Re-run with --restart "
                          << "to clear and rebuild.\n";
                return;
            }
        }
    }

    uint64_t resumeFrom = 1;
    bool resuming = false;
    bool persistState = !partial;

    if (partial) {
        long fromLevId = args["--from-levid"].asLong();
        if (fromLevId < 1) {
            std::cerr << "Error: --from-levid must be >= 1\n";
            return;
        }
        resumeFrom = static_cast<uint64_t>(fromLevId);
        resuming = true; // skip the clear-index branch below
        std::cout << "Partial reindex requested: starting from levId " << resumeFrom
                  << " (SearchState will not be updated)\n";
    } else {
        auto txn = env.txn_ro();
        auto stateView = env.lookup_SearchState(txn, 1);
        if (stateView && !restart && stateView->indexVersion() == 0) {
            resuming = true;
            uint64_t last = stateView->lastIndexedLevId();
            resumeFrom = last < std::numeric_limits<uint64_t>::max() ? last + 1 : last;
            if (resumeFrom == 0) resumeFrom = 1;
        }
    }

    if (!resuming) {
        std::cout << "Clearing existing search index...\n";
        auto txn = lmdb::txn::begin(env.lmdb_env);

        // Empty SearchIndex and SearchDocMeta wholesale via mdb_drop(del=0)
        // — keeps the DBI handles, empties the data. The previous
        // cursor.del() loops only deleted one duplicate per key, leaving
        // most of the DUPSORT data behind.
        env.dbi_SearchIndex.drop(txn, false);
        env.dbi_SearchDocMeta.drop(txn, false);

        // Mark rebuild as in-progress (indexVersion = 0)
        persistSearchState(txn, 0, 0);

        txn.commit();
        std::cout << "Existing index cleared.\n";
    } else if (!partial) {
        std::cout << "Resuming search index rebuild from levId " << resumeFrom << "...\n";
    }

    // Get total number of events to index
    uint64_t totalEvents = 0;
    {
        auto txn = env.txn_ro();
        totalEvents = env.get_largest_integer_key_or_zero(txn, env.dbi_EventPayload);
        std::cout << "Total events to scan: " << totalEvents << "\n";
    }

    if (resumeFrom > totalEvents) {
        if (persistState) {
            auto txn = lmdb::txn::begin(env.lmdb_env);
            persistSearchState(txn, totalEvents, LmdbSearchProvider::kIndexVersion);
            txn.commit();
            std::cout << "Index already up to date.\n";
        } else {
            std::cout << "Nothing to do: --from-levid=" << resumeFrom
                      << " is past the largest event (" << totalEvents << ").\n";
        }
        return;
    }

    // Re-index events from EventPayload
    uint64_t indexed = 0;
    uint64_t skipped = 0;
    Decompressor decomp;

    auto persistStandalone = [&](uint64_t levId) {
        if (!persistState) return;
        auto txn = lmdb::txn::begin(env.lmdb_env);
        persistSearchState(txn, levId, 0);
        txn.commit();
    };

    for (uint64_t levId = resumeFrom; levId <= totalEvents; levId++) {
        try {
            // Read event payload and copy decoded JSON to an owned string before
            // closing the read transaction. The decoded view from decodeEventPayload()
            // is only valid until the next txn close or decompressor reuse, and LMDB
            // does not allow a read txn and a write txn on the same thread simultaneously
            // (without MDB_NOTLS). We must close the read txn before indexEventWithTxnHook
            // opens its write txn.
            std::string json;
            {
                auto txn = env.txn_ro();
                std::string_view eventPayload;

                if (!env.dbi_EventPayload.get(txn, lmdb::to_sv<uint64_t>(levId), eventPayload)) {
                    skipped++;
                    persistStandalone(levId);
                    continue; // Event doesn't exist (sparse levId space)
                }

                // Decode and immediately copy: view is invalid after txn closes.
                std::string_view jsonView = decodeEventPayload(txn, decomp, eventPayload, nullptr, nullptr);
                json.assign(jsonView.data(), jsonView.size());
            } // txn closes here — before any write txn is opened

            // Parse event to get kind and created_at
            auto eventJson = tao::json::from_string(json);
            uint64_t kind = eventJson.at("kind").get_unsigned();
            uint64_t created_at = eventJson.at("created_at").get_unsigned();

            // Index the event and (when not in --from-levid partial mode) persist
            // progress within the same transaction. In partial mode we deliberately
            // skip the SearchState update so a real in-progress catch-up is not
            // overwritten by the partial validation run.
            bool wrote = persistState
                ? provider.indexEventWithTxnHook(levId, json, kind, created_at, [&, levId](lmdb::txn &wtxn) {
                      persistSearchState(wtxn, levId, 0);
                  })
                : provider.indexEventWithTxnHook(levId, json, kind, created_at, [](lmdb::txn &) {});

            if (wrote) {
                indexed++;
            } else {
                skipped++;
                persistStandalone(levId);
            }

        } catch (std::exception &e) {
            std::cerr << "Warning: Failed to index event levId=" << levId << ": " << e.what() << "\n";
            skipped++;
            persistStandalone(levId);
        }

        if (((levId - resumeFrom + 1) % batchSize) == 0) {
            std::cout << "Progress: " << levId << "/" << totalEvents
                      << " events scanned, " << indexed << " indexed, "
                      << skipped << " skipped\n";
        }
    }

    // Update SearchState to mark index as fully caught up — skipped in
    // --from-levid partial mode since we don't claim authoritative progress.
    if (persistState) {
        auto txn = lmdb::txn::begin(env.lmdb_env);
        persistSearchState(txn, totalEvents, LmdbSearchProvider::kIndexVersion);
        txn.commit();
    }

    if (persistState) {
        std::cout << "\nSearch index rebuild complete!\n";
        std::cout << "Total events scanned: " << totalEvents << "\n";
    } else {
        std::cout << "\nPartial reindex complete (SearchState unchanged).\n";
        std::cout << "Range scanned: levId " << resumeFrom << " to " << totalEvents << "\n";
    }
    std::cout << "Events indexed: " << indexed << "\n";
    std::cout << "Events skipped: " << skipped << "\n";
}
