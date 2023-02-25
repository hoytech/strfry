#pragma once

#include <zstd.h>
#include <zdict.h>

#include <mutex>

#include "golpe.h"


struct DictionaryBroker {
    std::mutex mutex;
    flat_hash_map<uint32_t, ZSTD_DDict*> dicts;

    ZSTD_DDict *getDict(lmdb::txn &txn, uint32_t dictId) {
        std::lock_guard<std::mutex> guard(mutex);

        auto it = dicts.find(dictId);
        if (it != dicts.end()) return it->second;

        auto view = env.lookup_CompressionDictionary(txn, dictId);
        if (!view) throw herr("couldn't find dictId ", dictId);
        auto dictBuffer = view->dict();

        auto *dict = dicts[dictId] = ZSTD_createDDict(dictBuffer.data(), dictBuffer.size());

        return dict;
    }
};

extern DictionaryBroker globalDictionaryBroker;


struct Decompressor {
    ZSTD_DCtx *dctx;
    flat_hash_map<uint32_t, ZSTD_DDict*> dicts;
    std::string buffer;

    Decompressor() {
        dctx = ZSTD_createDCtx();
    }

    ~Decompressor() {
        ZSTD_freeDCtx(dctx);
    }

    void reserve(size_t n) {
        buffer.resize(n);
    }

    // Return result only valid until one of: a) next call to decompress()/reserve(), or Decompressor destroyed

    std::string_view decompress(lmdb::txn &txn, uint32_t dictId, std::string_view src) {
        auto it = dicts.find(dictId);
        ZSTD_DDict *dict;

        if (it == dicts.end()) {
            dict = dicts[dictId] = globalDictionaryBroker.getDict(txn, dictId);
        } else {
            dict = it->second;
        }

        auto ret = ZSTD_decompress_usingDDict(dctx, buffer.data(), buffer.size(), src.data(), src.size(), dict);
        if (ZDICT_isError(ret)) throw herr("zstd decompression failed: ", ZSTD_getErrorName(ret));

        return std::string_view(buffer.data(), ret);
    }
};
