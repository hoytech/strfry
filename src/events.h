#pragma once

#include <secp256k1_schnorrsig.h>

#include "golpe.h"

#include "PackedEvent.h"
#include "Decompressor.h"




inline bool isReplaceableKind(uint64_t kind) {
    return (
        kind == 0 ||
        kind == 3 ||
        kind == 41 ||
        (kind >= 10'000 && kind < 20'000)
    );
}

inline bool isParamReplaceableKind(uint64_t kind) {
    return (
        (kind >= 30'000 && kind < 40'000)
    );
}

inline bool isEphemeralKind(uint64_t kind) {
    return (
        (kind >= 20'000 && kind < 30'000)
    );
}




std::string nostrJsonToPackedEvent(const tao::json::value &v);
std::string nostrHash(const tao::json::value &origJson);

bool verifySig(secp256k1_context* ctx, std::string_view sig, std::string_view hash, std::string_view pubkey);
void verifyNostrEvent(secp256k1_context *secpCtx, PackedEventView packed, const tao::json::value &origJson);
void verifyNostrEventJsonSize(std::string_view jsonStr);
void verifyEventTimestamp(PackedEventView packed);

void parseAndVerifyEvent(const tao::json::value &origJson, secp256k1_context *secpCtx, bool verifyMsg, bool verifyTime, std::string &packedStr, std::string &jsonStr);



std::optional<defaultDb::environment::View_Event> lookupEventById(lmdb::txn &txn, std::string_view id);
defaultDb::environment::View_Event lookupEventByLevId(lmdb::txn &txn, uint64_t levId); // throws if can't find
uint64_t getMostRecentLevId(lmdb::txn &txn);
std::string_view decodeEventPayload(lmdb::txn &txn, Decompressor &decomp, std::string_view raw, uint32_t *outDictId, size_t *outCompressedSize);
std::string_view getEventJson(lmdb::txn &txn, Decompressor &decomp, uint64_t levId);
std::string_view getEventJson(lmdb::txn &txn, Decompressor &decomp, uint64_t levId, std::string_view eventPayload);




enum class EventSourceType {
    None = 0,
    IP4 = 1,
    IP6 = 2,
    Import = 3,
    Stream = 4,
    Sync = 5,
    Stored = 6,
};

inline std::string eventSourceTypeToStr(EventSourceType t) {
    if (t == EventSourceType::IP4) return "IP4";
    else if (t == EventSourceType::IP6) return "IP6";
    else if (t == EventSourceType::Import) return "Import";
    else if (t == EventSourceType::Stream) return "Stream";
    else if (t == EventSourceType::Sync) return "Sync";
    else return "?";
}



enum class EventWriteStatus {
    Pending,
    Written,
    Duplicate,
    Replaced,
    Deleted,
};


struct EventToWrite {
    std::string packedStr;
    std::string jsonStr;
    void *userData = nullptr;
    EventWriteStatus status = EventWriteStatus::Pending;
    uint64_t levId = 0;

    EventToWrite() {}

    EventToWrite(std::string packedStr, std::string jsonStr, void *userData = nullptr) : packedStr(packedStr), jsonStr(jsonStr), userData(userData) {
    }

    std::string_view id() {
        return PackedEventView(packedStr).id();
    }

    uint64_t createdAt() {
        return PackedEventView(packedStr).created_at();
    }
};


void writeEvents(lmdb::txn &txn, std::vector<EventToWrite> &evs, uint64_t logLevel = 1);
bool deleteEvent(lmdb::txn &txn, uint64_t levId);
