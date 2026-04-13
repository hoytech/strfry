#include "RelayServer.h"
#include "jsonParseUtils.h"


void RelayServer::runIngester(ThreadPool<MsgIngester>::Thread &thr) {
    RelayServerCtx rsctx;

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
                                ingesterProcessEvent(txn, rsctx, msg->connId, msg->ipAddr, arr[1], writerMsgs);
                            } catch (std::exception &e) {
                                sendOKResponse(msg->connId, arr[1].is_object() && arr[1].at("id").is_string() ? arr[1].at("id").get_string() : "?",
                                               false, std::string("invalid: ") + e.what());
                                if (cfg().relay__logging__invalidEvents) LI << "Rejected invalid event: " << e.what();
                            }
                        } else if (cmd == "AUTH") {
                            PROM_INC_CLIENT_MSG(cmd);
                            if (cfg().relay__logging__dumpInAll) LI << "[" << msg->connId << "] dumpInAuth: " << msg->payload;

                            try {
                                ingesterProcessAuth(rsctx, msg->connId, arr[1]);
                            } catch (std::exception &e) {
                                sendNoticeError(msg->connId, std::string("auth failed: ") + e.what());
                            }
                        } else if (cmd == "REQ" || cmd == "COUNT") {
                            PROM_INC_CLIENT_MSG(cmd);
                            if (cfg().relay__logging__dumpInReqs) LI << "[" << msg->connId << "] dumpInReq: " << msg->payload; 

                            std::string subIdStr;

                            try {
                                ingesterProcessReq(txn, rsctx, msg->connId, arr, cmd == "COUNT", subIdStr);
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
                                ingesterProcessNegentropy(txn, msg->connId, arr);
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

                rsctx.connIdToAuthStatus.erase(connId);

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

void RelayServer::ingesterProcessEvent(lmdb::txn &txn, RelayServerCtx &rsctx, uint64_t connId, std::string ipAddr, const tao::json::value &origJson, std::vector<MsgWriter> &output) {
    std::string packedStr, jsonStr;

    parseAndVerifyEvent(origJson, rsctx.secpCtx, true, true, packedStr, jsonStr);

    PackedEventView packed(packedStr);
    Bytes32 authedPubkey;
    
    // Track event kind metrics
    PROM_INC_EVENT_KIND(std::to_string(packed.kind()));

    {
        // discard reposts that embed protected events
        if (packed.kind() == 6 || packed.kind() == 16) {
            if (origJson.at("content").get_string().find("[\"-\"]") != std::string::npos) {
                auto idHex = to_hex(packed.id());
                LI << "Repost embedded a protected event, blocking: " << idHex;
                sendOKResponse(connId, idHex, false, "blocked: reposts can't embed protected events");
                return;
            }
        }

        bool foundProtected = false;

        packed.foreachTag([&](char tagName, std::string_view){
            if (tagName == '-') {
                foundProtected = true;
                return false;
            }
            return true;
        });

        if (foundProtected) {
            // NIP-70 protected events must be rejected unless published by an authenticated public key
            // that matches the event author, so we do all the AUTH flow here

            auto idHex = to_hex(packed.id());

            if (!cfg().relay__auth__enabled) {
                LI << "[" << connId << "] Protected event and auth disabled, rejecting: " << idHex;
                sendOKResponse(connId, idHex, false, "blocked: event marked as protected");
                return;
            }

            if (cfg().relay__auth__serviceUrl.empty()) {
                // If we don't have a serviceUrl, just fail
                LI << "[" << connId << "] Protected event and no serviceUrl configured, rejecting: " << idHex;
                sendOKResponse(connId, idHex, false, "blocked: event marked as protected");
                return;
            }

            auto it = rsctx.connIdToAuthStatus.find(connId);
            if (it == rsctx.connIdToAuthStatus.end()) {
                // we haven't sent an AUTH event for this, so first we generate a challenge for this connection
                auto challenge = rsctx.challengeGenerator.get();
                rsctx.connIdToAuthStatus.emplace(connId, challenge);

                LI << "[" << connId << "] Protected event, requesting AUTH: " << idHex;
                sendAuthChallenge(connId, challenge);
                sendOKResponse(connId, idHex, false, "auth-required: event marked as protected");
                return;
            }

            const auto &as = it->second;

            if (!as.isAuthed()) {
                // not authenticated
                LI << "[" << connId << "] Protected event, AUTH already requested: " << idHex;
                sendOKResponse(connId, idHex, false, "auth-required: event marked as protected");
                return;
            } else if (as.authed != packed.pubkey()) {
                // authenticated as someone else
                sendOKResponse(connId, idHex, false, "restricted: must be published by the author");
                return;
            }

            // otherwise we proceed to accept the event
            authedPubkey = as.authed;
        }
    }

    {
        auto existing = lookupEventById(txn, packed.id());
        if (existing) {
            auto hexId = to_hex(packed.id());
            LI << "[" << connId << "] Duplicate event, skipping: " << hexId;
            sendOKResponse(connId, hexId, true, "duplicate: have this event");
            return;
        }
    }

    output.emplace_back(MsgWriter{MsgWriter::AddEvent{connId, std::move(ipAddr), std::move(packedStr), std::move(jsonStr), authedPubkey}});
}

void RelayServer::ingesterProcessReq(lmdb::txn &txn, RelayServerCtx &rsctx, uint64_t connId, const tao::json::value &arr, bool countOnly, std::string &outSubIdStr) {
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
        rsctx.filterValidator.validate(filterGroup);
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

void RelayServer::ingesterProcessAuth(RelayServerCtx &rsctx, uint64_t connId, const tao::json::value &eventJson) {
    if (cfg().relay__auth__serviceUrl.empty()) throw herr("relay needs serviceUrl to be configured before AUTH can work");

    std::string packedStr, jsonStr;
    parseAndVerifyEvent(eventJson, rsctx.secpCtx, true, true, packedStr, jsonStr);

    PackedEventView packed(packedStr);

    if (packed.kind() != 22242) throw herr("wrong event kind, expected 22242");

    auto it = rsctx.connIdToAuthStatus.find(connId);
    if (it == rsctx.connIdToAuthStatus.end()) throw herr("no auth status available for connection");

    auto &as = it->second;

    if (as.isAuthed()) throw herr("already authenticated");

    bool foundChallenge = false;
    bool foundCorrectRelayUrl = false;

    // normalize URL: removes ws://, wss://, http://, https:// and strips any trailing slashes
    auto normalizeRelayUrl = [](std::string_view url) -> std::string_view {
        auto pos = url.find("://");

        if (pos != std::string_view::npos) url.remove_prefix(pos + 3);
        while (!url.empty() && url.back() == '/') url.remove_suffix(1);

        return url;
    };

    auto expectedRelay = normalizeRelayUrl(cfg().relay__auth__serviceUrl);

    for (const auto &tagj : eventJson.at("tags").get_array()) {
        const auto &tag = tagj.get_array();
        if (tag.size() < 2) continue;
        const auto name = tag[0].as<std::string_view>();
        const auto value = tag[1].as<std::string_view>();
        if (name == "relay" && normalizeRelayUrl(value) == expectedRelay) {
            foundCorrectRelayUrl = true;
        } else if (name == "challenge" && value == as.challengeSv()) {
            foundChallenge = true;
        }
    }

    if (!foundChallenge) throw herr("challenge string mismatch");
    if (!foundCorrectRelayUrl) throw herr("incorrect or missing relay tag, expected: " + cfg().relay__auth__serviceUrl);

    // set the connection as authenticated with this pubkey
    as.authed = packed.pubkey();

    LI << "[" << connId << "] AUTHed as " << to_hex(packed.pubkey());
    sendOKResponse(connId, to_hex(packed.id()), true, "successfully authenticated");
}

void RelayServer::ingesterProcessNegentropy(lmdb::txn &txn, uint64_t connId, const tao::json::value &arr) {
    const auto &subscriptionStr = jsonGetString(arr[1], "NEG-OPEN subscription id was not a string");

    if (arr.at(0) == "NEG-OPEN") {
        if (arr.get_array().size() < 4) throw herr("negentropy query missing elements");

        auto maxFilterLimit = cfg().relay__negentropy__maxSyncEvents + 1;

        auto filterJson = arr.at(2);
        if (!filterJson.is_object()) throw herr("negentropy filter must be an object");

        NostrFilterGroup filter = NostrFilterGroup::unwrapped(filterJson, maxFilterLimit);
        Subscription sub(connId, subscriptionStr, std::move(filter));

        filterJson.get_object().erase("since");
        filterJson.get_object().erase("until");
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
