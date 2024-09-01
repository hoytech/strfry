#pragma once

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

    void foreachTag(const std::function<bool(char, std::string_view)> &cb) {
        std::string_view b = buf.substr(88);

        while (b.size()) {
            bool ret = cb(b[0], b.substr(2, (size_t)b[1]));
            if (!ret) break;
            b = b.substr(2 + b[1]);
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
