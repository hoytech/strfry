#pragma once

#include <string>
#include <cstring>
#include <optional>

#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <openssl/crypto.h>

#include <hoytech/hex.h>
#include <hoytech/time.h>

#include "golpe.h"


struct SessionToken {
    static constexpr size_t SECRET_SIZE = 32;
    static constexpr size_t HMAC_SIZE = 32;
    static constexpr size_t PUBKEY_SIZE = 32;

    // Token binary layout: pubkey(32) + issuedAt(8) + expiresAt(8) + hmac(32) = 80 bytes
    static constexpr size_t DATA_SIZE = PUBKEY_SIZE + 8 + 8;
    static constexpr size_t TOKEN_SIZE = DATA_SIZE + HMAC_SIZE;

    static std::string generateSecret() {
        uint8_t buf[SECRET_SIZE];
        if (RAND_bytes(buf, SECRET_SIZE) != 1) throw herr("failed to generate random bytes for session secret");
        return std::string(reinterpret_cast<char*>(buf), SECRET_SIZE);
    }

    static std::string generateChallenge() {
        uint8_t buf[16];
        if (RAND_bytes(buf, sizeof(buf)) != 1) throw herr("failed to generate random bytes for challenge");
        return to_hex(std::string_view(reinterpret_cast<char*>(buf), sizeof(buf)));
    }

    static void computeHmac(const std::string &secret, const uint8_t *data, size_t dataLen, uint8_t *out) {
        unsigned int hmacLen = HMAC_SIZE;
        HMAC(EVP_sha256(),
             reinterpret_cast<const unsigned char*>(secret.data()), secret.size(),
             data, dataLen,
             out, &hmacLen);
    }

    static std::string generate(const std::string &secret, std::string_view pubkeyHex, uint64_t lifetimeSeconds) {
        auto pubkeyBin = from_hex(pubkeyHex, false);
        if (pubkeyBin.size() != PUBKEY_SIZE) throw herr("invalid pubkey size for session token");

        uint64_t issuedAt = hoytech::curr_time_s();
        uint64_t expiresAt = issuedAt + lifetimeSeconds;

        uint8_t data[DATA_SIZE];
        memcpy(data, pubkeyBin.data(), PUBKEY_SIZE);
        memcpy(data + PUBKEY_SIZE, &issuedAt, 8);
        memcpy(data + PUBKEY_SIZE + 8, &expiresAt, 8);

        uint8_t token[TOKEN_SIZE];
        memcpy(token, data, DATA_SIZE);
        computeHmac(secret, data, DATA_SIZE, token + DATA_SIZE);

        return to_hex(std::string_view(reinterpret_cast<char*>(token), TOKEN_SIZE));
    }

    struct ValidatedToken {
        std::string pubkeyHex;
        uint64_t issuedAt;
        uint64_t expiresAt;
    };

    static std::optional<ValidatedToken> validate(const std::string &secret, const std::string &tokenHex) {
        std::string tokenBin;
        try {
            tokenBin = from_hex(tokenHex, false);
        } catch (...) {
            return std::nullopt;
        }

        if (tokenBin.size() != TOKEN_SIZE) return std::nullopt;

        auto *raw = reinterpret_cast<const uint8_t*>(tokenBin.data());

        // Check expiration first (cheap)
        uint64_t issuedAt, expiresAt;
        memcpy(&issuedAt, raw + PUBKEY_SIZE, 8);
        memcpy(&expiresAt, raw + PUBKEY_SIZE + 8, 8);

        uint64_t now = hoytech::curr_time_s();
        if (now > expiresAt) return std::nullopt;
        if (issuedAt > now + 60) return std::nullopt; // issued in the future (clock skew tolerance)

        // Verify HMAC (constant-time comparison)
        uint8_t expectedHmac[HMAC_SIZE];
        computeHmac(secret, raw, DATA_SIZE, expectedHmac);

        if (CRYPTO_memcmp(expectedHmac, raw + DATA_SIZE, HMAC_SIZE) != 0) {
            return std::nullopt;
        }

        ValidatedToken result;
        result.pubkeyHex = to_hex(std::string_view(reinterpret_cast<const char*>(raw), PUBKEY_SIZE));
        result.issuedAt = issuedAt;
        result.expiresAt = expiresAt;
        return result;
    }
};
