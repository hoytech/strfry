#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

#include "golpe.h"


struct Bytes32 {
    uint8_t buf[32];

    Bytes32() {
        ::memset(buf, '\0', sizeof(buf));
    }

    Bytes32(std::string_view s) {
        if (s.size() != 32) throw herr("invalid length for Bytes32");
        ::memcpy(buf, s.data(), 32);
    }

    std::string str() const {
        return std::string((char*)buf, 32);
    }

    std::string_view sv() const {
        return std::string_view((char*)buf, 32);
    }

    bool isNull() const {
        static constexpr uint8_t zeroBuf[32] = {};
        return ::memcmp(buf, zeroBuf, 32) == 0;
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
            uint64_t words[4];
            ::memcpy(words, b.buf, sizeof(words));
            return size_t(words[0] ^ words[1] ^ words[2] ^ words[3]);
        }
    };
}
