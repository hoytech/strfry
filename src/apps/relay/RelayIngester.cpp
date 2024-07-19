#include "RelayServer.h"


void RelayServer::runIngester(ThreadPool<MsgIngester>::Thread &thr) {
    secp256k1_context *secpCtx = secp256k1_context_create(SECP256K1_CONTEXT_VERIFY);
    Decompressor decomp;

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

                        if (!payload.is_array()) throw herr("message is not an array");
                        auto &arr = payload.get_array();
                        if (arr.size() < 2) throw herr("bad message");

                        auto &cmd = arr[0].get_string();

                        if (cmd == "EVENT") {
                            if (cfg().relay__logging__dumpInEvents) LI << "[" << msg->connId << "] dumpInEvent: " << msg->payload; 

                            try {
                                ingesterProcessEvent(txn, msg->connId, msg->ipAddr, secpCtx, arr[1], writerMsgs);
                            } catch (std::exception &e) {
                                sendOKResponse(msg->connId, arr[1].at("id").get_string(), false, std::string("invalid: ") + e.what());
                                if (cfg().relay__logging__invalidEvents) LI << "Rejected invalid event: " << e.what();
                            }
                        } else if (cmd == "REQ") {
                            if (cfg().relay__logging__dumpInReqs) LI << "[" << msg->connId << "] dumpInReq: " << msg->payload; 

                            try {
                                ingesterProcessReq(txn, msg->connId, arr);
                            } catch (std::exception &e) {
                                sendNoticeError(msg->connId, std::string("bad req: ") + e.what());
                            }
                        } else if (cmd == "CLOSE") {
                            if (cfg().relay__logging__dumpInReqs) LI << "[" << msg->connId << "] dumpInReq: " << msg->payload; 

                            try {
                                ingesterProcessClose(txn, msg->connId, arr);
                            } catch (std::exception &e) {
                                sendNoticeError(msg->connId, std::string("bad close: ") + e.what());
                            }
                        } else if (cmd.starts_with("NEG-")) {
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
    std::string flatStr, jsonStr;

    parseAndVerifyEvent(origJson, secpCtx, true, true, flatStr, jsonStr);

    auto *flat = flatbuffers::GetRoot<NostrIndex::Event>(flatStr.data());

    {
        for (const auto &tagArr : origJson.at("tags").get_array()) {
            auto tag = tagArr.get_array();
            if (tag.size() == 1 && tag.at(0).get_string() == "-") {
                LI << "Protected event, skipping";
                sendOKResponse(connId, to_hex(sv(flat->id())), false, "blocked: event marked as protected");
                return;
            }
        }
    }

    {
        auto existing = lookupEventById(txn, sv(flat->id()));
        if (existing) {
            LI << "Duplicate event, skipping";
            sendOKResponse(connId, to_hex(sv(flat->id())), true, "duplicate: have this event");
            return;
        }
    }

    output.emplace_back(MsgWriter{MsgWriter::AddEvent{connId, std::move(ipAddr), hoytech::curr_time_us(), std::move(flatStr), std::move(jsonStr)}});
}

void RelayServer::ingesterProcessReq(lmdb::txn &txn, uint64_t connId, const tao::json::value &arr) {
    if (arr.get_array().size() < 2 + 1) throw herr("arr too small");
    if (arr.get_array().size() > 2 + 20) throw herr("arr too big");

    Subscription sub(connId, arr[1].get_string(), NostrFilterGroup(arr));

    tpReqWorker.dispatch(connId, MsgReqWorker{MsgReqWorker::NewSub{std::move(sub)}});
}

void RelayServer::ingesterProcessClose(lmdb::txn &txn, uint64_t connId, const tao::json::value &arr) {
    if (arr.get_array().size() != 2) throw herr("arr too small/big");

    tpReqWorker.dispatch(connId, MsgReqWorker{MsgReqWorker::RemoveSub{connId, SubId(arr[1].get_string())}});
}

void RelayServer::ingesterProcessNegentropy(lmdb::txn &txn, Decompressor &decomp, uint64_t connId, const tao::json::value &arr) {
    if (arr.at(0) == "NEG-OPEN") {
        if (arr.get_array().size() < 5) throw herr("negentropy query missing elements");

        NostrFilterGroup filter;
        auto maxFilterLimit = cfg().relay__negentropy__maxSyncEvents + 1;

        if (arr.at(2).is_string()) {
            auto ev = lookupEventById(txn, from_hex(arr.at(2).get_string()));
            if (!ev) {
                sendToConn(connId, tao::json::to_string(tao::json::value::array({
                    "NEG-ERR",
                    arr[1].get_string(),
                    "FILTER_NOT_FOUND"
                })));

                return;
            }

            tao::json::value json = tao::json::from_string(getEventJson(txn, decomp, ev->primaryKeyId));

            try {
                filter = std::move(NostrFilterGroup::unwrapped(tao::json::from_string(json.at("content").get_string()), maxFilterLimit));
            } catch (std::exception &e) {
                sendToConn(connId, tao::json::to_string(tao::json::value::array({
                    "NEG-ERR",
                    arr[1].get_string(),
                    "FILTER_INVALID"
                })));

                return;
            }
        } else {
            filter = std::move(NostrFilterGroup::unwrapped(arr.at(2), maxFilterLimit));
        }

        Subscription sub(connId, arr[1].get_string(), std::move(filter));

        uint64_t idSize = arr.at(3).get_unsigned();
        if (idSize < 8 || idSize > 32) throw herr("idSize out of range");

        std::string negPayload = from_hex(arr.at(4).get_string());

        tpNegentropy.dispatch(connId, MsgNegentropy{MsgNegentropy::NegOpen{std::move(sub), idSize, std::move(negPayload)}});
    } else if (arr.at(0) == "NEG-MSG") {
        std::string negPayload = from_hex(arr.at(2).get_string());
        tpNegentropy.dispatch(connId, MsgNegentropy{MsgNegentropy::NegMsg{connId, SubId(arr[1].get_string()), std::move(negPayload)}});
    } else if (arr.at(0) == "NEG-CLOSE") {
        tpNegentropy.dispatch(connId, MsgNegentropy{MsgNegentropy::NegClose{connId, SubId(arr[1].get_string())}});
    } else {
        throw herr("unknown command");
    }
}
