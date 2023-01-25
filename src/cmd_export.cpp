#include <iostream>

#include <docopt.h>
#include "golpe.h"

#include "events.h"


static const char USAGE[] =
R"(
    Usage:
      export [--since=<since>] [--until=<until>] [--include-ephemeral]
)";


void cmd_export(const std::vector<std::string> &subArgs) {
    std::map<std::string, docopt::value> args = docopt::docopt(USAGE, subArgs, true, "");

    uint64_t since = 0, until = MAX_U64;
    if (args["--since"]) since = args["--since"].asLong();
    if (args["--until"]) until = args["--until"].asLong();

    Decompressor decomp;

    auto txn = env.txn_ro();

    env.generic_foreachFull(txn, env.dbi_Event__created_at, lmdb::to_sv<uint64_t>(since), lmdb::to_sv<uint64_t>(0), [&](auto k, auto v) {
        if (lmdb::from_sv<uint64_t>(k) > until) return false;

        auto view = env.lookup_Event(txn, lmdb::from_sv<uint64_t>(v));
        if (!view) throw herr("missing event from index, corrupt DB?");

        if (!args["--include-ephemeral"].asBool()) {
            if (isEphemeralEvent(view->flat_nested()->kind())) return true;
        }

        std::cout << getEventJson(txn, decomp, view->primaryKeyId) << "\n";

        return true;
    });
}
