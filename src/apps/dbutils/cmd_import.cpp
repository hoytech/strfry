#include <stdio.h>
#include <stdlib.h>
#include <cstring>

#include <iostream>

#include <docopt.h>
#include "golpe.h"

#include "PackedEvent.h"
#include "WriterPipeline.h"


static const char USAGE[] =
R"(
    Usage:
      import [--show-rejected] [--no-verify] [--debounce-millis=<debounce-millis>] [--write-batch=<write-batch>] [--fried]
)";



EventToWrite parseFried(std::string_view lineSv) {
    std::string line(lineSv);

    if (line.size() < 64) throw herr("fried too small");
    if (!line.ends_with("\"}")) throw herr("fried parse error");

    size_t i;
    for (i = line.size() - 3; i > 0 && line[i] != '"'; i--) {}

    if (!std::string_view(line).substr(0, i + 1).ends_with(",\"fried\":\"")) throw herr("fried parse error");

    std::string packed = from_hex(std::string_view(line).substr(i + 1, line.size() - i - 3));
    if (packed.size() < PACKED_EVENT_HEADER_SIZE) throw herr("fried packed too short");
    friedSwapEndianInPlace(packed);

    line[i - 9] = '}';
    line.resize(i - 8);

    return { std::move(packed), std::move(line), };
}


void cmd_import(const std::vector<std::string> &subArgs) {
    std::map<std::string, docopt::value> args = docopt::docopt(USAGE, subArgs, true, "");

    bool showRejected = args["--show-rejected"].asBool();
    bool noVerify = args["--no-verify"].asBool();
    bool fried = args["--fried"].asBool();
    uint64_t debounceMillis = 1'000;
    if (args["--debounce-millis"]) debounceMillis = args["--debounce-millis"].asLong();
    uint64_t writeBatch = fried ? 100'000 : 10'000;
    if (args["--write-batch"]) writeBatch = args["--write-batch"].asLong();

    if (noVerify) LW << "not verifying event IDs or signatures!";

    WriterPipeline writer;

    writer.debounceDelayMilliseconds = debounceMillis;
    writer.writeBatchSize = writeBatch;
    writer.verifyMsg = !noVerify;
    writer.verifyTime = false;
    writer.verboseReject = [=]{ return showRejected; };
    writer.onCommit = [&](uint64_t numCommitted){
        LI << "Committed " << numCommitted
           << ". Processed " << writer.totalProcessed << " lines. " << writer.totalWritten << " added, " << writer.totalRejected << " rejected, " << writer.totalDups << " dups";
    };

    size_t bufLen = cfg().events__maxEventSize + 1024;
    char *buf = (char*)::malloc(bufLen);

    uint64_t currLine = 0;

    while (ssize_t numRead = ::getline(&buf, &bufLen, stdin)) {
        if (numRead <= 0) break;
        currLine++;

        if ((uint64_t)numRead > cfg().events__maxEventSize) throw herr("Line larger than configured maxEventSize on line ", currLine);

        std::string_view line(buf, (size_t)numRead-1);

        if (fried) {
            try {
                writer.write(parseFried(line));
            } catch (std::exception &e) {
                LW << "Unable to parse fried JSON on line " << currLine;
                continue;
            }
        } else {
            tao::json::value evJson;

            try {
                evJson = tao::json::from_string(line);
            } catch (std::exception &e) {
                LW << "Unable to parse JSON on line " << currLine;
                continue;
            }

            writer.write({ std::move(evJson), });
        }
        writer.wait();
    }

    writer.flush();

    LI << "Done. Processed " << writer.totalProcessed << " lines. " << writer.totalWritten << " added, " << writer.totalRejected << " rejected, " << writer.totalDups << " dups";
}
