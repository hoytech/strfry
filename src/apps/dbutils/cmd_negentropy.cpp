#include <iostream>

#include <docopt.h>
#include "golpe.h"

#include "Bytes32.h"
#include "NegentropyFilterCache.h"
#include "events.h"
#include "DBQuery.h"


static const char USAGE[] =
R"(
    Usage:
      negentropy list
      negentropy add <filter>
      negentropy build <treeId>
)";


static void increaseModCounter(lmdb::txn &txn) {
    auto m = env.lookup_Meta(txn, 1);
    if (!m) throw herr("no Meta entry?");
    env.update_Meta(txn, *m, { .negentropyModificationCounter = m->negentropyModificationCounter() + 1 });
}


void cmd_negentropy(const std::vector<std::string> &subArgs) {
    std::map<std::string, docopt::value> args = docopt::docopt(USAGE, subArgs, true, "");

    if (args["list"].asBool()) {
        auto txn = env.txn_ro();

        env.foreach_NegentropyFilter(txn, [&](auto &f){
            auto treeId = f.primaryKeyId;

            std::cout << "tree " << treeId << "\n";
            std::cout << "  filter: " << f.filter() << "\n";

            negentropy::storage::BTreeLMDB storage(txn, negentropyDbi, treeId);
            auto size = storage.size();
            std::cout << "  size: " << size << "\n";
            std::cout << "  fingerprint: " << to_hex(storage.fingerprint(0, size).sv()) << "\n";

            return true;
        });
    } else if (args["add"].asBool()) {
        std::string filterStr = args["<filter>"].asString();

        tao::json::value filterJson = tao::json::from_string(filterStr);
        auto compiledFilter = NostrFilterGroup::unwrapped(filterJson);

        if (compiledFilter.filters.size() == 1 && (compiledFilter.filters[0].since != 0 || compiledFilter.filters[0].until != MAX_U64)) {
            throw herr("single filters should not have since/until");
        }
        if (compiledFilter.filters.size() == 0) throw herr("filter will never match");

        filterStr = tao::json::to_string(filterJson); // make canonical

        auto txn = env.txn_rw();
        increaseModCounter(txn);

        env.foreach_NegentropyFilter(txn, [&](auto &f){
            if (f.filter() == filterStr) throw herr("filter already exists as tree: ", f.primaryKeyId);
            return true;
        });

        uint64_t treeId = env.insert_NegentropyFilter(txn, filterStr);
        txn.commit();

        std::cout << "created tree " << treeId << "\n";
        std::cout << "  to populate, run: strfry negentropy build " << treeId << "\n";
    } else if (args["build"].asBool()) {
        uint64_t treeId = args["<treeId>"].asLong();

        struct Record {
            uint64_t created_at;
            Bytes32 id;
        };

        std::vector<Record> recs;

        auto txn = env.txn_rw(); // FIXME: split this into a read-only phase followed by a write
        increaseModCounter(txn);

        // Get filter

        std::string filterStr;

        {
            auto view = env.lookup_NegentropyFilter(txn, treeId);
            if (!view) throw herr("couldn't find treeId: ", treeId);
            filterStr = view->filter();
        }

        // Query all matching events

        DBQuery query(tao::json::from_string(filterStr));

        while (1) {
            bool complete = query.process(txn, [&](const auto &sub, uint64_t levId){
                auto ev = lookupEventByLevId(txn, levId);
                auto packed = PackedEventView(ev.buf);
                recs.emplace_back(packed.created_at(), packed.id());
                //memcpy(recs.back().id, packed.id().data(), 32);
            });

            if (complete) break;
        }

        // Store events in negentropy tree

        negentropy::storage::BTreeLMDB storage(txn, negentropyDbi, treeId);

        for (const auto &r : recs) {
            storage.insert(r.created_at, r.id.sv());
        }

        storage.flush();

        txn.commit();
    }
}
