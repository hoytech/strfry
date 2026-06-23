#pragma once

#include "SearchProvider.h"

// No-op search provider (default when search is disabled)
// Accepts all operations but performs no indexing and returns empty results
class NoopSearchProvider : public ISearchProvider {
public:
    NoopSearchProvider() = default;
    ~NoopSearchProvider() override = default;

    bool healthy() const override {
        // Always report as healthy (no backend to fail)
        return true;
    }

    void indexEvent(uint64_t levId, std::string_view json, uint64_t kind, uint64_t created_at) override {
        // No-op: ignore indexing requests
        (void)levId;
        (void)json;
        (void)kind;
        (void)created_at;
    }

    void deleteEvent(uint64_t levId) override {
        // No-op: ignore deletion requests
        (void)levId;
    }

    std::vector<SearchHit> query(const SearchQuery& q, lmdb::txn &) override {
        // Return empty results
        (void)q;
        return {};
    }
};
