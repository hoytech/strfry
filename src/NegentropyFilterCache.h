#pragma once

#include <string>

#include <negentropy/storage/BTreeLMDB.h>

#include "golpe.h"

#include "filters.h"
#include "PackedEvent.h"


struct NegentropyFilterCache {
    struct FilterInfo {
        NostrFilter f;
        uint64_t treeId;
    };

    std::vector<FilterInfo> filters;
    uint64_t modificationCounter = 0;

    void ctx(lmdb::txn &txn, const std::function<void(const std::function<void(const PackedEventView &, bool)> &)> &cb) {
        freshenCache(txn);

        std::vector<std::unique_ptr<negentropy::storage::BTreeLMDB>> storages(filters.size());

        cb([&](const PackedEventView &ev, bool insert){
            for (size_t i = 0; i < filters.size(); i++) {
                const auto &filter = filters[i];

                if (!filter.f.doesMatch(ev)) continue;

                if (!storages[i]) storages[i] = std::make_unique<negentropy::storage::BTreeLMDB>(txn, negentropyDbi, filter.treeId);

                if (insert) storages[i]->insert(ev.created_at(), ev.id());
                else storages[i]->erase(ev.created_at(), ev.id());
            }
        });
    }

  private:
    void freshenCache(lmdb::txn &txn) {
        uint64_t curr = env.lookup_Meta(txn, 1)->negentropyModificationCounter();

        if (curr != modificationCounter) {
            filters.clear();

            env.foreach_NegentropyFilter(txn, [&](auto &f){
                filters.emplace_back(
                    NostrFilter(tao::json::from_string(f.filter()), MAX_U64),
                    f.primaryKeyId
                );
                return true;
            });

            modificationCounter = curr;
        }
    }
};
