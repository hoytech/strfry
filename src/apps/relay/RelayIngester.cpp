#include "RelayServer.h"


struct ConnectionAuth {
    std::string challenge;
    bool authenticated = false;
    std::string authedPubkey; // hex
};


void RelayServer::runIngester(ThreadPool<MsgIngester>::Thread &thr) {
    secp256k1_context *secpCtx = secp256k1_context_create(SECP256K1_CONTEXT_VERIFY);
    Decompressor decomp;

    flat_hash_map<uint64_t, ConnectionAuth> connAuth;

    auto isAuthenticated = [&](uint64_t connId) -> bool {
        if (!cfg().relay__auth__enabled) return true;
        if (!cfg().relay__auth__required) return true;
        auto it = connAuth.find(connId);
        return it != connAuth.end() && it->second.authenticated;
    };

    auto sendRestricted = [&](uint64_t connId, const std::string &reason) {
        auto it = connAuth.find(connId);
        if (it != connAuth.end() && it->second.challenge.size()) {
            sendAuthChallenge(connId, it->second.challenge);
        }
        auto reply = tao::json::value::array({ "NOTICE", std::string("restricted: ") + reason });
        sendToConn(connId, tao::json::to_string(reply));
    };

    while(1) {
        auto newMsgs = thr.inbox.pop_all();

        auto txn = env.txn_ro();

        std::vector<MsgWriter> writerMsgs;

        for (auto &newMsg : newMsgs) {
            if (auto msg = std::get_if<MsgIngester::OpenConn>(&newMsg.msg)) {
                auto &auth = connAuth[msg->connId];
                auth.challenge = SessionToken::generateChallenge();
                LI << "[" << msg->connId << "] Sending AUTH challenge";
                sendAuthChallenge(msg->connId, auth.challenge);

            } else if (auto msg = std::get_if<MsgIngester::ClientMessage>(&newMsg.msg)) {
                try {
                    if (msg->payload.starts_with('[')) {
                        auto payload = tao::json::from_string(msg->payload);

                        if (cfg().relay__logging__dumpInAll) LI << "[" << msg->connId << "] dumpInAll: " << msg->payload; 

                        auto &arr = jsonGetArray(payload, "message is not an array");
                        if (arr.size() < 2) throw herr("too few array elements");

                        auto &cmd = jsonGetString(arr[0], "first element not a command like REQ");

                        if (cmd == "EVENT") {
                            if (cfg().relay__logging__dumpInEvents) LI << "[" << msg->connId << "] dumpInEvent: " << msg->payload; 

                            if (!isAuthenticated(msg->connId)) {
                                auto eventId = arr[1].is_object() && arr[1].at("id").is_string() ? arr[1].at("id").get_string() : "?";
                                sendOKResponse(msg->connId, eventId, false, "restricted: authentication required to publish events");
                                continue;
                            }

                            try {
                                ingesterProcessEvent(txn, msg->connId, msg->ipAddr, secpCtx, arr[1], writerMsgs);
                            } catch (std::exception &e) {
                                sendOKResponse(msg->connId, arr[1].is_object() && arr[1].at("id").is_string() ? arr[1].at("id").get_string() : "?",
                                               false, std::string("invalid: ") + e.what());
                                if (cfg().relay__logging__invalidEvents) LI << "Rejected invalid event: " << e.what();
                            }
                        } else if (cmd == "REQ") {
                            if (cfg().relay__logging__dumpInReqs) LI << "[" << msg->connId << "] dumpInReq: " << msg->payload; 

                            if (!isAuthenticated(msg->connId)) {
                                sendRestricted(msg->connId, "authentication required to request events");
                                continue;
                            }

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
                        } else if (cmd == "AUTH") {
                            if (!cfg().relay__auth__enabled) throw herr("auth not enabled on this relay");

                            try {
                                if (arr.size() < 2) throw herr("AUTH message too short");

                                if (!arr[1].is_object()) throw herr("AUTH message second element must be a signed event object");

                                auto &authEvent = arr[1];

                                // Parse and verify the auth event signature
                                std::string packedStr, jsonStr;
                                parseAndVerifyEvent(authEvent, secpCtx, true, false, packedStr, jsonStr);
                                PackedEventView packed(packedStr);

                                // Verify kind == 22242
                                if (packed.kind() != 22242) throw herr("AUTH event must be kind 22242");

                                // Verify created_at is within ~10 minutes
                                auto now = hoytech::curr_time_s();
                                auto ts = packed.created_at();
                                if (ts > now + 600 || ts < now - 600) throw herr("AUTH event created_at is too far from current time");

                                // Extract and verify tags
                                std::string challengeTag, relayTag, clientTag;
                                auto &tags = authEvent.at("tags").get_array();
                                for (auto &tag : tags) {
                                    auto &tagArr = tag.get_array();
                                    if (tagArr.size() >= 2) {
                                        auto &tagName = tagArr[0].get_string();
                                        auto &tagVal = tagArr[1].get_string();
                                        if (tagName == "challenge") challengeTag = tagVal;
                                        else if (tagName == "relay") relayTag = tagVal;
                                        else if (tagName == "client") clientTag = tagVal;
                                    }
                                }

                                if (challengeTag.empty()) throw herr("AUTH event missing challenge tag");
                                if (relayTag.empty()) throw herr("AUTH event missing relay tag");

                                // Verify challenge matches
                                auto authIt = connAuth.find(msg->connId);
                                if (authIt == connAuth.end()) throw herr("no AUTH challenge pending for this connection");
                                if (authIt->second.challenge != challengeTag) throw herr("AUTH challenge mismatch");

                                // Verify relay URL if configured
                                if (cfg().relay__auth__relayUrl.size()) {
                                    if (relayTag != cfg().relay__auth__relayUrl) {
                                        LI << "[" << msg->connId << "] AUTH relay tag mismatch: got '" << relayTag << "' expected '" << cfg().relay__auth__relayUrl << "'";
                                        throw herr("AUTH relay URL mismatch");
                                    }
                                }

                                // Auth successful
                                auto pubkeyHex = to_hex(packed.pubkey());
                                authIt->second.authenticated = true;
                                authIt->second.authedPubkey = pubkeyHex;

                                LI << "[" << msg->connId << "] Authenticated as " << pubkeyHex;

                                auto eventIdHex = to_hex(packed.id());
                                sendOKResponse(msg->connId, eventIdHex, true, "");

                                // Issue session token if enabled
                                if (cfg().relay__auth__sessionTokenEnabled && sessionSecret.size()) {
                                    uint64_t lifetime = cfg().relay__auth__sessionTokenLifetimeSeconds;
                                    auto token = SessionToken::generate(sessionSecret, pubkeyHex, lifetime, clientTag);
                                    uint64_t expiresAt = hoytech::curr_time_s() + lifetime;
                                    sendSessionToken(msg->connId, token, expiresAt);
                                    if (clientTag.size()) {
                                        LI << "[" << msg->connId << "] Issued client-bound session token (client=" << clientTag << "), expires in " << lifetime << "s";
                                    } else {
                                        LI << "[" << msg->connId << "] Issued unbound session token, expires in " << lifetime << "s";
                                    }
                                }

                            } catch (std::exception &e) {
                                LI << "[" << msg->connId << "] AUTH failed: " << e.what();
                                sendOKResponse(msg->connId, "?", false, std::string("auth-required: ") + e.what());
                            }
                        } else if (cmd == "SESSION") {
                            if (!cfg().relay__auth__enabled) throw herr("auth not enabled on this relay");
                            if (!cfg().relay__auth__sessionTokenEnabled) throw herr("session tokens not enabled on this relay");

                            try {
                                if (arr.size() < 2) throw herr("SESSION message too short");
                                auto &tokenStr = jsonGetString(arr[1], "SESSION token must be a string");

                                // Optional clientId: ["SESSION", "<token>", "<client_id>"]
                                std::string clientId;
                                if (arr.size() >= 3 && arr[2].is_string()) {
                                    clientId = arr[2].get_string();
                                }

                                auto validated = SessionToken::validate(sessionSecret, tokenStr, clientId);
                                if (!validated) throw herr("invalid, expired, or client-mismatched session token");

                                auto &auth = connAuth[msg->connId];
                                auth.authenticated = true;
                                auth.authedPubkey = validated->pubkeyHex;

                                if (validated->clientIdHex.size()) {
                                    LI << "[" << msg->connId << "] Client-bound session token accepted for " << validated->pubkeyHex
                                       << " (client=" << validated->clientIdHex << ", expires at " << validated->expiresAt << ")";
                                } else {
                                    LI << "[" << msg->connId << "] Unbound session token accepted for " << validated->pubkeyHex
                                       << " (expires at " << validated->expiresAt << ")";
                                }

                                sendToConn(msg->connId, tao::json::to_string(
                                    tao::json::value::array({ "NOTICE", "session token accepted" })
                                ));

                                // Issue a fresh token (preserving client binding) so the client always has a valid one
                                uint64_t lifetime = cfg().relay__auth__sessionTokenLifetimeSeconds;
                                auto newToken = SessionToken::generate(sessionSecret, validated->pubkeyHex, lifetime, clientId);
                                uint64_t expiresAt = hoytech::curr_time_s() + lifetime;
                                sendSessionToken(msg->connId, newToken, expiresAt);

                            } catch (std::exception &e) {
                                LI << "[" << msg->connId << "] SESSION failed: " << e.what();
                                sendNoticeError(msg->connId, std::string("session error: ") + e.what());
                                // Fall back: send an AUTH challenge so client can do NIP-42
                                if (connAuth.count(msg->connId)) {
                                    sendAuthChallenge(msg->connId, connAuth[msg->connId].challenge);
                                }
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
                connAuth.erase(connId);
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

    if (packed.kind() == 22242) {
        sendOKResponse(connId, to_hex(packed.id()), false, "blocked: kind 22242 events should be sent via AUTH, not EVENT");
        return;
    }

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
