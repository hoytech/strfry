#include <openssl/sha.h>
#include <negentropy.h>

#include "events.h"


std::string nostrJsonToFlat(const tao::json::value &v) {
    flatbuffers::FlatBufferBuilder builder; // FIXME: pre-allocate size approximately the same as orig JSON?

    // Extract values from JSON, add strings to builder

    auto id = from_hex(v.at("id").get_string(), false);
    auto pubkey = from_hex(v.at("pubkey").get_string(), false);
    uint64_t created_at = v.at("created_at").get_unsigned();
    uint64_t kind = v.at("kind").get_unsigned();

    if (id.size() != 32) throw herr("unexpected id size");
    if (pubkey.size() != 32) throw herr("unexpected pubkey size");

    std::vector<flatbuffers::Offset<NostrIndex::TagGeneral>> tagsGeneral;
    std::vector<flatbuffers::Offset<NostrIndex::TagFixed32>> tagsFixed32;

    uint64_t expiration = 0;

    if (isReplaceableKind(kind)) {
        // Prepend virtual d-tag
        tagsGeneral.emplace_back(NostrIndex::CreateTagGeneral(builder,
            'd',
            builder.CreateVector((uint8_t*)"", 0)
        ));
    }

    if (v.at("tags").get_array().size() > cfg().events__maxNumTags) throw herr("too many tags: ", v.at("tags").get_array().size());
    for (auto &tagArr : v.at("tags").get_array()) {
        auto &tag = tagArr.get_array();
        if (tag.size() < 1) throw herr("too few fields in tag");

        auto tagName = tag.at(0).get_string();
        auto tagVal = tag.size() >= 2 ? tag.at(1).get_string() : "";

        if (tagName == "e" || tagName == "p") {
            tagVal = from_hex(tagVal, false);
            if (tagVal.size() != 32) throw herr("unexpected size for fixed-size tag");

            tagsFixed32.emplace_back(NostrIndex::CreateTagFixed32(builder,
                (uint8_t)tagName[0],
                (NostrIndex::Fixed32Bytes*)tagVal.data()
            ));
        } else if (tagName == "expiration") {
            if (expiration == 0) {
                expiration = parseUint64(tagVal);
                if (expiration < 100) throw herr("invalid expiration");
            }
        } else if (tagName.size() == 1) {
            if (tagVal.size() > cfg().events__maxTagValSize) throw herr("tag val too large: ", tagVal.size());

            if (tagVal.size() <= MAX_INDEXED_TAG_VAL_SIZE) {
                tagsGeneral.emplace_back(NostrIndex::CreateTagGeneral(builder,
                    (uint8_t)tagName[0],
                    builder.CreateVector((uint8_t*)tagVal.data(), tagVal.size())
                ));
            }
        }
    }

    if (isParamReplaceableKind(kind)) {
        // Append virtual d-tag
        tagsGeneral.emplace_back(NostrIndex::CreateTagGeneral(builder,
            'd',
            builder.CreateVector((uint8_t*)"", 0)
        ));
    }

    if (isEphemeralKind(kind)) {
        expiration = 1;
    }

    // Create flatbuffer

    auto eventPtr = NostrIndex::CreateEvent(builder,
        (NostrIndex::Fixed32Bytes*)id.data(),
        (NostrIndex::Fixed32Bytes*)pubkey.data(),
        created_at,
        kind,
        builder.CreateVector<flatbuffers::Offset<NostrIndex::TagGeneral>>(tagsGeneral),
        builder.CreateVector<flatbuffers::Offset<NostrIndex::TagFixed32>>(tagsFixed32),
        expiration
    );

    builder.Finish(eventPtr);

    return std::string(reinterpret_cast<char*>(builder.GetBufferPointer()), builder.GetSize());
}

std::string nostrHash(const tao::json::value &origJson) {
    tao::json::value arr = tao::json::empty_array;

    arr.emplace_back(0);

    arr.emplace_back(origJson.at("pubkey"));
    arr.emplace_back(origJson.at("created_at"));
    arr.emplace_back(origJson.at("kind"));
    arr.emplace_back(origJson.at("tags"));
    arr.emplace_back(origJson.at("content"));

    std::string encoded = tao::json::to_string(arr);

    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<unsigned char*>(encoded.data()), encoded.size(), hash);

    return std::string(reinterpret_cast<char*>(hash), SHA256_DIGEST_LENGTH);
}

bool verifySig(secp256k1_context* ctx, std::string_view sig, std::string_view hash, std::string_view pubkey) {
    if (sig.size() != 64 || hash.size() != 32 || pubkey.size() != 32) throw herr("verify sig: bad input size");

    secp256k1_xonly_pubkey pubkeyParsed;
    if (!secp256k1_xonly_pubkey_parse(ctx, &pubkeyParsed, (const uint8_t*)pubkey.data())) throw herr("verify sig: bad pubkey");

    return secp256k1_schnorrsig_verify(
                ctx,
                (const uint8_t*)sig.data(),
                (const uint8_t*)hash.data(),
#ifdef SECP256K1_SCHNORRSIG_EXTRAPARAMS_INIT // old versions of libsecp256k1 didn't take a msg size param, this define added just after
                hash.size(),
#endif
                &pubkeyParsed
    );
}

