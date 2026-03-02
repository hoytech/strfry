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

inline std::string decodeBech32Simple(std::string_view v) {
    auto res = bech32::decode(std::string(v));

    if (res.encoding == bech32::Encoding::INVALID) throw herr("invalid bech32");
    else if (res.encoding == bech32::Encoding::BECH32M) throw herr("got bech32m");

    std::vector<uint8_t> out;
    if (!convertbits<5, 8, false>(out, res.data)) throw herr("convertbits failed");
    if (out.size() != 32) throw herr("unexpected size from bech32");

    return std::string((char*)out.data(), out.size());
}
