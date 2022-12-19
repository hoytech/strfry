#pragma once

#include <hoytech/protected_queue.h>

#include "golpe.h"

#include "events.h"


struct WriterPipeline {
  public:
    hoytech::protected_queue<tao::json::value> inbox;
    hoytech::protected_queue<bool> flushInbox;

  private:
    hoytech::protected_queue<EventToWrite> writerInbox;
    std::thread validatorThread;
    std::thread writerThread;

  public:
    WriterPipeline() {
        validatorThread = std::thread([&]() {
            setThreadName("Validator");

            secp256k1_context *secpCtx = secp256k1_context_create(SECP256K1_CONTEXT_VERIFY);

            while (1) {
                auto msgs = inbox.pop_all();

                for (auto &m : msgs) {
                    if (m.is_null()) {
                        writerInbox.push_move({ "", "", 0 });
                        break;
                    }

                    std::string flatStr;
                    std::string jsonStr;

                    try {
                        parseAndVerifyEvent(m, secpCtx, true, true, flatStr, jsonStr);
                    } catch (std::exception &e) {
                        LW << "Rejected event: " << m << " reason: " << e.what();
                        continue;
                    }

                    writerInbox.push_move({ std::move(flatStr), std::move(jsonStr), hoytech::curr_time_us() });
                }
            }
        });

        writerThread = std::thread([&]() {
            setThreadName("Writer");

            quadrable::Quadrable qdb;
            {
                auto txn = env.txn_ro();
                qdb.init(txn);
            }
            qdb.checkout("events");

            while (1) {
                // Debounce
                writerInbox.wait();
                std::this_thread::sleep_for(std::chrono::milliseconds(1'000));
                auto newEvents = writerInbox.pop_all();

                bool flush = false;
                uint64_t written = 0, dups = 0;

                // Collect a certain amount of records in a batch, push the rest back into the inbox
                // Pre-filter out dups in a read-only txn as an optimisation

                std::deque<EventToWrite> newEventsToProc;

                {
                    auto txn = env.txn_ro();

                    for (auto &event : newEvents) {
                        if (newEventsToProc.size() > 1'000) {
                            // Put the rest back in the inbox
                            writerInbox.unshift_move_all(newEvents);
                            newEvents.clear();
                            break;
                        }

                        if (event.flatStr.size() == 0) {
                            flush = true;
                            break;
                        }

                        auto *flat = flatStrToFlatEvent(event.flatStr);
                        if (lookupEventById(txn, sv(flat->id()))) {
                            dups++;
                            continue;
                        }

                        newEventsToProc.emplace_back(std::move(event));
                    }
                }

                if (newEventsToProc.size()) {
                    {
                        auto txn = env.txn_rw();
                        writeEvents(txn, qdb, newEventsToProc);
                        txn.commit();
                    }

                    for (auto &ev : newEventsToProc) {
                        if (ev.status == EventWriteStatus::Written) written++;
                        else dups++;
                        // FIXME: log rejected stats too
                    }
                }

                LI << "Writer: added: " << written << " dups: " << dups;

                if (flush) flushInbox.push_move(true);
            }
        });
    }

    void flush() {
        inbox.push_move(tao::json::null);
        flushInbox.wait();
    }
};
