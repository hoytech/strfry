#include <iostream>

#include <docopt.h>
#include "golpe.h"

#include "events.h"
#include "filters.h"


static const char USAGE[] =
R"(
    Usage:
      import [--show-rejected] [--no-verify]
)";


void cmd_import(const std::vector<std::string> &subArgs) {
    std::map<std::string, docopt::value> args = docopt::docopt(USAGE, subArgs, true, "");

    bool showRejected = args["--show-rejected"].asBool();
    bool noVerify = args["--no-verify"].asBool();

    if (noVerify) LW << "not verifying event IDs or signatures!";

    auto txn = env.txn_rw();

    secp256k1_context *secpCtx = secp256k1_context_create(SECP256K1_CONTEXT_VERIFY);

    std::string line;
    uint64_t processed = 0, added = 0, rejected = 0, dups = 0;
    std::vector<EventToWrite> newEvents;

    auto logStatus = [&]{
        LI << "Processed " << processed << " lines. " << added << " added, " << rejected << " rejected, " << dups << " dups";
    };

    auto flushChanges = [&]{
        writeEvents(txn, newEvents, 0);

        uint64_t numCommits = 0;

        for (auto &newEvent : newEvents) {
            if (newEvent.status == EventWriteStatus::Written) {
                added++;
                numCommits++;
            } else if (newEvent.status == EventWriteStatus::Duplicate) {
                dups++;
            } else {
                rejected++;
            }
        }

        logStatus();
        LI << "Committing " << numCommits << " records";

        txn.commit();

        txn = env.txn_rw();
        newEvents.clear();
    };


    while (std::cin) {
        std::getline(std::cin, line);
        if (!line.size()) continue;

        processed++;

        std::string flatStr;
        std::string jsonStr;

        try {
            auto origJson = tao::json::from_string(line);
            parseAndVerifyEvent(origJson, secpCtx, !noVerify, false, flatStr, jsonStr);
        } catch (std::exception &e) {
            if (showRejected) LW << "Line " << processed << " rejected: " << e.what();
            rejected++;
            continue;
        }

        newEvents.emplace_back(std::move(flatStr), std::move(jsonStr), hoytech::curr_time_us(), EventSourceType::Import, "");

        if (newEvents.size() >= 10'000) flushChanges();
    }

    flushChanges();

    txn.commit();
}
