#include <docopt.h>

#include "golpe.h"

#include "WebData.h"
#include "AlgoScanner.h"
#include "Decompressor.h"


static const char USAGE[] =
R"(
    Usage:
      algo scan <descriptor>
)";


void cmd_algo(const std::vector<std::string> &subArgs) {
    std::map<std::string, docopt::value> args = docopt::docopt(USAGE, subArgs, true, "");

    std::string descriptor = args["<descriptor>"].asString();


    UserCache userCache;
    Decompressor decomp;
    auto txn = env.txn_ro();

    auto communitySpec = lookupCommunitySpec(txn, decomp, userCache, descriptor);

    AlgoScanner a(txn, communitySpec.algo);
    auto events = a.getEvents(txn, decomp, 300);

    for (const auto &e : events) {
        auto ev = Event::fromLevId(txn, e.levId);
        ev.populateJson(txn, decomp);
        std::cout << e.info.score << "/" << e.info.comments << " : " << ev.summaryHtml() << "\n";
    }


/*
    std::string str;

    {
        std::string line;
        while (std::getline(std::cin, line)) {
            str += line;
            str += "\n";
        }
    }

    auto alg = parseAlgo(txn, str);

    for (const auto &[k, v] : alg.variableIndexLookup) {
        LI << k << " = " << alg.pubkeySets[v].size() << " recs";
    }
    */
}
