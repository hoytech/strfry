#include <iostream>

#include <docopt.h>
#include "golpe.h"

#include "WriterPipeline.h"


static const char USAGE[] =
R"(
    Usage:
      import [--show-rejected] [--no-verify] [--debounce-millis=<debounce-millis>] [--write-batch=<write-batch>]
)";


void cmd_import(const std::vector<std::string> &subArgs) {
    std::map<std::string, docopt::value> args = docopt::docopt(USAGE, subArgs, true, "");

    bool showRejected = args["--show-rejected"].asBool();
    bool noVerify = args["--no-verify"].asBool();
    uint64_t debounceMillis = 1'000;
    if (args["--debounce-millis"]) debounceMillis = args["--debounce-millis"].asLong();
    uint64_t writeBatch = 10'000;
    if (args["--write-batch"]) writeBatch = args["--write-batch"].asLong();

    if (noVerify) LW << "not verifying event IDs or signatures!";

    WriterPipeline writer;

    writer.debounceDelayMilliseconds = debounceMillis;
    writer.writeBatchSize = writeBatch;
    writer.verifyMsg = !noVerify;
    writer.verifyTime = false;
    writer.verboseReject = showRejected;
    writer.verboseCommit = false;
    writer.onCommit = [&](uint64_t numCommitted){
        LI << "Committed " << numCommitted
           << ". Processed " << writer.totalProcessed << " lines. " << writer.totalWritten << " added, " << writer.totalRejected << " rejected, " << writer.totalDups << " dups";
    };

    std::string line;
    uint64_t currLine = 0;

    while (std::cin) {
        currLine++;
        std::getline(std::cin, line);
        if (!line.size()) continue;

        tao::json::value evJson;

        try {
            evJson = tao::json::from_string(line);
        } catch (std::exception &e) {
            LW << "Unable to parse JSON on line " << currLine;
            continue;
        }

        writer.write({ std::move(evJson), });
        writer.wait();
    }

    writer.flush();

    LI << "Done. Processed " << writer.totalProcessed << " lines. " << writer.totalWritten << " added, " << writer.totalRejected << " rejected, " << writer.totalDups << " dups";
}
