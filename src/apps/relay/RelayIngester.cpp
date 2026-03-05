#include "RelayServer.h"


void RelayServer::runIngester(ThreadPool<MsgIngester>::Thread &thr) {
    secp256k1_context *secpCtx = secp256k1_context_create(SECP256K1_CONTEXT_VERIFY);
    Decompressor decomp;
    FilterValidator filterValidator;

    while(1) {
        auto newMsgs = thr.inbox.pop_all();

        auto txn = env.txn_ro();

        std::vector<MsgWriter> writerMsgs;

        for (auto &newMsg : newMsgs) {
            if (auto msg = std::get_if<MsgIngester::ClientMessage>(&newMsg.msg)) {
                try {
                    if (msg->payload.starts_with('[')) {
                        auto payload = tao::json::from_string(msg->payload);

                        if (cfg().relay__logging__dumpInAll) LI << "[" << msg->connId << "] dumpInAll: " << msg->payload; 

                        auto &arr = jsonGetArray(payload, "message is not an array");
                        if (arr.size() < 2) throw herr("too few array elements");

                        auto &cmd = jsonGetString(arr[0], "first element not a command like REQ");

                        if (cmd == "EVENT") {
                            PROM_INC_CLIENT_MSG(cmd);
                            if (cfg().relay__logging__dumpInEvents) LI << "[" << msg->connId << "] dumpInEvent: " << msg->payload; 

                            try {
                                ingesterProcessEvent(txn, msg->connId, msg->ipAddr, secpCtx, arr[1], writerMsgs);
                            } catch (std::exception &e) {
                                sendOKResponse(msg->connId, arr[1].is_object() && arr[1].at("id").is_string() ? arr[1].at("id").get_string() : "?",
                                               false, std::string("invalid: ") + e.what());
                                if (cfg().relay__logging__invalidEvents) LI << "Rejected invalid event: " << e.what();
                            }
                        } else if (cmd == "REQ" || cmd == "COUNT") {
                            PROM_INC_CLIENT_MSG(cmd);
                            if (cfg().relay__logging__dumpInReqs) LI << "[" << msg->connId << "] dumpInReq: " << msg->payload; 

                            std::string subIdStr;

                            try {
                                ingesterProcessReq(txn, filterValidator, msg->connId, arr, cmd == "COUNT", subIdStr);
                            } catch (std::exception &e) {
                                if (subIdStr.size()) sendClosedError(msg->connId, subIdStr, std::string("bad req: ") + e.what());
                                else sendNoticeError(msg->connId, std::string("bad req: ") + e.what());
                            }
                        } else if (cmd == "CLOSE") {
                            PROM_INC_CLIENT_MSG(cmd);
                            if (cfg().relay__logging__dumpInReqs) LI << "[" << msg->connId << "] dumpInReq: " << msg->payload; 

                            try {
                                ingesterProcessClose(txn, msg->connId, arr);
                            } catch (std::exception &e) {
                                sendNoticeError(msg->connId, std::string("bad close: ") + e.what());
                            }
                        } else if (cmd.starts_with("NEG-")) {
                            PROM_INC_CLIENT_MSG(cmd);
                            if (!cfg().relay__negentropy__enabled) throw herr("negentropy disabled");

                            try {
                                ingesterProcessNegentropy(txn, decomp, msg->connId, arr);
                            } catch (std::exception &e) {
                                sendNoticeError(msg->connId, std::string("negentropy error: ") + e.what());
                            }
                        } else {
                            throw herr("unknown cmd");
                        }
                    } else if (msg->payload == "\n") {
                        // Do nothing.
                        // This is for when someone is just sending newlines on websocat for debugging purposes.
                    } else {
                        throw herr("unparseable message");
                    }
                } catch (std::exception &e) {
                    sendNoticeError(msg->connId, std::string("bad msg: ") + e.what());
                }
            } else if (auto msg = std::get_if<MsgIngester::CloseConn>(&newMsg.msg)) {
                auto connId = msg->connId;
                tpWriter.dispatch(connId, MsgWriter{MsgWriter::CloseConn{connId}});
                tpReqWorker.dispatch(connId, MsgReqWorker{MsgReqWorker::CloseConn{connId}});
                tpNegentropy.dispatch(connId, MsgNegentropy{MsgNegentropy::CloseConn{connId}});
            }
        }

        if (writerMsgs.size()) {
            tpWriter.dispatchMulti(0, writerMsgs);
        }
    }
}

