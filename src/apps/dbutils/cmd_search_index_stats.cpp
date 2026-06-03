#include <iostream>
#include <iomanip>
#include <sstream>

#include <docopt.h>
#include "golpe.h"
#include "search/LmdbSearchProvider.h"


static const char USAGE[] =
R"(
    Display LMDB stats for the search index tables.

    Usage:
      search_index_stats
)";

static std::string humanSize(uint64_t bytes) {
    const char *suffixes[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    size_t idx = 0;
    double count = static_cast<double>(bytes);

    while (count >= 1024.0 && idx + 1 < std::size(suffixes)) {
        count /= 1024.0;
        idx++;
    }

    std::ostringstream oss;
    oss << std::fixed << std::setprecision(idx == 0 ? 0 : 2) << count << ' ' << suffixes[idx];
    return oss.str();
}

static void printStat(const std::string &name, const MDB_stat &st) {
    uint64_t totalPages = st.ms_branch_pages + st.ms_leaf_pages + st.ms_overflow_pages;
    uint64_t approxBytes = totalPages * st.ms_psize;

    std::cout << name << ":\n"
              << "  entries        : " << st.ms_entries << "\n"
              << "  depth          : " << st.ms_depth << "\n"
              << "  branch pages   : " << st.ms_branch_pages << "\n"
              << "  leaf pages     : " << st.ms_leaf_pages << "\n"
              << "  overflow pages : " << st.ms_overflow_pages << "\n"
              << "  page size      : " << st.ms_psize << " bytes\n"
              << "  approx size    : " << approxBytes << " bytes (" << humanSize(approxBytes) << ")\n";
}

void cmd_search_index_stats(const std::vector<std::string> &subArgs) {
    std::map<std::string, docopt::value> args = docopt::docopt(USAGE, subArgs, true, "");
    (void)args;

    if (!cfg().relay__search__enabled) {
        std::cerr << "Error: Search is not enabled (relay.search.enabled = false)\n";
        return;
    }

    if (cfg().relay__search__backend != "lmdb") {
        std::cerr << "Error: LMDB search backend not configured (relay.search.backend != \"lmdb\")\n";
        return;
    }

    auto txn = env.txn_ro();

    MDB_stat indexStat{};
    MDB_stat docMetaStat{};
    bool statsOk = true;

    try {
        indexStat = env.dbi_SearchIndex.stat(txn);
    } catch (const std::exception &e) {
        std::cerr << "Error: Unable to open SearchIndex table: " << e.what() << "\n";
        statsOk = false;
    }

    try {
        docMetaStat = env.dbi_SearchDocMeta.stat(txn);
    } catch (const std::exception &e) {
        std::cerr << "Error: Unable to open SearchDocMeta table: " << e.what() << "\n";
        statsOk = false;
    }

    if (!statsOk) return;

    std::cout << "Search index LMDB statistics:\n";
    printStat("  SearchIndex", indexStat);
    printStat("  SearchDocMeta", docMetaStat);

    auto stateView = env.lookup_SearchState(txn, 1);
    if (stateView) {
        std::cout << "SearchState:\n"
                  << "  lastIndexedLevId : " << stateView->lastIndexedLevId() << "\n"
                  << "  indexVersion     : " << stateView->indexVersion() << "\n";
    } else {
        std::cout << "SearchState: not initialized\n";
    }
}
