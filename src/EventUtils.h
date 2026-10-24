#pragma once

#include <string_view>
#include <string>
#include <tuple>

#include "golpe.h"


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


// Tag values can be up to events.maxTagValSize (1024 by default), which is
// larger than LMDB's maximum key size. Use a fixed-size hash as the index key
// so all tag values can be indexed without changing LMDB's key-size limit.
inline std::string makeTagIndexKey(char tagName, std::string_view tagVal) {
    std::string buf;
    buf.reserve(1 + tagVal.size());
    buf += tagName;
    buf += tagVal;
    return sha256(buf).str();
}

// Replaceable events are keyed by pubkey + d-tag. Hash this as well so long
// d-tag values do not collide with the empty virtual d-tag or overflow LMDB
// key limits.
inline std::string makeReplaceIndexKey(std::string_view pubkey, std::string_view dTag) {
    std::string buf;
    buf.reserve(pubkey.size() + dTag.size());
    buf += pubkey;
    buf += dTag;
    return sha256(buf).str();
}

inline std::tuple<uint64_t, std::string, std::string> parseATag(std::string_view input) {
    size_t firstColon = input.find(':');
    if (firstColon == std::string::npos) throw herr("parse error");

    std::string_view kindStr = input.substr(0, firstColon);
    size_t pos;
    uint64_t kind = std::stoull(std::string(kindStr), &pos);
    if (pos != kindStr.size()) throw herr("parse error");

    size_t secondColon = input.find(':', firstColon + 1);
    if (secondColon == std::string::npos) throw herr("parse error");

    std::string_view pubkeyStr = input.substr(firstColon + 1, secondColon - firstColon - 1);
    if (pubkeyStr.size() != 64) throw herr("parse error");

    std::string dTag = std::string(input.substr(secondColon + 1));

    return {kind, hoytech::from_hex(pubkeyStr), std::move(dTag)};
}
