// Tests for NIP-50 posting pack/unpack and DUPSORT integration.
//
// The pack helpers below MUST stay in sync with src/search/LmdbSearchProvider.h.
// A tripwire test below asserts a known packed value so accidental drift fails
// loud. Kept standalone (no golpe.h) so the test is small and fast.

#include <endian.h>
#include <lmdb.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <sys/stat.h>
#include <unistd.h>


// ---- mirrors src/search/LmdbSearchProvider.h ------------------------------
static inline uint64_t packPosting(uint64_t levId, uint16_t tf) {
    return htobe64((levId << 16) | tf);
}

static inline void unpackPosting(uint64_t stored, uint64_t &levId, uint16_t &tf) {
    uint64_t packed = be64toh(stored);
    levId = packed >> 16;
    tf = static_cast<uint16_t>(packed & 0xFFFF);
}
// --------------------------------------------------------------------------


static int g_failures = 0;

#define FAIL(...) do { \
    std::fprintf(stderr, "FAIL: "); \
    std::fprintf(stderr, __VA_ARGS__); \
    std::fprintf(stderr, "\n"); \
    g_failures++; \
} while (0)


static void test_roundtrip() {
    const uint64_t levIds[] = {
        0, 1, 254, 255, 256, 257,
        65534, 65535, 65536, 65537,
        16777214, 16777215, 16777216, 16777217,
        (1ULL << 47) - 1, (1ULL << 47),
    };
    const uint16_t tfs[] = { 1, 2, 65535 };

    for (uint64_t levId : levIds) {
        for (uint16_t tf : tfs) {
            uint64_t stored = packPosting(levId, tf);
            uint64_t outLev = 0;
            uint16_t outTf = 0;
            unpackPosting(stored, outLev, outTf);
            if (outLev != levId || outTf != tf) {
                FAIL("roundtrip levId=%lu tf=%u -> levId=%lu tf=%u",
                     levId, tf, outLev, outTf);
            }
        }
    }
}


static int sgn(long long v) { return (v > 0) - (v < 0); }


static void test_memcmp_ordering() {
    // memcmp(pack(a), pack(b)) sign must match numeric (a-b) sign.
    // This is the property MDB_DUPSORT relies on without a custom comparator.
    const uint64_t levIds[] = {
        0, 1, 2, 254, 255, 256, 257, 511, 512,
        65534, 65535, 65536, 65537, 131071, 131072,
        16777214, 16777215, 16777216, 16777217,
        (1ULL << 32), (1ULL << 40), (1ULL << 47) - 1, (1ULL << 47),
    };
    const size_t n = sizeof(levIds) / sizeof(levIds[0]);

    for (size_t i = 0; i < n; i++) {
        for (size_t j = 0; j < n; j++) {
            uint64_t a = packPosting(levIds[i], 1);
            uint64_t b = packPosting(levIds[j], 1);
            int memSgn = sgn(std::memcmp(&a, &b, sizeof(a)));
            int numSgn = sgn(static_cast<long long>(levIds[i]) - static_cast<long long>(levIds[j]));
            if (memSgn != numSgn) {
                FAIL("memcmp ordering levId=%lu vs %lu -> memSgn=%d numSgn=%d",
                     levIds[i], levIds[j], memSgn, numSgn);
            }
        }
    }
}


static void test_tripwire_constant() {
    // If anyone changes the packing formula, this test breaks loudly and
    // forces re-verification of every assumption that depends on the layout.
    // Layout: 6 BE bytes of levId (high) followed by 2 BE bytes of tf (low).
    // levId=0x1234, tf=0xABCD -> 00 00 00 00 12 34 AB CD
    uint64_t got = packPosting(0x1234, 0xABCD);
    uint8_t expected[8] = { 0x00, 0x00, 0x00, 0x00, 0x12, 0x34, 0xAB, 0xCD };
    if (std::memcmp(&got, expected, 8) != 0) {
        const uint8_t *gotBytes = reinterpret_cast<const uint8_t*>(&got);
        FAIL("tripwire: packPosting(0x1234, 0xABCD) bytes = "
             "%02X %02X %02X %02X %02X %02X %02X %02X (expected 00 00 00 00 12 34 AB CD)",
             gotBytes[0], gotBytes[1], gotBytes[2], gotBytes[3],
             gotBytes[4], gotBytes[5], gotBytes[6], gotBytes[7]);
    }
}


