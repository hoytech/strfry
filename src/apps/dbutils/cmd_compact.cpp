#include <unistd.h>
#include <stdio.h>

#include <docopt.h>
#include "golpe.h"


static const char USAGE[] =
R"(
    Usage:
      compact <output_file>
)";


void cmd_compact(const std::vector<std::string> &subArgs) {
    std::map<std::string, docopt::value> args = docopt::docopt(USAGE, subArgs, true, "");

    std::string outputFile = args["<output_file>"].asString();

    if (outputFile == "-") {
        env.copy_fd(1);
    } else {
        if (access(outputFile.c_str(), F_OK) == 0) throw herr("output file '", outputFile, "' exists, not overwriting");

        auto *f = ::fopen(outputFile.c_str(), "w");
        if (!f) throw herr("opening output file '", outputFile, "' failed: ", strerror(errno));

        env.copy_fd(::fileno(f));
    }
}
