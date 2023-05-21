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

    auto txn = env.txn_ro();

    std::cout << "DB version: " << getDBVersion(txn) << "\n";
}
