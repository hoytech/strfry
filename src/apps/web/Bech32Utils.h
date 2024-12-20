#pragma once

#include <vector>

#include "bech32.h"


/** Convert from one power-of-2 number base to another. */
template<int frombits, int tobits, bool pad>
bool convertbits(std::vector<uint8_t>& out, const std::vector<uint8_t>& in) {
    int acc = 0;
    int bits = 0;
    const int maxv = (1 << tobits) - 1;
    const int max_acc = (1 << (frombits + tobits - 1)) - 1;
    for (size_t i = 0; i < in.size(); ++i) {
        int value = in[i];
        acc = ((acc << frombits) | value) & max_acc;
        bits += frombits;
        while (bits >= tobits) {
            bits -= tobits;
            out.push_back((acc >> bits) & maxv);
        }
    }
    if (pad) {
        if (bits) out.push_back((acc << (tobits - bits)) & maxv);
    } else if (bits >= frombits || ((acc << (tobits - bits)) & maxv)) {
        return false;
    }
    return true;
}

inline std::string encodeBech32Simple(const std::string &hrp, std::string_view v) {
    if (v.size() != 32) throw herr("expected bech32 argument to be 32 bytes");

    std::vector<uint8_t> values(32, '\0');
    memcpy(values.data(), v.data(), 32);

    std::vector<uint8_t> values5;
    convertbits<8, 5, true>(values5, values);

    return bech32::encode(hrp, values5, bech32::Encoding::BECH32);
}

inline std::string decodeBech32Raw(std::string_view v) {
    auto res = bech32::decode(std::string(v));

    if (res.encoding == bech32::Encoding::INVALID) throw herr("invalid bech32");
    else if (res.encoding == bech32::Encoding::BECH32M) throw herr("got bech32m");

    std::vector<uint8_t> out;
    if (!convertbits<5, 8, false>(out, res.data)) throw herr("convertbits failed");

    return std::string((char*)out.data(), out.size());
}

inline std::string decodeBech32Simple(std::string_view v) {
    auto out = decodeBech32Raw(v);
    if (out.size() != 32) throw herr("unexpected size from bech32");
    return out;
}

inline uint8_t getByte(std::string_view &encoded) {
    if (encoded.size() < 1) throw herr("parse ends prematurely");
    uint8_t output = encoded[0];
    encoded = encoded.substr(1);
    return output;
}

inline std::string getBytes(std::string_view &encoded, size_t n) {
    if (encoded.size() < n) throw herr("parse ends prematurely");
    auto res = encoded.substr(0, n);
    encoded = encoded.substr(n);
    return std::string(res);
};

inline std::string decodeBech32GetSpecial(std::string_view origStr) {
    auto decoded = decodeBech32Raw(origStr);
    std::string_view s(decoded);

    while (s.size()) {
        auto tag = getByte(s);
        auto len = getByte(s);
        auto val = getBytes(s, len);
        if (tag == 0) {
            if (len != 32) throw herr("unexpected size from bech32 special");
            return std::string(val);
        }
    }

    throw herr("no special tag found");
}
