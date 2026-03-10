#include <iostream>

#include <docopt.h>
#include "golpe.h"

#include "DBQuery.h"
#include "events.h"
#include "search/SearchProvider.h"


static const char USAGE[] =
R"(
    Usage:
      delete [--age=<age>] [--filter=<filter>] [--dry-run]
)";


void cmd_delete(const std::vector<std::string> &subArgs) {
    std::map<std::string, docopt::value> args = docopt::docopt(USAGE, subArgs, true, "");

    uint64_t age = MAX_U64;
    if (args["--age"]) age = args["--age"].asLong();

    std::string filterStr;
    if (args["--filter"]) filterStr = args["--filter"].asString();

    bool dryRun = args["--dry-run"].asBool();



    if (filterStr.size() == 0 && age == MAX_U64) throw herr("must specify --age and/or --filter");
    if (filterStr.size() == 0) filterStr = "{}";


    auto filter = tao::json::from_string(filterStr);
    auto now = hoytech::curr_time_s();

    if (age != MAX_U64) {
        if (age > now) age = now;
        if (filter.optional<uint64_t>("until")) throw herr("--age is not compatible with filter containing 'until'");

        filter["until"] = now - age;
    }


    DBQuery query(filter);

    btree_set<uint64_t> levIds;

    {
        auto txn = env.txn_ro();

        while (1) {
            bool complete = query.process(txn, [&](const auto &sub, uint64_t levId){
                levIds.insert(levId);
            });

            if (complete) break;
        }
    }

    if (dryRun) {
        LI << "Would delete " << levIds.size() << " events";
        return;
    }


    LI << "Deleting " << levIds.size() << " events";

    {
        auto txn = env.txn_rw();
        NegentropyFilterCache neFilterCache;

        deleteEvents(txn, neFilterCache, levIds);

        txn.commit();
    }

    // Remove from search index if search is enabled
    if (cfg().relay__search__enabled) {
        auto searchProvider = makeSearchProvider();
        if (searchProvider && searchProvider->healthy()) {
            LI << "Removing " << levIds.size() << " events from search index";
            uint64_t removed = 0;
            for (uint64_t levId : levIds) {
                try {
                    searchProvider->deleteEvent(levId);
                    removed++;
                } catch (std::exception &e) {
                    LE << "Failed to remove event from search index levId=" << levId << ": " << e.what();
                }
            }
            LI << "Removed " << removed << " events from search index";
        }
    }
}
