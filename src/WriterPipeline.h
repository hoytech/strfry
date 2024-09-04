#pragma once

#include <hoytech/protected_queue.h>

#include "golpe.h"

#include "events.h"


struct WriterPipelineInput {
    tao::json::value eventJson;
};


struct WriterPipeline {
  public:
    // Params:

    uint64_t debounceDelayMilliseconds = 1'000;
    uint64_t writeBatchSize = 1'000;
    bool verifyMsg = true;
    bool verifyTime = true;
    bool verboseReject = true;
    bool verboseCommit = true;
    std::function<void(uint64_t)> onCommit;

    // For logging:

    std::atomic<uint64_t> totalProcessed = 0;
    std::atomic<uint64_t> totalWritten = 0;
    std::atomic<uint64_t> totalRejected = 0;
    std::atomic<uint64_t> totalDups = 0;

  private:
    hoytech::protected_queue<WriterPipelineInput> validatorInbox;
    hoytech::protected_queue<EventToWrite> writerInbox;
    hoytech::protected_queue<bool> flushInbox;
    std::thread validatorThread;
    std::thread writerThread;

    std::condition_variable shutdownCv;
    std::mutex shutdownMutex;
    std::atomic<bool> shutdownRequested = false;
    std::atomic<bool> shutdownComplete = false;

    std::atomic<uint64_t> numLive = 0;
    std::condition_variable backpressureCv;
    std::mutex backpressureMutex;

  public:
    WriterPipeline() {
        validatorThread = std::thread([&]() {
            setThreadName("Validator");

            secp256k1_context *secpCtx = secp256k1_context_create(SECP256K1_CONTEXT_VERIFY);

            while (1) {
                auto msgs = validatorInbox.pop_all();

                for (auto &m : msgs) {
                    if (m.eventJson.is_null()) {
                        shutdownRequested = true;
                        writerInbox.push_move({});
                        shutdownCv.notify_all();
                        return;
                    }

                    std::string packedStr;
                    std::string jsonStr;

                    try {
                        parseAndVerifyEvent(m.eventJson, secpCtx, verifyMsg, verifyTime, packedStr, jsonStr);
                    } catch (std::exception &e) {
                        if (verboseReject) LW << "Rejected event: " << m.eventJson << " reason: " << e.what();
                        numLive--;
                        totalRejected++;
                        continue;
                    }

                    writerInbox.push_move({ std::move(packedStr), std::move(jsonStr), });
                }
            }
        });

        writerThread = std::thread([&]() {
            setThreadName("Writer");

            NegentropyFilterCache neFilterCache;

            while (1) {
                // Debounce

                {
                    auto numPendingElems = writerInbox.wait();

                    if (!shutdownRequested && numPendingElems < writeBatchSize) {
                        std::unique_lock<std::mutex> lk(shutdownMutex);
                        shutdownCv.wait_for(lk, std::chrono::milliseconds(debounceDelayMilliseconds), [&]{return !!shutdownRequested;}); 
                    }
                }

                auto newEvents = writerInbox.pop_all();

                uint64_t written = 0, dups = 0;

                // Collect a certain amount of records in a batch, push the rest back into the writerInbox
                // Pre-filter out dups in a read-only txn as an optimisation

                std::vector<EventToWrite> newEventsToProc;

                {
                    auto txn = env.txn_ro();

                    while (newEvents.size()) {
                        if (newEventsToProc.size() >= writeBatchSize) {
                            // Put the rest back in the writerInbox
                            writerInbox.unshift_move_all(newEvents);
                            newEvents.clear();
                            break;
                        }

                        auto event = std::move(newEvents.front());
                        newEvents.pop_front();

                        if (event.packedStr.size() == 0) {
                            shutdownComplete = true;
                            break;
                        }

                        numLive--;

                        PackedEventView packed(event.packedStr);
                        if (lookupEventById(txn, packed.id())) {
                            dups++;
                            totalDups++;
                            continue;
                        }

                        newEventsToProc.emplace_back(std::move(event));
                    }
                }

                if (newEventsToProc.size()) {
                    {
                        auto txn = env.txn_rw();
                        writeEvents(txn, neFilterCache, newEventsToProc);
                        txn.commit();
                    }

                    for (auto &ev : newEventsToProc) {
                        if (ev.status == EventWriteStatus::Written) {
                            written++;
                            totalWritten++;
                        } else {
                            dups++;
                            totalDups++;
                        }
                    }

                    if (onCommit) onCommit(written);
                }

                if (verboseCommit && (written || dups)) LI << "Writer: added: " << written << " dups: " << dups;

                if (shutdownComplete) {
                    flushInbox.push_move(true);
                    if (numLive != 0) LW << "numLive was not 0 after shutdown!";
                    return;
                }

                backpressureCv.notify_all();
            }
        });
    }

    ~WriterPipeline() {
        flush();
        validatorThread.join();
        writerThread.join();
    }

    void write(WriterPipelineInput &&inp) {
        if (inp.eventJson.is_null()) return;
        totalProcessed++;
        numLive++;
        validatorInbox.push_move(std::move(inp));
    }

    void write(EventToWrite &&inp) {
        totalProcessed++;
        numLive++;
        writerInbox.push_move(std::move(inp));
    }

    void flush() {
        validatorInbox.push_move({ tao::json::null, });
        flushInbox.wait();
    }

    void wait() {
        uint64_t drainUntil = writeBatchSize * 2;
        if (numLive < drainUntil) return;
        std::unique_lock<std::mutex> lk(backpressureMutex);
        backpressureCv.wait_for(lk, std::chrono::milliseconds(50), [&]{return numLive < drainUntil;}); 
    }
};
