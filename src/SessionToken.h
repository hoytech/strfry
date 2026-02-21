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
    static constexpr size_t CLIENT_ID_SIZE = 16;

    // Token binary layout v2: pubkey(32) + issuedAt(8) + expiresAt(8) + clientId(16) + hmac(32) = 96 bytes
    static constexpr size_t DATA_SIZE = PUBKEY_SIZE + 8 + 8 + CLIENT_ID_SIZE;
    static constexpr size_t TOKEN_SIZE = DATA_SIZE + HMAC_SIZE;

    // Legacy token layout v1: pubkey(32) + issuedAt(8) + expiresAt(8) + hmac(32) = 80 bytes
    static constexpr size_t LEGACY_DATA_SIZE = PUBKEY_SIZE + 8 + 8;
    static constexpr size_t LEGACY_TOKEN_SIZE = LEGACY_DATA_SIZE + HMAC_SIZE;

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

    // Generate a session token bound to a clientId.
    // If clientIdHex is empty, clientId is zeroed (backward compatible / unbound token).
    static std::string generate(const std::string &secret, std::string_view pubkeyHex, uint64_t lifetimeSeconds, const std::string &clientIdHex = "") {
        auto pubkeyBin = from_hex(pubkeyHex, false);
        if (pubkeyBin.size() != PUBKEY_SIZE) throw herr("invalid pubkey size for session token");

        uint8_t clientId[CLIENT_ID_SIZE] = {};
        if (clientIdHex.size()) {
            auto clientIdBin = from_hex(clientIdHex, false);
            if (clientIdBin.size() != CLIENT_ID_SIZE) throw herr("invalid clientId size: expected 16 bytes (32 hex chars)");
            memcpy(clientId, clientIdBin.data(), CLIENT_ID_SIZE);
        }

        uint64_t issuedAt = hoytech::curr_time_s();
        uint64_t expiresAt = issuedAt + lifetimeSeconds;

        uint8_t data[DATA_SIZE];
        memcpy(data, pubkeyBin.data(), PUBKEY_SIZE);
        memcpy(data + PUBKEY_SIZE, &issuedAt, 8);
        memcpy(data + PUBKEY_SIZE + 8, &expiresAt, 8);
        memcpy(data + PUBKEY_SIZE + 16, clientId, CLIENT_ID_SIZE);

        uint8_t token[TOKEN_SIZE];
        memcpy(token, data, DATA_SIZE);
        computeHmac(secret, data, DATA_SIZE, token + DATA_SIZE);

        return to_hex(std::string_view(reinterpret_cast<char*>(token), TOKEN_SIZE));
    }

    struct ValidatedToken {
        std::string pubkeyHex;
        uint64_t issuedAt;
        uint64_t expiresAt;
        std::string clientIdHex; // empty string if unbound (zeroed)
    };

    // Validate a session token. If clientIdHex is provided, it must match the clientId baked into the token.
    // Accepts both v2 (192 hex) and legacy v1 (160 hex) tokens.
    static std::optional<ValidatedToken> validate(const std::string &secret, const std::string &tokenHex, const std::string &clientIdHex = "") {
        std::string tokenBin;
        try {
            tokenBin = from_hex(tokenHex, false);
        } catch (...) {
            return std::nullopt;
        }

        // Accept v2 tokens (96 bytes) and legacy v1 tokens (80 bytes)
        bool isLegacy = (tokenBin.size() == LEGACY_TOKEN_SIZE);
        if (tokenBin.size() != TOKEN_SIZE && !isLegacy) return std::nullopt;

        auto *raw = reinterpret_cast<const uint8_t*>(tokenBin.data());

        // Check expiration first (cheap)
        uint64_t issuedAt, expiresAt;
        memcpy(&issuedAt, raw + PUBKEY_SIZE, 8);
        memcpy(&expiresAt, raw + PUBKEY_SIZE + 8, 8);

        uint64_t now = hoytech::curr_time_s();
        if (now > expiresAt) return std::nullopt;
        if (issuedAt > now + 60) return std::nullopt; // issued in the future (clock skew tolerance)

        if (isLegacy) {
            // Legacy v1: no clientId in token, validate with legacy data size
            uint8_t expectedHmac[HMAC_SIZE];
            computeHmac(secret, raw, LEGACY_DATA_SIZE, expectedHmac);
            if (CRYPTO_memcmp(expectedHmac, raw + LEGACY_DATA_SIZE, HMAC_SIZE) != 0) return std::nullopt;

            // If caller provided a clientId, legacy tokens can't match (they have no binding)
            if (clientIdHex.size()) return std::nullopt;

            ValidatedToken result;
            result.pubkeyHex = to_hex(std::string_view(reinterpret_cast<const char*>(raw), PUBKEY_SIZE));
            result.issuedAt = issuedAt;
            result.expiresAt = expiresAt;
            return result;
        }

        // V2: verify HMAC over full data including clientId
        uint8_t expectedHmac[HMAC_SIZE];
        computeHmac(secret, raw, DATA_SIZE, expectedHmac);
        if (CRYPTO_memcmp(expectedHmac, raw + DATA_SIZE, HMAC_SIZE) != 0) return std::nullopt;

        // Extract clientId from token
        const uint8_t *tokenClientId = raw + PUBKEY_SIZE + 16;

        // Check if the token's clientId is all zeros (unbound)
        static const uint8_t zeroes[CLIENT_ID_SIZE] = {};
        bool tokenIsUnbound = (memcmp(tokenClientId, zeroes, CLIENT_ID_SIZE) == 0);

        // If caller provided a clientId, it must match the token's clientId
        if (clientIdHex.size()) {
            std::string callerBin;
            try { callerBin = from_hex(clientIdHex, false); } catch (...) { return std::nullopt; }
            if (callerBin.size() != CLIENT_ID_SIZE) return std::nullopt;
            if (CRYPTO_memcmp(tokenClientId, callerBin.data(), CLIENT_ID_SIZE) != 0) return std::nullopt;
        } else if (!tokenIsUnbound) {
            // Token is client-bound but no clientId was provided — reject
            return std::nullopt;
        }

        ValidatedToken result;
        result.pubkeyHex = to_hex(std::string_view(reinterpret_cast<const char*>(raw), PUBKEY_SIZE));
        result.issuedAt = issuedAt;
        result.expiresAt = expiresAt;
        if (!tokenIsUnbound) {
            result.clientIdHex = to_hex(std::string_view(reinterpret_cast<const char*>(tokenClientId), CLIENT_ID_SIZE));
        }
        return result;
    }
};
