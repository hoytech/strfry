#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <algorithm>


struct HyperLogLog {
    uint8_t registers[256] = {};

    void addPubkeyBytes(const uint8_t *pubkey, int offset) {
        const uint8_t *x = pubkey + offset;
        uint8_t ri = x[0]; // register index

        // Build big-endian uint64 via explicit shifting (no endian-dependent casts)
        uint64_t w = (uint64_t)x[0] << 56 | (uint64_t)x[1] << 48 | (uint64_t)x[2] << 40 | (uint64_t)x[3] << 32
                   | (uint64_t)x[4] << 24 | (uint64_t)x[5] << 16 | (uint64_t)x[6] << 8  | (uint64_t)x[7];

        uint8_t zeroBits = clz56(w) + 1;

        if (zeroBits > registers[ri]) {
            registers[ri] = zeroBits;
        }
    }

    std::string encodeHex() const {
        static const char hexChars[] = "0123456789abcdef";
        std::string out;
        out.resize(512);
        for (int i = 0; i < 256; i++) {
            out[i * 2]     = hexChars[registers[i] >> 4];
            out[i * 2 + 1] = hexChars[registers[i] & 0x0F];
        }
        return out;
    }

  private:
    static uint8_t clz56(uint64_t x) {
        uint8_t c = 0;
        for (uint64_t m = uint64_t(1) << 55; (m & x) == 0 && m != 0; m >>= 1) {
            c++;
        }
        return c;
    }
};
