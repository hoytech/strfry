#include <unistd.h>
#include <stdio.h>

#include <docopt.h>
#include "golpe.h"

#include "gc.h"


static const char USAGE[] =
R"(
    Usage:
      gc
)";


void cmd_gc(const std::vector<std::string> &subArgs) {
    std::map<std::string, docopt::value> args = docopt::docopt(USAGE, subArgs, true, "");

    auto qdb = getQdbInstance();
    quadrableGarbageCollect(qdb, 2);
}
