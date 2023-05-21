#include <iostream>

#include <docopt.h>
#include "golpe.h"

#include "DBQuery.h"
#include "events.h"


static const char USAGE[] =
R"(
    Usage:
      scan [--pause=<pause>] [--metrics] [--count] <filter>
)";


void cmd_scan(const std::vector<std::string> &subArgs) {
    std::map<std::string, docopt::value> args = docopt::docopt(USAGE, subArgs, true, "");

    uint64_t pause = 0;
    if (args["--pause"]) pause = args["--pause"].asLong();

    bool metrics = args["--metrics"].asBool();
    bool count = args["--count"].asBool();

    std::string filterStr = args["<filter>"].asString();


    DBQuery query(tao::json::from_string(filterStr));

    Decompressor decomp;

    auto txn = env.txn_ro();

    uint64_t numEvents = 0;

    while (1) {
        bool complete = query.process(txn, [&](const auto &sub, uint64_t levId, std::string_view eventPayload){
            if (count) numEvents++;
            else std::cout << getEventJson(txn, decomp, levId, eventPayload) << "\n";
        }, pause ? pause : MAX_U64, metrics);

        if (complete) break;
    }

    if (count) std::cout << numEvents << std::endl;
}
