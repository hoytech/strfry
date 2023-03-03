#pragma once

#include <string>
#include <string_view>

#include "golpe.h"


inline unsigned char getByte(std::string_view &encoded){
    if (encoded.size() < 1) throw herr("parse ends prematurely");
    auto res = static_cast<unsigned char>(encoded[0]);
    encoded = encoded.substr(1);
    return res;
};

inline std::string getBytes(std::string_view &encoded, size_t n) {
    if (encoded.size() < n) throw herr("parse ends prematurely");
    auto res = encoded.substr(0, n);
    encoded = encoded.substr(n);
    return std::string(res);
};

inline std::string encodeVarInt(uint64_t n) {
    if (n == 0) return std::string(1, '\0');

    std::string o;

    while (n) {
        o.push_back(static_cast<unsigned char>(n & 0x7F));
        n >>= 7;
    }

    std::reverse(o.begin(), o.end());

    for (size_t i = 0; i < o.size() - 1; i++) {
        o[i] |= 0x80;
    }

    return o;
}

inline uint64_t decodeVarInt(std::string_view &encoded) {
    uint64_t res = 0;

    while (1) {
        if (encoded.size() == 0) throw herr("premature end of varint");
        uint64_t byte = encoded[0];
        encoded = encoded.substr(1);
        res = (res << 7) | (byte & 0b0111'1111);
        if ((byte & 0b1000'0000) == 0) break;
    }

    return res;
}