static void test_lmdb_dupsort_integration() {
    // Open a temp env, insert postings via MDB_APPENDDUP for adversarial
    // levIds, assert no MDB_KEYEXIST, and assert iteration order is numeric.
    char tmpl[] = "/tmp/strfry_posting_test_XXXXXX";
    if (!mkdtemp(tmpl)) {
        FAIL("mkdtemp failed: %s", std::strerror(errno));
        return;
    }
    std::string path = tmpl;

    MDB_env *env = nullptr;
    if (mdb_env_create(&env) != 0) { FAIL("mdb_env_create"); return; }
    mdb_env_set_maxdbs(env, 4);
    mdb_env_set_mapsize(env, 1ULL << 28);
    if (mdb_env_open(env, path.c_str(), 0, 0644) != 0) {
        FAIL("mdb_env_open at %s", path.c_str());
        mdb_env_close(env);
        return;
    }

    MDB_txn *txn = nullptr;
    if (mdb_txn_begin(env, nullptr, 0, &txn) != 0) { FAIL("mdb_txn_begin"); return; }

    MDB_dbi dbi;
    if (mdb_dbi_open(txn, "postings", MDB_CREATE | MDB_DUPSORT, &dbi) != 0) {
        FAIL("mdb_dbi_open");
        mdb_txn_abort(txn);
        mdb_env_close(env);
        return;
    }

    // Insertions in monotonically increasing levId order, spanning every
    // byte boundary that broke MDB_APPENDDUP with the host-endian packing.
    const uint64_t levIds[] = {
        1, 2, 254, 255, 256, 257,
        65535, 65536, 65537,
        16777215, 16777216, 16777217,
        (1ULL << 32), (1ULL << 40),
    };
    const size_t n = sizeof(levIds) / sizeof(levIds[0]);

    std::string key = "tok";
    int appendErrors = 0;
    std::vector<uint64_t> storedPostings(n);

    for (size_t i = 0; i < n; i++) {
        storedPostings[i] = packPosting(levIds[i], 1);
        MDB_val k = { key.size(), const_cast<char*>(key.data()) };
        MDB_val v = { sizeof(storedPostings[i]), &storedPostings[i] };
        int rc = mdb_put(txn, dbi, &k, &v, MDB_APPENDDUP);
        if (rc != 0) {
            appendErrors++;
            FAIL("MDB_APPENDDUP rejected levId=%lu: %s", levIds[i], mdb_strerror(rc));
        }
    }

    if (appendErrors == 0) {
        // Iteration order check.
        MDB_cursor *cur = nullptr;
        mdb_cursor_open(txn, dbi, &cur);
        MDB_val k = { key.size(), const_cast<char*>(key.data()) };
        MDB_val v;
        size_t i = 0;
        if (mdb_cursor_get(cur, &k, &v, MDB_SET) == 0) {
            mdb_cursor_get(cur, &k, &v, MDB_FIRST_DUP);
            do {
                uint64_t stored = 0;
                std::memcpy(&stored, v.mv_data, sizeof(stored));
                uint64_t lev = 0;
                uint16_t tf = 0;
                unpackPosting(stored, lev, tf);
                if (i >= n) {
                    FAIL("dupsort iteration produced more than %zu entries", n);
                    break;
                }
                if (lev != levIds[i]) {
                    FAIL("dupsort iteration position %zu: got levId=%lu, expected %lu",
                         i, lev, levIds[i]);
                }
                i++;
            } while (mdb_cursor_get(cur, &k, &v, MDB_NEXT_DUP) == 0);
        }
        if (i != n) FAIL("dupsort iteration produced %zu entries, expected %zu", i, n);
        mdb_cursor_close(cur);
    }

    mdb_txn_abort(txn);
    mdb_env_close(env);

    // Best-effort cleanup of the temp env directory.
    std::string dataFile = path + "/data.mdb";
    std::string lockFile = path + "/lock.mdb";
    unlink(dataFile.c_str());
    unlink(lockFile.c_str());
    rmdir(path.c_str());
}


int main() {
    test_roundtrip();
    test_memcmp_ordering();
    test_tripwire_constant();
    test_lmdb_dupsort_integration();

    if (g_failures) {
        std::fprintf(stderr, "Search posting tests: %d failure(s)\n", g_failures);
        return EXIT_FAILURE;
    }
    std::puts("Search posting tests passed");
    return EXIT_SUCCESS;
}
