#pragma once

#include <cstring>
#include <string_view>

#include "golpe.h"


struct Bytes32 {
    uint8_t buf[32];

    Bytes32(std::string_view s) {
        if (s.size() != 32) throw herr("invalid length for Bytes32");
        memcpy(buf, s.data(), 32);
    }

    std::string_view sv() const {
        return std::string_view((char*)buf, 32);
    }

    int operator <=>(const Bytes32& rhs) const {
        return memcmp(buf, rhs.buf, 32);
    }

    bool operator==(const Bytes32 &o) const {
        return std::memcmp(buf, o.buf, sizeof(buf)) == 0;
    }
};


// Inject specialization of std::hash, so we can use it in a flat_hash_set
namespace std {
    template<> struct hash<Bytes32> {
        std::size_t operator()(Bytes32 const &b) const {
            static_assert(sizeof(b.buf) == 32);
            uint64_t *p = (size_t*)&b.buf;
            return size_t(p[0] ^ p[1] ^ p[2] ^ p[3]);
        }
    };
}
