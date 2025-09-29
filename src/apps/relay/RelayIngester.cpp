#include "RelayServer.h"
#include "jsonParseUtils.h"
#include <cstdlib>


void RelayServer::runIngester(ThreadPool<MsgIngester>::Thread &thr) {
    secp256k1_context *secpCtx = secp256k1_context_create(SECP256K1_CONTEXT_VERIFY);
    Decompressor decomp;
    flat_hash_map<uint64_t, AuthStatus*> connIdToAuthStatus;

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
                            if (cfg().relay__logging__dumpInEvents) LI << "[" << msg->connId << "] dumpInEvent: " << msg->payload; 

                            try {
                                ingesterProcessEvent(txn, msg->connId, connIdToAuthStatus, msg->ipAddr, secpCtx, arr[1], writerMsgs);
                            } catch (std::exception &e) {
                                sendOKResponse(msg->connId, arr[1].is_object() && arr[1].at("id").is_string() ? arr[1].at("id").get_string() : "?",
                                               false, std::string("invalid: ") + e.what());
                                if (cfg().relay__logging__invalidEvents) LI << "Rejected invalid event: " << e.what();
                            }
                        } else if (cmd == "AUTH") {
                            if (cfg().relay__logging__dumpInAll) LI << "[" << msg->connId << "] dumpInAuth: " << msg->payload;

                            try {
                                ingesterProcessAuth(msg->connId, connIdToAuthStatus, secpCtx, arr[1]);
                            } catch (std::exception &e) {
                                sendNoticeError(msg->connId, std::string("auth failed: ") + e.what());
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

void RelayServer::ingesterProcessEvent(lmdb::txn &txn, uint64_t connId, flat_hash_map<uint64_t, AuthStatus*> &connIdToAuthStatus, std::string ipAddr, secp256k1_context *secpCtx, const tao::json::value &origJson, std::vector<MsgWriter> &output) {
    std::string packedStr, jsonStr;

    parseAndVerifyEvent(origJson, secpCtx, true, true, packedStr, jsonStr);

    PackedEventView packed(packedStr);

    {
        bool foundProtected = false;

        packed.foreachTag([&](char tagName, std::string_view tagVal){
            if (tagName == '-') {
                foundProtected = true;
                return false;
            }
            return true;
        });

        if (foundProtected) {
            // NIP-70 protected events must be rejected unless published by an authenticated public key
            // that matches the event author, so we do all the AUTH flow here
            if (cfg().relay__serviceUrl.empty()) {
                // except if we don't have a serviceUrl, in that case just fail
                LI << "Protected event and no serviceUrl configured, skipping";
                sendOKResponse(connId, to_hex(packed.id()), false, "blocked: event marked as protected");
                return;
            }

            auto as = connIdToAuthStatus.find(connId);
            if (as == connIdToAuthStatus.end()) {
                // we haven't sent an AUTH event for this, so first we generate a challenge for this connection
                auto authStatus = new AuthStatus();
                authStatus->challenge = std::to_string(int64_t(std::pow(packed.created_at(), connId + 1)));
                connIdToAuthStatus.emplace(connId, authStatus);
                LI << "Protected event, requesting AUTH";
                sendAuthChallenge(connId, authStatus->challenge);
                sendOKResponse(connId, to_hex(packed.id()), false, "auth-required: event marked as protected");
                return;
            }

            const auto authed = (*as->second).authed;
            if (authed.empty()) {
                // not authenticated
                sendOKResponse(connId, to_hex(packed.id()), false, "auth-required: event marked as protected");
                return;
            } else if (authed != packed.pubkey()) {
                // authenticated as someone else
                sendOKResponse(connId, to_hex(packed.id()), false, "restricted: must be published by the author");
                return;
            }
            // otherwise we proceed to accept the event
        }
        if (!foundProtected) {
            // Reject non-protected events
            LI << "Non-protected event rejected";
            sendOKResponse(connId, to_hex(packed.id()), false, "blocked: only protected events accepted");
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

void RelayServer::ingesterProcessReq(lmdb::txn &txn, uint64_t connId, const tao::json::value &arr) {
    if (arr.get_array().size() < 2 + 1) throw herr("arr too small");
    if (arr.get_array().size() > 2 + cfg().relay__maxReqFilterSize) throw herr("arr too big");

    Subscription sub(connId, jsonGetString(arr[1], "REQ subscription id was not a string"), NostrFilterGroup(arr));

    tpReqWorker.dispatch(connId, MsgReqWorker{MsgReqWorker::NewSub{std::move(sub)}});
}

void RelayServer::ingesterProcessClose(lmdb::txn &txn, uint64_t connId, const tao::json::value &arr) {
    if (arr.get_array().size() != 2) throw herr("arr too small/big");

    tpReqWorker.dispatch(connId, MsgReqWorker{MsgReqWorker::RemoveSub{connId, SubId(jsonGetString(arr[1], "CLOSE subscription id was not a string"))}});
}

void RelayServer::ingesterProcessAuth(uint64_t connId, flat_hash_map<uint64_t, AuthStatus*> connIdToAuthStatus, secp256k1_context *secpCtx, const tao::json::value &eventJson) {
    if (cfg().relay__serviceUrl.empty()) {
        throw herr("relay needs serviceUrl to be configured before AUTH can work");
    }

    std::string packedStr, jsonStr;
    parseAndVerifyEvent(eventJson, secpCtx, true, true, packedStr, jsonStr);

    PackedEventView packed(packedStr);

    if (packed.kind() != 22242) {
        throw herr("wrong event kind, expected 22242");
    }

    auto as = connIdToAuthStatus.find(connId);
    if (as == connIdToAuthStatus.end()) {
        throw herr("no auth status available for connection");
    }
    if (!(*as->second).authed.empty()) {
        throw herr("already authenticated");
    }
    const auto challenge = (*as->second).challenge;

    bool foundChallenge = false;
    bool foundCorrectRelayUrl = false;

    for (const auto &tagj : eventJson.at("tags").get_array()) {
        const auto &tag = tagj.get_array();
        if (tag.size() < 2) continue;
        const auto name = tag[0].as<std::string_view>();
        const auto value = tag[1].as<std::string_view>();
        if (name == "relay" && value == cfg().relay__serviceUrl) {
            foundCorrectRelayUrl = true;
        } else if (name == "challenge" && value == challenge) {
            foundChallenge = true;
        }
    }

    if (!foundChallenge) {
        throw herr("challenge string mismatch");
    }
    if (!foundCorrectRelayUrl) {
        throw herr("incorrect or missing relay tag, expected: " + cfg().relay__serviceUrl);
    }

    // set the connection as authenticated with this pubkey
    (*as->second).authed = packed.pubkey();

    sendOKResponse(connId, to_hex(packed.id()), true, "successfully authenticated");
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
