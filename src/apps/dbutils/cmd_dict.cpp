#include <zstd.h>
#include <zdict.h>

#include <iostream>
#include <random>

#include <docopt.h>
#include "golpe.h"

#include "DBQuery.h"
#include "events.h"


static const char USAGE[] =
R"(
    Usage:
      dict stats [--filter=<filter>]
      dict train [--filter=<filter>] [--limit=<limit>] [--dictSize=<dictSize>]
      dict compress [--filter=<filter>] [--dictId=<dictId>] [--level=<level>]
      dict decompress [--filter=<filter>]
)";


void cmd_dict(const std::vector<std::string> &subArgs) {
    std::map<std::string, docopt::value> args = docopt::docopt(USAGE, subArgs, true, "");

    std::string filterStr;
    if (args["--filter"]) filterStr = args["--filter"].asString();
    else filterStr = "{}";

    uint64_t limit = MAX_U64;
    if (args["--limit"]) limit = args["--limit"].asLong();

    uint64_t dictSize = 100'000;
    if (args["--dictSize"]) dictSize = args["--dictSize"].asLong();

    uint64_t dictId = 0;
    if (args["--dictId"]) dictId = args["--dictId"].asLong();

    int level = 3;
    if (args["--level"]) level = args["--level"].asLong();


    Decompressor decomp;
    std::vector<uint64_t> levIds;


    auto txn = env.txn_ro();

    DBQuery query(tao::json::from_string(filterStr));

    while (1) {
        bool complete = query.process(txn, [&](const auto &sub, uint64_t levId, std::string_view){
            levIds.push_back(levId);
        });

        if (complete) break;
    }

    LI << "Filter matched " << levIds.size() << " records";


    if (args["stats"].asBool()) {
        uint64_t totalSize = 0;
        uint64_t totalCompressedSize = 0;
        uint64_t numCompressed = 0;

        btree_map<uint32_t, uint64_t> dicts;

        env.foreach_CompressionDictionary(txn, [&](auto &view){
            auto dictId = view.primaryKeyId;
            if (!dicts.contains(dictId)) dicts[dictId] = 0;
            return true;
        });

        for (auto levId : levIds) {
            std::string_view raw;

            bool found = env.dbi_EventPayload.get(txn, lmdb::to_sv<uint64_t>(levId), raw);
            if (!found) throw herr("couldn't find event in EventPayload");

            uint32_t dictId;
            size_t outCompressedSize;

            auto json = decodeEventPayload(txn, decomp, raw, &dictId, &outCompressedSize);

            totalSize += json.size();
            totalCompressedSize += dictId ? outCompressedSize : json.size();

            if (dictId) {
                numCompressed++;
                dicts[dictId]++;
            }
        }

        auto ratio = renderPercent(1.0 - (double)totalCompressedSize / totalSize);

        std::cout << "Num compressed: " << numCompressed << " / " << levIds.size() << "\n";
        std::cout << "Uncompressed size: " << renderSize(totalSize) << "\n";
        std::cout << "Compressed size:   " << renderSize(totalCompressedSize) << " (" << ratio << ")" << "\n";
        std::cout << "\ndictId : events\n";

        for (auto &[dictId, n] : dicts) {
            std::cout << "  " << dictId << " : " << n << "\n";
        }
    } else if (args["train"].asBool()) {
        std::string trainingBuf;
        std::vector<size_t> trainingSizes;

        if (levIds.size() > limit) {
            LI << "Randomly selecting " << limit << " records";
            std::random_device rd;
            std::mt19937 g(rd());
            std::shuffle(levIds.begin(), levIds.end(), g);
            levIds.resize(limit);
        }

        for (auto levId : levIds) {
            std::string json = std::string(getEventJson(txn, decomp, levId));
            trainingBuf += json;
            trainingSizes.emplace_back(json.size());
        }

        std::string dict(dictSize, '\0');

        LI << "Performing zstd training...";

        auto ret = ZDICT_trainFromBuffer(dict.data(), dict.size(), trainingBuf.data(), trainingSizes.data(), trainingSizes.size());
        if (ZDICT_isError(ret)) throw herr("zstd training failed: ", ZSTD_getErrorName(ret));

        txn.abort();
        txn = env.txn_rw();

        uint64_t newDictId = env.insert_CompressionDictionary(txn, dict);

        std::cout << "Saved new dictionary, dictId = " << newDictId << std::endl;

        txn.commit();
    } else if (args["compress"].asBool()) {
        if (dictId == 0) throw herr("specify --dictId or --decompress");

        txn.abort();
        txn = env.txn_rw();

        auto view = env.lookup_CompressionDictionary(txn, dictId);
        if (!view) throw herr("couldn't find dictId ", dictId);
        auto dict = view->dict();

        auto *cctx = ZSTD_createCCtx();
        auto *cdict = ZSTD_createCDict(dict.data(), dict.size(), level);

        uint64_t origSizes = 0;
        uint64_t compressedSizes = 0;
        uint64_t pendingFlush = 0;
        uint64_t processed = 0;

        std::string compressedData(500'000, '\0');

        for (auto levId : levIds) {
            std::string_view orig;

            try {
                orig = getEventJson(txn, decomp, levId);
            } catch (std::exception &e) {
                continue;
            }

            auto ret = ZSTD_compress_usingCDict(cctx, compressedData.data(), compressedData.size(), orig.data(), orig.size(), cdict);
            if (ZDICT_isError(ret)) throw herr("zstd compression failed: ", ZSTD_getErrorName(ret));

            origSizes += orig.size();
            compressedSizes += ret;

            std::string newVal;

            if (ret + 4 < orig.size()) {
                newVal += '\x01';
                newVal += lmdb::to_sv<uint32_t>(dictId);
                newVal += std::string_view(compressedData.data(), ret);
            } else {
                newVal += '\x00';
                newVal += orig;
            }

            env.dbi_EventPayload.put(txn, lmdb::to_sv<uint64_t>(levId), newVal);

            pendingFlush++;
            processed++;
            if (pendingFlush > 10'000) {
                txn.commit();

                LI << "Progress: " << processed << "/" << levIds.size();
                pendingFlush = 0;

                txn = env.txn_rw();
            }
        }

        txn.commit();

        LI << "Original event sizes: " << origSizes;
        LI << "New event sizes:      " << compressedSizes;
    } else if (args["decompress"].asBool()) {
        txn.abort();
        txn = env.txn_rw();

        uint64_t pendingFlush = 0;
        uint64_t processed = 0;

        for (auto levId : levIds) {
            std::string_view orig;

            try {
                orig = getEventJson(txn, decomp, levId);
            } catch (std::exception &e) {
                continue;
            }

            std::string newVal;

            newVal += '\x00';
            newVal += orig;

            env.dbi_EventPayload.put(txn, lmdb::to_sv<uint64_t>(levId), newVal);

            pendingFlush++;
            processed++;
            if (pendingFlush > 10'000) {
                txn.commit();

                LI << "Progress: " << processed << "/" << levIds.size();
                pendingFlush = 0;

                txn = env.txn_rw();
            }
        }

        txn.commit();
    }
}
