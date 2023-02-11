#include <iostream>

#include <docopt.h>
#include "golpe.h"

#include "DBScan.h"
#include "events.h"
#include "gc.h"


static const char USAGE[] =
R"(
    Usage:
      delete [--age=<age>] [--filter=<filter>] [--dry-run] [--no-gc]
)";


void cmd_delete(const std::vector<std::string> &subArgs) {
    std::map<std::string, docopt::value> args = docopt::docopt(USAGE, subArgs, true, "");

    uint64_t age = MAX_U64;
    if (args["--age"]) age = args["--age"].asLong();

    std::string filterStr;
    if (args["--filter"]) filterStr = args["--filter"].asString();

    bool dryRun = args["--dry-run"].asBool();
    bool noGc = args["--no-gc"].asBool();



    if (filterStr.size() == 0 && age == MAX_U64) throw herr("must specify --age and/or --filter");
    if (filterStr.size() == 0) filterStr = "{}";


    auto filter = tao::json::from_string(filterStr);
    auto now = hoytech::curr_time_s();

    if (age != MAX_U64) {
        if (age > now) age = now;
        if (filter.optional<uint64_t>("until")) throw herr("--age is not compatible with filter containing 'until'");

        filter["until"] = now - age;
    }


    auto filterGroup = NostrFilterGroup::unwrapped(filter, MAX_U64);
    Subscription sub(1, "junkSub", filterGroup);
    DBScanQuery query(sub);


    btree_set<uint64_t> levIds;

    {
        auto txn = env.txn_ro();

        while (1) {
            bool complete = query.process(txn, MAX_U64, false, [&](const auto &sub, uint64_t levId){
                levIds.insert(levId);
            });

            if (complete) break;
        }
    }

    if (dryRun) {
        LI << "Would delete " << levIds.size() << " events";
        return;
    }


    auto qdb = getQdbInstance();

    LI << "Deleting " << levIds.size() << " events";

    {
        auto txn = env.txn_rw();

        auto changes = qdb.change();

        for (auto levId : levIds) {
            auto view = env.lookup_Event(txn, levId);
            if (!view) continue; // Deleted in between transactions
            deleteEvent(txn, changes, *view);
        }

        changes.apply(txn);

        txn.commit();
    }

    if (!noGc) quadrableGarbageCollect(qdb, 2);
}