void RelayServer::ingesterProcessEvent(lmdb::txn &txn, uint64_t connId, std::string ipAddr, secp256k1_context *secpCtx, const tao::json::value &origJson, std::vector<MsgWriter> &output) {
    std::string packedStr, jsonStr;

    parseAndVerifyEvent(origJson, secpCtx, true, true, packedStr, jsonStr);

    PackedEventView packed(packedStr);
    
    // Track event kind metrics
    PROM_INC_EVENT_KIND(std::to_string(packed.kind()));

    {
        // discard reposts that embed protected events
        if ((packed.kind() == 6 || packed.kind() == 16)) {
          auto content = jsonGetString(origJson.at("content"),
                                       "event content field was not a string");

          if (content.find("[\"-\"]") != std::string::npos) {
            try {
              auto embedded = tao::json::from_string(content);
              auto &embeddedTags =
                  jsonGetArray(embedded.at("tags"),
                               "embedded event tags field was not an array");

              for (auto &tagArr : embeddedTags) {
                auto &tag = jsonGetArray(
                    tagArr, "tag in embedded event tags field was not an array");
                if (tag.size() == 1 &&
                    jsonGetString(tag.at(0),
                                  "embedded tag name was not a string") == "-") {
                  sendOKResponse(connId, to_hex(packed.id()), false,
                                 "blocked: reposts can't embed protected events");
                  return;
                }
              }
            } catch (std::exception &) {
            }
          }
        }

        bool foundProtected = false;

        packed.foreachTag([&](char tagName, std::string_view tagVal){
            if (tagName == '-') {
                foundProtected = true;
                return false;
            }
            return true;
        });

        if (foundProtected) {
            LI << "Protected event, skipping";
            sendOKResponse(connId, to_hex(packed.id()), false, "blocked: event marked as protected");
            return;
        }
    }

    {
        auto existing = lookupEventById(txn, packed.id());
        if (existing) {
            LI << "Duplicate event, skipping";
            sendOKResponse(connId, to_hex(packed.id()), true, "duplicate: have this event");
            return;
        }
    }

    output.emplace_back(MsgWriter{MsgWriter::AddEvent{connId, std::move(ipAddr), std::move(packedStr), std::move(jsonStr)}});
}

void RelayServer::ingesterProcessReq(lmdb::txn &txn, FilterValidator &filterValidator, uint64_t connId, const tao::json::value &arr, bool countOnly, std::string &outSubIdStr) {
    if (arr.get_array().size() < 2 + 1) throw herr("arr too small");
    outSubIdStr = jsonGetString(arr[1], "subscription id was not a string");
    if (arr.get_array().size() > 2 + cfg().relay__maxReqFilterSize) throw herr("arr too big");

    uint64_t maxFilterLimit;

    if (countOnly) {
        // + 1 so we can distinguish exact count versus limit exceeded
        maxFilterLimit = cfg().relay__maxFilterLimitCount + 1;
        if (maxFilterLimit == 1) throw herr("COUNT disabled");
    } else {
        maxFilterLimit = cfg().relay__maxFilterLimit;
    }

    NostrFilterGroup filterGroup(arr, maxFilterLimit);

    try {
        filterValidator.validate(filterGroup);
    } catch (std::exception &e) {
        throw herr("filter validation failed: ", e.what());
    }

    Subscription sub(connId, outSubIdStr, std::move(filterGroup), countOnly);

    tpReqWorker.dispatch(connId, MsgReqWorker{MsgReqWorker::NewSub{std::move(sub)}});
}

void RelayServer::ingesterProcessClose(lmdb::txn &txn, uint64_t connId, const tao::json::value &arr) {
    if (arr.get_array().size() != 2) throw herr("arr too small/big");

    tpReqWorker.dispatch(connId, MsgReqWorker{MsgReqWorker::RemoveSub{connId, SubId(jsonGetString(arr[1], "CLOSE subscription id was not a string"))}});
}

void RelayServer::ingesterProcessNegentropy(lmdb::txn &txn, Decompressor &decomp, uint64_t connId, const tao::json::value &arr) {
    const auto &subscriptionStr = jsonGetString(arr[1], "NEG-OPEN subscription id was not a string");

    if (arr.at(0) == "NEG-OPEN") {
        if (arr.get_array().size() < 4) throw herr("negentropy query missing elements");

        auto maxFilterLimit = cfg().relay__negentropy__maxSyncEvents + 1;

        auto filterJson = arr.at(2);

        NostrFilterGroup filter = NostrFilterGroup::unwrapped(filterJson, maxFilterLimit);
        Subscription sub(connId, subscriptionStr, std::move(filter));

        if (filterJson.is_object()) {
            filterJson.get_object().erase("since");
            filterJson.get_object().erase("until");
        }
        std::string filterStr = tao::json::to_string(filterJson);

        std::string negPayload = from_hex(jsonGetString(arr.at(3), "negentropy payload not a string"));

        tpNegentropy.dispatch(connId, MsgNegentropy{MsgNegentropy::NegOpen{std::move(sub), std::move(filterStr), std::move(negPayload)}});
    } else if (arr.at(0) == "NEG-MSG") {
        std::string negPayload = from_hex(jsonGetString(arr.at(2), "negentropy payload not a string"));
        tpNegentropy.dispatch(connId, MsgNegentropy{MsgNegentropy::NegMsg{connId, SubId(subscriptionStr), std::move(negPayload)}});
    } else if (arr.at(0) == "NEG-CLOSE") {
        tpNegentropy.dispatch(connId, MsgNegentropy{MsgNegentropy::NegClose{connId, SubId(subscriptionStr)}});
    } else {
        throw herr("unknown command");
    }
}
