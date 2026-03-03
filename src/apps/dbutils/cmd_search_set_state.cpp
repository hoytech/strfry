#include <iostream>

#include <docopt.h>
#include "golpe.h"
#include "search/LmdbSearchProvider.h"


static const char USAGE[] =
R"(
    Manually set the search index progress record.

    Usage:
      search-set-state --lev-id=<n> [--index-version=<n>] [--allow-lower] [--in-progress]

    Options:
      --lev-id=<n>         LevId to record as last indexed (required)
      --index-version=<n>  Index version flag to store (defaults to provider version)
      --allow-lower        Allow decreasing the stored levId
      --in-progress        Shortcut for --index-version=0 (resume mode)
)";


void cmd_search_set_state(const std::vector<std::string> &subArgs) {
    std::map<std::string, docopt::value> args = docopt::docopt(USAGE, subArgs, true, "");

    if (!cfg().relay__search__enabled) {
        std::cerr << "Error: Search is not enabled (relay.search.enabled = false)\n";
        return;
    }

    if (cfg().relay__search__backend != "lmdb") {
        std::cerr << "Error: LMDB search backend not configured (relay.search.backend != \"lmdb\")\n";
        return;
    }

    try {
        auto txn = env.txn_ro();
        auto cursorIndex = lmdb::cursor::open(txn, env.dbi_SearchIndex);
        auto cursorDocMeta = lmdb::cursor::open(txn, env.dbi_SearchDocMeta);
        (void)cursorIndex;
        (void)cursorDocMeta;
    } catch (const std::exception &e) {
        std::cerr << "Error: Search tables are not initialized in this database: " << e.what() << "\n";
        return;
    }

    if (!args["--lev-id"]) {
        throw herr("missing required --lev-id argument");
    }

    uint64_t levId = 0;
    try {
        levId = std::stoull(args["--lev-id"].asString());
    } catch (...) {
        throw herr("invalid --lev-id value");
    }

    bool markInProgress = args["--in-progress"].asBool();
    uint64_t indexVersion = LmdbSearchProvider::kIndexVersion;

    if (args["--index-version"]) {
        try {
            indexVersion = std::stoull(args["--index-version"].asString());
        } catch (...) {
            throw herr("invalid --index-version value");
        }
    }

    if (markInProgress) {
        if (args["--index-version"] && indexVersion != 0) {
            throw herr("--in-progress conflicts with explicit --index-version");
        }
        indexVersion = 0;
    }

    if (levId == 0) {
        throw herr("--lev-id must be >= 1");
    }

    bool allowLower = args["--allow-lower"].asBool();

    auto txn = lmdb::txn::begin(env.lmdb_env);

    uint64_t oldLevId = 0;
    uint64_t oldVersion = 0;

    auto stateView = env.lookup_SearchState(txn, 1);
    if (stateView) {
        oldLevId = stateView->lastIndexedLevId();
        oldVersion = stateView->indexVersion();

        if (!allowLower && levId < oldLevId) {
            throw herr("refusing to decrease lastIndexedLevId (have ", oldLevId, ", requested ", levId, "). Use --allow-lower to override.");
        }

        defaultDb::environment::Updates_SearchState upd;
        upd.lastIndexedLevId = levId;
        upd.indexVersion = indexVersion;
        env.update_SearchState(txn, *stateView, upd);
    } else {
        env.insert_SearchState(txn, levId, indexVersion);
    }

    txn.commit();

    std::cout << "SearchState updated.\n"
              << "  previous: levId=" << oldLevId << " indexVersion=" << oldVersion << "\n"
              << "  current:  levId=" << levId << " indexVersion=" << indexVersion << "\n";
}
