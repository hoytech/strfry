#include <openssl/sha.h>

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

    if (v.at("tags").get_array().size() > cfg().events__maxNumTags) throw herr("too many tags: ", v.at("tags").get_array().size());
    for (auto &tagArr : v.at("tags").get_array()) {
        auto &tag = tagArr.get_array();
        if (tag.size() < 2) throw herr("too few fields in tag");

        auto tagName = tag.at(0).get_string();
        if (tagName.size() != 1) continue; // only single-char tags need indexing

        auto tagVal = tag.at(1).get_string();

        if (tagName == "e" || tagName == "p") {
            tagVal = from_hex(tagVal, false);
            if (tagVal.size() != 32) throw herr("unexpected size for fixed-size tag");

            tagsFixed32.emplace_back(NostrIndex::CreateTagFixed32(builder,
                (uint8_t)tagName[0],
                (NostrIndex::Fixed32Bytes*)tagVal.data()
            ));
        } else {
            if (tagVal.size() < 1 || tagVal.size() > cfg().events__maxTagValSize) throw herr("tag val too small/large: ", tagVal.size());

            tagsGeneral.emplace_back(NostrIndex::CreateTagGeneral(builder,
                (uint8_t)tagName[0],
                builder.CreateVector((uint8_t*)tagVal.data(), tagVal.size())
            ));
        }
    }

    // Create flatbuffer

    auto eventPtr = NostrIndex::CreateEvent(builder,
        (NostrIndex::Fixed32Bytes*)id.data(),
        (NostrIndex::Fixed32Bytes*)pubkey.data(),
        created_at,
        kind,
        builder.CreateVector<flatbuffers::Offset<NostrIndex::TagGeneral>>(tagsGeneral),
        builder.CreateVector<flatbuffers::Offset<NostrIndex::TagFixed32>>(tagsFixed32)
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

    uint64_t earliest = now - (isEphemeralEvent(flat->kind()) ? cfg().events__rejectEphemeralEventsOlderThanSeconds : cfg().events__rejectEventsOlderThanSeconds);
    uint64_t latest = now + cfg().events__rejectEventsNewerThanSeconds;

    if (ts < earliest) throw herr("created_at too early");
    if (ts > latest) throw herr("created_at too late");
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

uint64_t getMostRecentLevId(lmdb::txn &txn) {
    uint64_t levId = 0;

    env.foreach_Event(txn, [&](auto &ev){
        levId = ev.primaryKeyId;
        return false;
    }, true);

    return levId;
}

std::string_view getEventJson(lmdb::txn &txn, uint64_t levId) {
    std::string_view raw;
    bool found = env.dbi_EventPayload.get(txn, lmdb::to_sv<uint64_t>(levId), raw);
    if (!found) throw herr("couldn't find leaf node in quadrable, corrupted DB?");
    return raw.substr(1);
}



void writeEvents(lmdb::txn &txn, quadrable::Quadrable &qdb, std::vector<EventToWrite> &evs) {
    std::sort(evs.begin(), evs.end(), [](auto &a, auto &b) { return a.quadKey < b.quadKey; });

    auto changes = qdb.change();

    std::vector<uint64_t> levIdsToDelete;

    for (size_t i = 0; i < evs.size(); i++) {
        auto &ev = evs[i];

        const NostrIndex::Event *flat = flatbuffers::GetRoot<NostrIndex::Event>(ev.flatStr.data());

        if (lookupEventById(txn, sv(flat->id())) || (i != 0 && ev.quadKey == evs[i-1].quadKey)) {
            ev.status = EventWriteStatus::Duplicate;
            continue;
        }

        if (env.lookup_Event__deletion(txn, std::string(sv(flat->id())) + std::string(sv(flat->pubkey())))) {
            ev.status = EventWriteStatus::Deleted;
            continue;
        }

        if (isReplaceableEvent(flat->kind())) {
            auto searchKey = makeKey_StringUint64Uint64(sv(flat->pubkey()), flat->kind(), MAX_U64);
            uint64_t otherLevId = 0;

            env.generic_foreachFull(txn, env.dbi_Event__pubkeyKind, searchKey, lmdb::to_sv<uint64_t>(MAX_U64), [&](auto k, auto v) {
                ParsedKey_StringUint64Uint64 parsedKey(k);
                if (parsedKey.s == sv(flat->pubkey()) && parsedKey.n1 == flat->kind()) {
                    if (parsedKey.n2 < flat->created_at()) {
                        otherLevId = lmdb::from_sv<uint64_t>(v);
                    } else {
                        ev.status = EventWriteStatus::Replaced;
                    }
                }
                return false;
            }, true);

            if (otherLevId) {
                auto otherEv = env.lookup_Event(txn, otherLevId);
                if (!otherEv) throw herr("missing event from index, corrupt DB?");
                changes.del(flatEventToQuadrableKey(otherEv->flat_nested()));
                levIdsToDelete.push_back(otherLevId);
            }
        }

        if (flat->kind() == 5) {
            // Deletion event, delete all referenced events
            for (const auto &tagPair : *(flat->tagsFixed32())) {
                if (tagPair->key() == 'e') {
                    auto otherEv = lookupEventById(txn, sv(tagPair->val()));
                    if (otherEv && sv(otherEv->flat_nested()->pubkey()) == sv(flat->pubkey())) {
                        LI << "Deleting event. id=" << to_hex(sv(tagPair->val()));
                        changes.del(flatEventToQuadrableKey(otherEv->flat_nested()));
                        levIdsToDelete.push_back(otherEv->primaryKeyId);
                    }
                }
            }
        }

        if (ev.status == EventWriteStatus::Pending) changes.put(ev.quadKey, "");
    }

    changes.apply(txn);

    for (auto levId : levIdsToDelete) {
        env.delete_Event(txn, levId);
        env.dbi_EventPayload.del(txn, lmdb::to_sv<uint64_t>(levId));
    }

    std::string tmpBuf;

    for (auto &ev : evs) {
        if (ev.status == EventWriteStatus::Pending) {
            ev.levId = env.insert_Event(txn, ev.receivedAt, ev.flatStr);

            tmpBuf.clear();
            tmpBuf += '\x00';
            tmpBuf += ev.jsonStr;
            env.dbi_EventPayload.put(txn, lmdb::to_sv<uint64_t>(ev.levId), tmpBuf);

            ev.status = EventWriteStatus::Written;
        }
    }
}