void verifyNostrEvent(secp256k1_context *secpCtx, const NostrIndex::Event *flat, const tao::json::value &origJson) {
    auto hash = nostrHash(origJson);
    if (hash != sv(flat->id())) throw herr("bad event id");

    bool valid = verifySig(secpCtx, from_hex(origJson.at("sig").get_string(), false), sv(flat->id()), sv(flat->pubkey()));
    if (!valid) throw herr("bad signature");
}

void verifyNostrEventJsonSize(std::string_view jsonStr) {
    if (jsonStr.size() > cfg().events__maxEventSize) throw herr("event too large: ", jsonStr.size());
}

void verifyEventTimestamp(const NostrIndex::Event *flat) {
    auto now = hoytech::curr_time_s();
    auto ts = flat->created_at();

    uint64_t earliest = now - (flat->expiration() == 1 ? cfg().events__rejectEphemeralEventsOlderThanSeconds : cfg().events__rejectEventsOlderThanSeconds);
    uint64_t latest = now + cfg().events__rejectEventsNewerThanSeconds;

    // overflows
    if (earliest > now) earliest = 0;
    if (latest < now) latest = MAX_U64 - 1;

    if (ts < earliest) throw herr("created_at too early");
    if (ts > latest) throw herr("created_at too late");

    if (flat->expiration() > 1 && flat->expiration() <= now) throw herr("event expired");
}

void parseAndVerifyEvent(const tao::json::value &origJson, secp256k1_context *secpCtx, bool verifyMsg, bool verifyTime, std::string &flatStr, std::string &jsonStr) {
    flatStr = nostrJsonToFlat(origJson);
    auto *flat = flatbuffers::GetRoot<NostrIndex::Event>(flatStr.data());
    if (verifyTime) verifyEventTimestamp(flat);
    if (verifyMsg) verifyNostrEvent(secpCtx, flat, origJson);

    // Build new object to remove unknown top-level fields from json
    jsonStr = tao::json::to_string(tao::json::value({
        { "content", &origJson.at("content") },
        { "created_at", &origJson.at("created_at") },
        { "id", &origJson.at("id") },
        { "kind", &origJson.at("kind") },
        { "pubkey", &origJson.at("pubkey") },
        { "sig", &origJson.at("sig") },
        { "tags", &origJson.at("tags") },
    }));

    if (verifyMsg) verifyNostrEventJsonSize(jsonStr);
}






std::optional<defaultDb::environment::View_Event> lookupEventById(lmdb::txn &txn, std::string_view id) {
    std::optional<defaultDb::environment::View_Event> output;

    env.generic_foreachFull(txn, env.dbi_Event__id, makeKey_StringUint64(id, 0), lmdb::to_sv<uint64_t>(0), [&](auto k, auto v) {
        if (k.starts_with(id)) output = env.lookup_Event(txn, lmdb::from_sv<uint64_t>(v));
        return false;
    });

    return output;
}

defaultDb::environment::View_Event lookupEventByLevId(lmdb::txn &txn, uint64_t levId) {
    auto view = env.lookup_Event(txn, levId);
    if (!view) throw herr("unable to lookup event by levId");
    return *view;
}

uint64_t getMostRecentLevId(lmdb::txn &txn) {
    uint64_t levId = 0;

    env.foreach_Event(txn, [&](auto &ev){
        levId = ev.primaryKeyId;
        return false;
    }, true);

    return levId;
}


// Return result validity same as getEventJson(), see below

std::string_view decodeEventPayload(lmdb::txn &txn, Decompressor &decomp, std::string_view raw, uint32_t *outDictId, size_t *outCompressedSize) {
    if (raw.size() == 0) throw herr("empty event in EventPayload");

    if (raw[0] == '\x00') {
        if (outDictId) *outDictId = 0;
        return raw.substr(1);
    } else if (raw[0] == '\x01') {
        raw = raw.substr(1);
        if (raw.size() < 4) throw herr("EventPayload record too short to read dictId");
        uint32_t dictId = lmdb::from_sv<uint32_t>(raw.substr(0, 4));
        raw = raw.substr(4);

        decomp.reserve(cfg().events__maxEventSize);
        std::string_view buf = decomp.decompress(txn, dictId, raw);

        if (outDictId) *outDictId = dictId;
        if (outCompressedSize) *outCompressedSize = raw.size();
        return buf;
    } else {
        throw("Unexpected first byte in EventPayload");
    }
}

// Return result only valid until one of: next call to getEventJson/decodeEventPayload, write to/closing of txn, or any action on decomp object

std::string_view getEventJson(lmdb::txn &txn, Decompressor &decomp, uint64_t levId) {
    std::string_view eventPayload;

    bool found = env.dbi_EventPayload.get(txn, lmdb::to_sv<uint64_t>(levId), eventPayload);
    if (!found) throw herr("couldn't find event in EventPayload");

    return getEventJson(txn, decomp, levId, eventPayload);
}

