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
      search_reindex [--batch-size=<n>] [--restart]

    Options:
      --batch-size=<n>  Number of events to index per batch [default: 1000]
      --restart         Discard any in-progress rebuild and start from levId 1
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
    uint64_t resumeFrom = 1;
    bool resuming = false;

    {
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

        // Clear SearchIndex
        auto indexCursor = lmdb::cursor::open(txn, env.dbi_SearchIndex);
        std::string_view key, val;
        if (indexCursor.get(key, val, MDB_FIRST)) {
            do {
                indexCursor.del();
            } while (indexCursor.get(key, val, MDB_NEXT_NODUP));
        }

        // Clear SearchDocMeta
        auto docMetaCursor = lmdb::cursor::open(txn, env.dbi_SearchDocMeta);
        if (docMetaCursor.get(key, val, MDB_FIRST)) {
            do {
                docMetaCursor.del();
            } while (docMetaCursor.get(key, val, MDB_NEXT));
        }

        // Mark rebuild as in-progress (indexVersion = 0)
        persistSearchState(txn, 0, 0);

        txn.commit();
        std::cout << "Existing index cleared.\n";
    } else {
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
        auto txn = lmdb::txn::begin(env.lmdb_env);
        persistSearchState(txn, totalEvents, LmdbSearchProvider::kIndexVersion);
        txn.commit();
        std::cout << "Index already up to date.\n";
        return;
    }

    // Re-index events from EventPayload
    uint64_t indexed = 0;
    uint64_t skipped = 0;
    Decompressor decomp;

    auto persistStandalone = [&](uint64_t levId) {
        auto txn = lmdb::txn::begin(env.lmdb_env);
        persistSearchState(txn, levId, 0);
        txn.commit();
    };

    for (uint64_t levId = resumeFrom; levId <= totalEvents; levId++) {
        try {
            auto txn = env.txn_ro();
            std::string_view eventPayload;

            if (!env.dbi_EventPayload.get(txn, lmdb::to_sv<uint64_t>(levId), eventPayload)) {
                skipped++;
                persistStandalone(levId);
                continue; // Event doesn't exist (sparse levId space)
            }

            // Decode event payload
            std::string_view json = decodeEventPayload(txn, decomp, eventPayload, nullptr, nullptr);

            // Parse event to get kind and created_at
            auto eventJson = tao::json::from_string(json);
            uint64_t kind = eventJson.at("kind").get_unsigned();
            uint64_t created_at = eventJson.at("created_at").get_unsigned();

            // Index the event and persist progress within the same transaction
            bool wrote = provider.indexEventWithTxnHook(levId, json, kind, created_at, [&, levId](lmdb::txn &wtxn) {
                persistSearchState(wtxn, levId, 0);
            });

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

    // Update SearchState to mark index as fully caught up
    {
        auto txn = lmdb::txn::begin(env.lmdb_env);
        persistSearchState(txn, totalEvents, LmdbSearchProvider::kIndexVersion);
        txn.commit();
    }

    std::cout << "\nSearch index rebuild complete!\n";
    std::cout << "Total events scanned: " << totalEvents << "\n";
    std::cout << "Events indexed: " << indexed << "\n";
    std::cout << "Events skipped: " << skipped << "\n";
}
