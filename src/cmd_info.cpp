#include <iostream>

#include <docopt.h>
#include "golpe.h"


static const char USAGE[] =
R"(
    Usage:
      info
)";


void cmd_info(const std::vector<std::string> &subArgs) {
    std::map<std::string, docopt::value> args = docopt::docopt(USAGE, subArgs, true, "");

    quadrable::Quadrable qdb;
    {
        auto txn = env.txn_ro();
        qdb.init(txn);
    }
    qdb.checkout("events");

    auto txn = env.txn_ro();

    uint64_t dbVersion;

    {
        auto s = env.lookup_Meta(txn, 1);

        if (s) {
            dbVersion = s->dbVersion();
        } else {
            dbVersion = 0;
        }
    }

    std::cout << "DB version: " << dbVersion << "\n";
    std::cout << "merkle root: " << to_hex(qdb.root(txn)) << "\n";
}