std::string_view getEventJson(lmdb::txn &txn, Decompressor &decomp, uint64_t levId, std::string_view eventPayload) {
    return decodeEventPayload(txn, decomp, eventPayload, nullptr, nullptr);
}





bool deleteEvent(lmdb::txn &txn, uint64_t levId, negentropy::storage::BTreeLMDB &negentropyStorage) {
    auto view = env.lookup_Event(txn, levId);
    if (!view) return false;
    auto *flat = view->flat_nested();

    negentropyStorage.erase(flat->created_at(), sv(flat->id()));

    bool deleted = env.dbi_EventPayload.del(txn, lmdb::to_sv<uint64_t>(levId));
    env.delete_Event(txn, levId);

    return deleted;
}



void writeEvents(lmdb::txn &txn, std::vector<EventToWrite> &evs, uint64_t logLevel) {
    std::sort(evs.begin(), evs.end(), [](auto &a, auto &b) {
        auto aC = a.createdAt();
        auto bC = b.createdAt();
        if (aC == bC) return a.id() < b.id();
        return aC < bC;
    });

    std::vector<uint64_t> levIdsToDelete;
    std::string tmpBuf;

    negentropy::storage::BTreeLMDB negentropyStorage(txn, negentropyDbi, 0);

    for (size_t i = 0; i < evs.size(); i++) {
        auto &ev = evs[i];

        const NostrIndex::Event *flat = flatbuffers::GetRoot<NostrIndex::Event>(ev.flatStr.data());

        if (lookupEventById(txn, sv(flat->id())) || (i != 0 && ev.id() == evs[i-1].id())) {
            ev.status = EventWriteStatus::Duplicate;
            continue;
        }

        if (env.lookup_Event__deletion(txn, std::string(sv(flat->id())) + std::string(sv(flat->pubkey())))) {
            ev.status = EventWriteStatus::Deleted;
            continue;
        }

        {
            std::optional<std::string> replace;

            if (isReplaceableKind(flat->kind()) || isParamReplaceableKind(flat->kind())) {
                for (const auto &tagPair : *(flat->tagsGeneral())) {
                    auto tagName = (char)tagPair->key();
                    if (tagName != 'd') continue;
                    replace = std::string(sv(tagPair->val()));
                    break;
                }
            }

            if (replace) {
                auto searchStr = std::string(sv(flat->pubkey())) + *replace;
                auto searchKey = makeKey_StringUint64(searchStr, flat->kind());

                env.generic_foreachFull(txn, env.dbi_Event__replace, searchKey, lmdb::to_sv<uint64_t>(MAX_U64), [&](auto k, auto v) {
                    ParsedKey_StringUint64 parsedKey(k);
                    if (parsedKey.s == searchStr && parsedKey.n == flat->kind()) {
                        auto otherEv = lookupEventByLevId(txn, lmdb::from_sv<uint64_t>(v));

                        auto thisTimestamp = flat->created_at();
                        auto otherTimestamp = otherEv.flat_nested()->created_at();

                        if (otherTimestamp < thisTimestamp ||
                            (otherTimestamp == thisTimestamp && sv(flat->id()) < sv(otherEv.flat_nested()->id()))) {
                            if (logLevel >= 1) LI << "Deleting event (d-tag). id=" << to_hex(sv(otherEv.flat_nested()->id()));
                            levIdsToDelete.push_back(otherEv.primaryKeyId);
                        } else {
                            ev.status = EventWriteStatus::Replaced;
                        }
                    }

                    return false;
                }, true);
            }
        }

        if (flat->kind() == 5) {
            // Deletion event, delete all referenced events
            for (const auto &tagPair : *(flat->tagsFixed32())) {
                if (tagPair->key() == 'e') {
                    auto otherEv = lookupEventById(txn, sv(tagPair->val()));
                    if (otherEv && sv(otherEv->flat_nested()->pubkey()) == sv(flat->pubkey())) {
                        if (logLevel >= 1) LI << "Deleting event (kind 5). id=" << to_hex(sv(tagPair->val()));
                        levIdsToDelete.push_back(otherEv->primaryKeyId);
                    }
                }
            }
        }

        if (ev.status == EventWriteStatus::Pending) {
            ev.levId = env.insert_Event(txn, ev.receivedAt, ev.flatStr, (uint64_t)ev.sourceType, ev.sourceInfo);

            tmpBuf.clear();
            tmpBuf += '\x00';
            tmpBuf += ev.jsonStr;
            env.dbi_EventPayload.put(txn, lmdb::to_sv<uint64_t>(ev.levId), tmpBuf);

            negentropyStorage.insert(ev.createdAt(), ev.id());

            ev.status = EventWriteStatus::Written;

            // Deletions happen after event was written to ensure levIds are not reused

            for (auto levId : levIdsToDelete) deleteEvent(txn, levId, negentropyStorage);
            levIdsToDelete.clear();
        }

        if (levIdsToDelete.size()) throw herr("unprocessed deletion");
    }

    negentropyStorage.flush();
}
