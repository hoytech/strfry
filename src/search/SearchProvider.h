#pragma once

#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include <cstdint>
#include <memory>
#include "golpe.h"


// Search query structure
struct SearchQuery {
    std::string q;                                      // Raw query string
    std::optional<std::vector<uint64_t>> kinds;         // Filter by kinds
    std::optional<std::vector<std::string>> authors;    // Filter by authors (hex pubkeys)
    std::optional<uint64_t> since;                      // Timestamp lower bound
    std::optional<uint64_t> until;                      // Timestamp upper bound
    uint64_t limit = 100;                               // Maximum results to return
};

// Search result hit with relevance score
struct SearchHit {
    uint64_t levId;     // Event's internal LMDB ID
    float score;        // Relevance score (higher = more relevant)
};

// Abstract search provider interface
class ISearchProvider {
public:
    virtual ~ISearchProvider() = default;

    // Health check - returns true if provider is operational
    virtual bool healthy() const = 0;

    // Index an event (called after successful write)
    // json: Full event JSON
    // kind: Event kind
    // created_at: Event timestamp
    virtual void indexEvent(uint64_t levId, std::string_view json, uint64_t kind, uint64_t created_at) = 0;

    // Remove an event from the index (called on delete/replace/expiration)
    virtual void deleteEvent(uint64_t levId) = 0;

    // Execute a search query using the provided read-only LMDB transaction
    // Fills hits sorted by score descending
    virtual std::vector<SearchHit> query(const SearchQuery& q, lmdb::txn &txn) = 0;
};


// Factory function to create search provider based on config
// Declared here, implemented in SearchProvider.cpp
std::unique_ptr<ISearchProvider> makeSearchProvider();
