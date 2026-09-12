#pragma once

#include <bit>
#include <cstring>
#include <string_view>

#include "golpe.h"


// PackedEvent (summary of indexable data in a nostr event)
//   0: id (32)
//  32: pubkey (32)
//  64: created_at (8)
//  72: kind (8)
//  80: expiration (8)
//  88: tags[] (variable)
//
// each tag:
//   0: tag char (1)
//   1: length (1)
//   2: value (variable)

// Fixed-header layout used by the --fried import/export byte-swapper below.
static constexpr size_t PACKED_EVENT_HEADER_SIZE    = 88;
static constexpr size_t PACKED_EVENT_CREATED_AT_OFF = 64;
static constexpr size_t PACKED_EVENT_KIND_OFF       = 72;
static constexpr size_t PACKED_EVENT_EXPIRATION_OFF = 80;

// Byte-swap the three uint64_t fields (created_at, kind, expiration) in the
// fixed header of a packed event between native byte order and little-endian.
// The --fried wire format is defined as always little-endian, so callers on
// big-endian systems invoke this to serialise/deserialise.
//
// On little-endian hosts this is a compile-time no-op (the whole body is
// discarded by `if constexpr`), giving zero runtime cost on x86/ARM-LE.
//
// Callers must ensure `packed.size() >= PACKED_EVENT_HEADER_SIZE` before
// invoking this — untrusted input (e.g. hex-decoded from an imported file)
// may be shorter.
inline void friedSwapEndianInPlace(std::string &packed) {
    if constexpr (std::endian::native != std::endian::little) {
        for (size_t offset : {PACKED_EVENT_CREATED_AT_OFF, PACKED_EVENT_KIND_OFF, PACKED_EVENT_EXPIRATION_OFF}) {
            uint64_t val;
            std::memcpy(&val, packed.data() + offset, 8);
            val = __builtin_bswap64(val);
            std::memcpy(packed.data() + offset, &val, 8);
        }
    } else {
        (void)packed;
    }
}

struct PackedEventView {
    std::string_view buf;

    PackedEventView(const std::string &str) : buf(std::string_view(str)) {
        if (buf.size() < 88) throw hoytech::error("PackedEventView too short");
    }

    PackedEventView(std::string_view sv) : buf(sv) {
        if (buf.size() < 88) throw hoytech::error("PackedEventView too short");
    }

    std::string_view id() const {
        return buf.substr(0, 32);
    }

    std::string_view pubkey() const {
        return buf.substr(32, 32);
    }

    uint64_t created_at() const {
        return lmdb::from_sv<uint64_t>(buf.substr(64, 8));
    }

    uint64_t kind() const {
        return lmdb::from_sv<uint64_t>(buf.substr(72, 8));
    }

    uint64_t expiration() const {
        return lmdb::from_sv<uint64_t>(buf.substr(80, 8));
    }

    void foreachTag(const std::function<bool(char, std::string_view)> &cb) const {
        std::string_view b = buf.substr(88);

        while (b.size() >= 2) {
            char tagName = b[0];
            size_t tagLen = (uint8_t)b[1];
            
            if (tagLen > b.size() - 2) break;

            bool ret = cb(tagName, b.substr(2, tagLen));
            if (!ret) break;
            b = b.substr(2 + tagLen);
        }
    }
};

struct PackedEventTagBuilder {
    std::string buf;

    void add(char tagKey, std::string_view tagVal) {
        if (tagVal.size() > 255) throw hoytech::error("tagVal too long");

        buf += tagKey;
        buf += (unsigned char) tagVal.size();
        buf += tagVal;
    }
};

struct PackedEventBuilder {
    std::string buf;

    PackedEventBuilder(std::string_view id, std::string_view pubkey, uint64_t created_at, uint64_t kind, uint64_t expiration, const PackedEventTagBuilder &tagBuilder) {
        if (id.size() != 32) throw hoytech::error("unexpected id size");
        if (pubkey.size() != 32) throw hoytech::error("unexpected pubkey size");

        buf.reserve(88 + tagBuilder.buf.size());

        buf += id;
        buf += pubkey;
        buf += lmdb::to_sv<uint64_t>(created_at);
        buf += lmdb::to_sv<uint64_t>(kind);
        buf += lmdb::to_sv<uint64_t>(expiration);
        buf += tagBuilder.buf;
    }
};
