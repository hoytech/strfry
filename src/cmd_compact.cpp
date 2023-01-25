#include <unistd.h>
#include <stdio.h>

#include <docopt.h>
#include "golpe.h"

#include "render.h"


static const char USAGE[] =
R"(
    Usage:
      compact export <output_file>
      compact quad-gc
)";


void cmd_compact(const std::vector<std::string> &subArgs) {
    std::map<std::string, docopt::value> args = docopt::docopt(USAGE, subArgs, true, "");

    if (args["export"].asBool()) {
        std::string outputFile = args["<output_file>"].asString();

        if (outputFile == "-") {
            env.copy_fd(1);
        } else {
            if (access(outputFile.c_str(), F_OK) == 0) throw herr("output file '", outputFile, "' exists, not overwriting");

            auto *f = ::fopen(outputFile.c_str(), "w");
            if (!f) throw herr("opening output file '", outputFile, "' failed: ", strerror(errno));

            env.copy_fd(::fileno(f));
        }
    } else if (args["quad-gc"].asBool()) {
        quadrable::Quadrable qdb;
        {
            auto txn = env.txn_ro();
            qdb.init(txn);
        }
        qdb.checkout("events");

        quadrable::Quadrable::GarbageCollector gc(qdb);

        {
            auto txn = env.txn_ro();
            gc.markAllHeads(txn);
        }

        {
            auto txn = env.txn_rw();

            auto stats = gc.sweep(txn);
            /*
            auto stats = gc.sweep(txn, [&](uint64_t nodeId){
                quadrable::Quadrable::ParsedNode node(&qdb, txn, nodeId);
                if (!node.isBranch()) throw herr("unexpected quadrable node type during gc: ", (int)node.nodeType);
                return true;
            });
            */

            txn.commit();

            LI << "Total nodes: " << stats.total;
            LI << "Collected:   " << stats.collected << " (" << renderPercent((double)stats.collected / stats.total) << ")";
        }
    }
}
