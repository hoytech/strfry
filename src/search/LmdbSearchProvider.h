#pragma once

#include "SearchProvider.h"
#include "Tokenizer.h"
#include "KindMatcher.h"
#include "Decompressor.h"
#include "events.h"
#include "golpe.h"
#include <tao/json.hpp>
#include <algorithm>
#include <cmath>
#include <thread>
#include <chrono>
#include <functional>


// Pack posting: [levId:48][tf:16] as host-endian uint64
inline uint64_t packPosting(uint64_t levId, uint16_t tf) {
    return (levId << 16) | tf;
}

inline void unpackPosting(uint64_t packed, uint64_t &levId, uint16_t &tf) {
    levId = packed >> 16;
    tf = static_cast<uint16_t>(packed & 0xFFFF);
}

// Pack doc metadata: [docLen:16][kind:16][reserved:32] as host-endian uint64
inline uint64_t packDocMeta(uint16_t docLen, uint16_t kind) {
    return (static_cast<uint64_t>(docLen) << 48) | (static_cast<uint64_t>(kind) << 32);
}

inline void unpackDocMeta(uint64_t packed, uint16_t &docLen, uint16_t &kind) {
    docLen = static_cast<uint16_t>(packed >> 48);
    kind = static_cast<uint16_t>((packed >> 32) & 0xFFFF);
}


// LMDB-backed search provider with BM25 scoring
class LmdbSearchProvider : public ISearchProvider {
private:
    static constexpr float k1 = 1.2f;
    static constexpr float b = 0.75f;

    mutable KindMatcher kindMatcher;
    mutable bool kindMatcherInitialized = false;

    const KindMatcher& getKindMatcher() const {
        if (!kindMatcherInitialized) {
            kindMatcher = KindMatcher::parse(cfg().relay__search__indexedKinds);
            kindMatcherInitialized = true;

            if (kindMatcher.hasError()) {
                LE << "Search indexedKinds config error: " << kindMatcher.getError();
                LE << "Search indexing disabled due to invalid configuration";
            } else {
                LI << "Search indexedKinds: " << kindMatcher.toString();
            }
        }
        return kindMatcher;
    }

    bool shouldIndexKind(uint64_t kind) const {
        return getKindMatcher().matches(kind);
    }


    uint64_t getTotalDocs(lmdb::txn &txn) const {
        uint64_t count = 0;
        auto cursor = lmdb::cursor::open(txn, env.dbi_SearchDocMeta);

        std::string_view key, val;
        if (cursor.get(key, val, MDB_FIRST)) {
            do {
                count++;
            } while (cursor.get(key, val, MDB_NEXT));
        }

        return count;
    }

    uint64_t getDocFreq(lmdb::txn &txn, const std::string &token) const {
        auto cursor = lmdb::cursor::open(txn, env.dbi_SearchIndex);

        std::string_view key(token.data(), token.size());
        std::string_view val;

        uint64_t count = 0;
        if (cursor.get(key, val, MDB_SET)) {
            do {
                count++;
            } while (cursor.get(key, val, MDB_NEXT_DUP));
        }

        return count;
    }

    float getAvgDocLen(lmdb::txn &txn) const {
        uint64_t totalLen = 0;
        uint64_t count = 0;

        auto cursor = lmdb::cursor::open(txn, env.dbi_SearchDocMeta);

        std::string_view key, val;
        if (cursor.get(key, val, MDB_FIRST)) {
            do {
                uint64_t packed = *reinterpret_cast<const uint64_t*>(val.data());
                uint16_t docLen, kind;
                unpackDocMeta(packed, docLen, kind);
                totalLen += docLen;
                count++;
            } while (cursor.get(key, val, MDB_NEXT));
        }

        return count > 0 ? static_cast<float>(totalLen) / count : 0.0f;
    }

    float computeIDF(uint64_t N, uint64_t df) const {
        if (df == 0) return 0.0f;
        return std::log((N - df + 0.5f) / (df + 0.5f) + 1.0f);
    }

    float computeBM25(uint64_t docLen, float avgDocLen, const std::vector<std::pair<std::string, uint16_t>> &termFreqs,
                      const std::vector<float> &idfs) const {
        float score = 0.0f;

        for (size_t i = 0; i < termFreqs.size(); i++) {
            uint16_t tf = termFreqs[i].second;
            float idf = idfs[i];

            float numerator = tf * (k1 + 1.0f);
            float denominator = tf + k1 * (1.0f - b + b * docLen / avgDocLen);

            score += idf * (numerator / denominator);
        }

        return score;
    }

    void persistSearchState(lmdb::txn &txn, uint64_t lastIndexedLevId, uint64_t indexVersion) const {
        auto stateView = env.lookup_SearchState(txn, 1);
        if (!stateView) {
            env.insert_SearchState(txn, lastIndexedLevId, indexVersion);
            return;
        }

        defaultDb::environment::Updates_SearchState upd;
        upd.lastIndexedLevId = lastIndexedLevId;
        if (stateView->indexVersion() != indexVersion) {
            upd.indexVersion = indexVersion;
        }
        env.update_SearchState(txn, *stateView, upd);
    }

public:
    static constexpr uint64_t kIndexVersion = 1;

    bool healthy() const override {
        if (!cfg().relay__search__enabled) return false;
        if (cfg().relay__search__backend != "lmdb") return false;

        // Tolerant readiness:
        // - Treat empty DBs as healthy
        // - If doc meta exists and head is small, consider healthy
        // - If SearchState exists, ensure it's close to head (within 1000 events),
        //   using saturating arithmetic to avoid underflow
        try {
            auto txn = env.txn_ro();

            uint64_t head = env.get_largest_integer_key_or_zero(txn, env.dbi_EventPayload);
            if (head == 0) return true; // empty DB

            uint64_t docCount = getTotalDocs(txn);
            if (docCount > 0 && head <= 1000) return true;

            auto stateView = env.lookup_SearchState(txn, 1);
            if (!stateView) {
                // No explicit state, but index exists
                return docCount > 0;
            }

            uint64_t last = stateView->lastIndexedLevId();
            if (last > head) last = head; // saturate
            return (head - last) < 1000;
        } catch (...) {
            return false; // conservative on errors
        }
    }

    bool indexEventWithTxnHook(uint64_t levId, std::string_view json, uint64_t kind, uint64_t created_at,
                               const std::function<void(lmdb::txn &)> &txnHook = {}) {
        if (!shouldIndexKind(kind)) return false;

        auto eventJson = tao::json::from_string(json);

        std::string text = Tokenizer::extractText(eventJson);
        if (text.empty()) return false;

        auto tokens = Tokenizer::tokenize(text);
        if (tokens.empty()) return false;

        uint16_t docLen = 0;
        for (const auto &token : tokens) {
            docLen += token.tf;
        }
        if (docLen > 65535) docLen = 65535; // Clamp to uint16 max

        auto txn = lmdb::txn::begin(env.lmdb_env);

        uint64_t docMetaPacked = packDocMeta(docLen, static_cast<uint16_t>(kind));
        env.dbi_SearchDocMeta.put(txn, lmdb::to_sv<uint64_t>(levId),
                                  std::string_view(reinterpret_cast<const char*>(&docMetaPacked), sizeof(docMetaPacked)));

        uint64_t maxPostings = cfg().relay__search__maxPostingsPerToken;
        auto cursor = lmdb::cursor::open(txn, env.dbi_SearchIndex);

        for (const auto &token : tokens) {
            uint64_t posting = packPosting(levId, token.tf);
            std::string_view postingView(reinterpret_cast<const char*>(&posting), sizeof(posting));

            env.dbi_SearchIndex.put(txn, token.text, postingView, MDB_APPENDDUP);

            uint64_t count = 0;
            std::string_view key = token.text;
            std::string_view val;

            if (cursor.get(key, val, MDB_SET)) {
                do {
                    count++;
                } while (cursor.get(key, val, MDB_NEXT_DUP));
            }

            // If over limit, delete (count - maxPostings) oldest entries
            if (count > maxPostings) {
                uint64_t toDelete = count - maxPostings;

                // Re-position cursor to first duplicate and delete oldest
                key = token.text; // Reset key for MDB_SET
                if (cursor.get(key, val, MDB_SET)) {
                    for (uint64_t i = 0; i < toDelete; i++) {
                        cursor.del(); // Delete current (oldest by levId due to APPENDDUP ordering)
                        if (i + 1 < toDelete && !cursor.get(key, val, MDB_NEXT_DUP)) break;
                    }
                }
            }
        }

        if (txnHook) txnHook(txn);

        txn.commit();
        return true;
    }

    void indexEvent(uint64_t levId, std::string_view json, uint64_t kind, uint64_t created_at) override {
        indexEventWithTxnHook(levId, json, kind, created_at, nullptr);
    }

    void deleteEvent(uint64_t levId) override {
        try {
            auto txn = lmdb::txn::begin(env.lmdb_env);

            try {
                env.dbi_SearchDocMeta.del(txn, lmdb::to_sv<uint64_t>(levId));
            } catch (...) {
                // Entry may not exist, that's ok
            }
            {
                auto cursor = lmdb::cursor::open(txn, env.dbi_SearchIndex);

                std::string_view key, val;
                if (cursor.get(key, val, MDB_FIRST)) {
                    do {
                        // Check all duplicates for this key
                        std::string_view dupKey = key;
                        std::string_view dupVal;

                        if (cursor.get(dupKey, dupVal, MDB_FIRST_DUP)) {
                            do {
                                uint64_t posting = *reinterpret_cast<const uint64_t*>(dupVal.data());
                                uint64_t postingLevId;
                                uint16_t tf;
                                unpackPosting(posting, postingLevId, tf);

                                if (postingLevId == levId) {
                                    cursor.del();
                                }
                            } while (cursor.get(dupKey, dupVal, MDB_NEXT_DUP));
                        }
                    } while (cursor.get(key, val, MDB_NEXT_NODUP));
                }
            }

            txn.commit();

        } catch (std::exception &e) {
            LE << "Failed to delete event from search index levId=" << levId << ": " << e.what();
        }
    }

    std::vector<SearchHit> query(const SearchQuery& q, lmdb::txn &txn) override {
        std::vector<SearchHit> results;

        try {
            // Parse query into tokens
            auto queryTokens = Tokenizer::parseQuery(q.q);
            if (queryTokens.empty()) return results;

            // Enforce max query terms limit
            if (queryTokens.size() > cfg().relay__search__maxQueryTerms) {
                queryTokens.resize(cfg().relay__search__maxQueryTerms);
            }

            // Get corpus statistics
            uint64_t N = getTotalDocs(txn);
            if (N == 0) return results;

            float avgDocLen = getAvgDocLen(txn);

            // Compute IDF for each query term
            std::vector<float> idfs;
            std::vector<uint64_t> dfs;
            for (const auto &token : queryTokens) {
                uint64_t df = getDocFreq(txn, token);
                dfs.push_back(df);
                idfs.push_back(computeIDF(N, df));
            }

            // Collect candidate documents
            // Strategy: Fetch postings for each query term, union all docs, compute scores
            flat_hash_map<uint64_t, std::vector<std::pair<std::string, uint16_t>>> docTerms; // levId -> [(token, tf)]

            for (size_t i = 0; i < queryTokens.size(); i++) {
                const auto &token = queryTokens[i];

                auto cursor = lmdb::cursor::open(txn, env.dbi_SearchIndex);
                std::string_view key(token.data(), token.size());
                std::string_view val;

                // Use MDB_SET to find the key, then MDB_FIRST_DUP to ensure we start from the first duplicate
                if (cursor.get(key, val, MDB_SET)) {
                    // Position to first duplicate for this key
                    if (cursor.get(key, val, MDB_FIRST_DUP)) {
                        uint64_t fetchedPostings = 0;
                        uint64_t maxFetch = cfg().relay__search__maxPostingsPerToken;

                        do {
                            if (fetchedPostings >= maxFetch) break;

                            uint64_t posting = *reinterpret_cast<const uint64_t*>(val.data());
                            uint64_t levId;
                            uint16_t tf;
                            unpackPosting(posting, levId, tf);

                            docTerms[levId].push_back({token, tf});
                            fetchedPostings++;
                        } while (cursor.get(key, val, MDB_NEXT_DUP));
                    }
                }
            }

            // Limit candidate documents
            if (docTerms.size() > cfg().relay__search__maxCandidateDocs) {
                // Two modes: order-based or weighted ranking
                std::string mode = cfg().relay__search__candidateRankMode;
                if (mode == "weighted") {
                    // Weighted ranking: score = w_terms * norm(terms) + w_tf * norm(tfSum) + w_recency * norm(recency)
                    // Normalization:
                    //  - terms: matchedTerms / queryTokens.size()
                    //  - tfSum: tfSum / maxTfSum over candidates
                    //  - recency: levId / head
                    uint64_t head = env.get_largest_integer_key_or_zero(txn, env.dbi_EventPayload);
                    uint32_t maxTfSum = 0;
                    for (const auto &kv : docTerms) {
                        uint32_t s = 0; for (const auto &p : kv.second) s += p.second; if (s > maxTfSum) maxTfSum = s;
                    }
                    float denomTerms = queryTokens.size() > 0 ? static_cast<float>(queryTokens.size()) : 1.0f;
                    float denomTf = maxTfSum > 0 ? static_cast<float>(maxTfSum) : 1.0f;
                    float denomRec = head > 0 ? static_cast<float>(head) : 1.0f;

                    uint64_t wTerms = cfg().relay__search__rankWeightTerms;
                    uint64_t wTf = cfg().relay__search__rankWeightTf;
                    uint64_t wRec = cfg().relay__search__rankWeightRecency;

                    struct Scored { uint64_t levId; float score; };
                    std::vector<Scored> scored;
                    scored.reserve(docTerms.size());
                    for (const auto &[levId, terms] : docTerms) {
                        uint32_t tfSum = 0; for (const auto &p : terms) tfSum += p.second;
                        float tNorm = static_cast<float>(terms.size()) / denomTerms;
                        float tfNorm = static_cast<float>(tfSum) / denomTf;
                        float rNorm = static_cast<float>(levId) / denomRec;
                        float s = wTerms * tNorm + wTf * tfNorm + wRec * rNorm;
                        scored.push_back(Scored{ levId, s });
                    }
                    std::sort(scored.begin(), scored.end(), [](const Scored &a, const Scored &b){ return a.score > b.score; });

                    flat_hash_map<uint64_t, std::vector<std::pair<std::string, uint16_t>>> limited;
                    uint64_t maxKeep = cfg().relay__search__maxCandidateDocs;
                    size_t kept = 0;
                    for (const auto &s : scored) {
                        if (kept++ >= maxKeep) break;
                        auto it = docTerms.find(s.levId);
                        if (it != docTerms.end()) limited.emplace(s.levId, std::move(it->second));
                    }
                    docTerms = std::move(limited);
                } else {
                    // Order-based ranking by configured key order
                    struct CandInfo { uint64_t levId; uint32_t matchedTerms; uint32_t tfSum; };
                    std::vector<CandInfo> ranked; ranked.reserve(docTerms.size());
                    for (const auto &[levId, terms] : docTerms) {
                        uint32_t tfSum = 0; for (const auto &kv : terms) tfSum += kv.second;
                        ranked.push_back(CandInfo{ levId, static_cast<uint32_t>(terms.size()), tfSum });
                    }

                    auto strategy = cfg().relay__search__candidateRanking;
                    auto cmp_terms_tf_recency = [](const CandInfo &a, const CandInfo &b){
                        if (a.matchedTerms != b.matchedTerms) return a.matchedTerms > b.matchedTerms;
                        if (a.tfSum != b.tfSum) return a.tfSum > b.tfSum;
                        return a.levId > b.levId;
                    };
                    auto cmp_terms_recency_tf = [](const CandInfo &a, const CandInfo &b){
                        if (a.matchedTerms != b.matchedTerms) return a.matchedTerms > b.matchedTerms;
                        if (a.levId != b.levId) return a.levId > b.levId;
                        return a.tfSum > b.tfSum;
                    };
                    auto cmp_tf_terms_recency = [](const CandInfo &a, const CandInfo &b){
                        if (a.tfSum != b.tfSum) return a.tfSum > b.tfSum;
                        if (a.matchedTerms != b.matchedTerms) return a.matchedTerms > b.matchedTerms;
                        return a.levId > b.levId;
                    };
                    auto cmp_tf_recency_terms = [](const CandInfo &a, const CandInfo &b){
                        if (a.tfSum != b.tfSum) return a.tfSum > b.tfSum;
                        if (a.levId != b.levId) return a.levId > b.levId;
                        return a.matchedTerms > b.matchedTerms;
                    };
                    auto cmp_recency_terms_tf = [](const CandInfo &a, const CandInfo &b){
                        if (a.levId != b.levId) return a.levId > b.levId;
                        if (a.matchedTerms != b.matchedTerms) return a.matchedTerms > b.matchedTerms;
                        return a.tfSum > b.tfSum;
                    };
                    auto cmp_recency_tf_terms = [](const CandInfo &a, const CandInfo &b){
                        if (a.levId != b.levId) return a.levId > b.levId;
                        if (a.tfSum != b.tfSum) return a.tfSum > b.tfSum;
                        return a.matchedTerms > b.matchedTerms;
                    };

                    if (strategy == "terms-recency-tf") std::sort(ranked.begin(), ranked.end(), cmp_terms_recency_tf);
                    else if (strategy == "tf-terms-recency") std::sort(ranked.begin(), ranked.end(), cmp_tf_terms_recency);
                    else if (strategy == "tf-recency-terms") std::sort(ranked.begin(), ranked.end(), cmp_tf_recency_terms);
                    else if (strategy == "recency-terms-tf") std::sort(ranked.begin(), ranked.end(), cmp_recency_terms_tf);
                    else if (strategy == "recency-tf-terms") std::sort(ranked.begin(), ranked.end(), cmp_recency_tf_terms);
                    else std::sort(ranked.begin(), ranked.end(), cmp_terms_tf_recency);

                    flat_hash_map<uint64_t, std::vector<std::pair<std::string, uint16_t>>> limited;
                    uint64_t maxKeep = cfg().relay__search__maxCandidateDocs;
                    size_t kept = 0;
                    for (const auto &ci : ranked) {
                        if (kept++ >= maxKeep) break;
                        auto it = docTerms.find(ci.levId);
                        if (it != docTerms.end()) limited.emplace(ci.levId, std::move(it->second));
                    }
                    docTerms = std::move(limited);
                }
            }

            // Fetch doc metadata and compute BM25 scores
            for (const auto &[levId, termFreqs] : docTerms) {
                std::string_view val;

                if (!env.dbi_SearchDocMeta.get(txn, lmdb::to_sv<uint64_t>(levId), val)) continue; // Doc deleted

                uint64_t packed = *reinterpret_cast<const uint64_t*>(val.data());
                uint16_t docLen, kind;
                unpackDocMeta(packed, docLen, kind);

                // Apply kind filter if specified
                if (q.kinds.has_value()) {
                    bool kindMatches = false;
                    for (uint64_t k : *q.kinds) {
                        if (k == kind) {
                            kindMatches = true;
                            break;
                        }
                    }
                    if (!kindMatches) continue;
                }

                // Compute BM25 score with optional recency tie-breaker
                // levId is monotonically increasing (newer events have higher levId)
                // If recencyBoostPercent (> 0), add a small recency boost normalized by percent/100
                float bm25Score = computeBM25(docLen, avgDocLen, termFreqs, idfs);
                float score = bm25Score;

                uint64_t recencyBoostPercent = cfg().relay__search__recencyBoostPercent;
                // Clamp to [0, 100] to avoid unexpected scaling
                if (recencyBoostPercent > 100) recencyBoostPercent = 100;
                if (recencyBoostPercent > 0 && N > 0) {
                    float factor = static_cast<float>(recencyBoostPercent) / 100.0f; // 1 = 1%
                    float recencyBoost = (static_cast<float>(levId) / static_cast<float>(N)) * factor;
                    score += recencyBoost;
                }

                results.push_back({levId, score});
            }

            // Sort by score descending (BM25 + optional recency tie-breaker if configured)
            std::sort(results.begin(), results.end(), [](const SearchHit &a, const SearchHit &b) {
                return a.score > b.score;
            });

            // Over-fetch to compensate for SearchRunner's post-filtering
            // SearchRunner will apply full filter matching (tags, etc.) which may drop results
            // Fetch (limit × overfetchFactor) candidates, bounded by maxCandidateDocs
            uint64_t overfetchFactor = cfg().relay__search__overfetchFactor;
            uint64_t overFetchLimit = std::min(q.limit * overfetchFactor, uint64_t(cfg().relay__search__maxCandidateDocs));
            if (results.size() > overFetchLimit) {
                results.resize(overFetchLimit);
            }

        } catch (std::exception &e) {
            LE << "Search query failed: " << e.what();
            return {};
        }

        return results;
    }

    // Background catch-up indexer - indexes events that were written before search was enabled
    // or while indexing was offline. Should be run in a background thread.
    void runCatchupIndexer(std::atomic<bool> &running) {
        try {
            LI << "Search catch-up indexer started";

            Decompressor decomp; // For decompressing event payloads

            while (running) {
                auto txn = lmdb::txn::begin(env.lmdb_env, nullptr, MDB_RDONLY);

                // Get last indexed levId from SearchState
                uint64_t lastIndexedLevId = 0;
                auto stateView = env.lookup_SearchState(txn, 1); // ID 1 is the single state record
                if (stateView) {
                    lastIndexedLevId = stateView->lastIndexedLevId();
                }

                // Get current head (most recent levId)
                uint64_t mostRecentLevId = env.get_largest_integer_key_or_zero(txn, env.dbi_EventPayload);

                if (lastIndexedLevId >= mostRecentLevId) {
                    // Index is caught up
                    txn.commit();
                    std::this_thread::sleep_for(std::chrono::seconds(10));
                    continue;
                }

                uint64_t startLevId = lastIndexedLevId + 1;
                uint64_t endLevId = std::min(lastIndexedLevId + 1000, mostRecentLevId); // Batch size: 1000 events

                LI << "Search indexer catching up: " << startLevId << " to " << endLevId << " (head: " << mostRecentLevId << ")";

                txn.commit();

                // Index batch
                uint64_t indexed = 0;
                for (uint64_t levId = startLevId; levId <= endLevId && running; levId++) {
                    try {
                        // Read and decode event from EventPayload
                        auto rtxn = lmdb::txn::begin(env.lmdb_env, nullptr, MDB_RDONLY);
                        std::string_view eventPayload;
                        if (!env.dbi_EventPayload.get(rtxn, lmdb::to_sv<uint64_t>(levId), eventPayload)) {
                            continue; // Event doesn't exist
                        }

                        // Decode event payload (handles compression)
                        std::string_view json = decodeEventPayload(rtxn, decomp, eventPayload, nullptr, nullptr);
                        rtxn.commit();

                        // Parse event to get kind and created_at
                        auto eventJson = tao::json::from_string(json);
                        uint64_t kind = eventJson.at("kind").get_unsigned();
                        uint64_t created_at = eventJson.at("created_at").get_unsigned();

                        // Index the event and persist progress inside the same transaction when data was written
                        bool wrote = indexEventWithTxnHook(levId, json, kind, created_at, [this, levId](lmdb::txn &txn) {
                            persistSearchState(txn, levId, kIndexVersion);
                        });

                        if (wrote) {
                            indexed++;
                        } else {
                            auto wtxn = lmdb::txn::begin(env.lmdb_env);
                            persistSearchState(wtxn, levId, kIndexVersion);
                            wtxn.commit();
                        }
                    } catch (std::exception &e) {
                        LE << "Failed to index event during catch-up levId=" << levId << ": " << e.what();

                        auto wtxn = lmdb::txn::begin(env.lmdb_env);
                        persistSearchState(wtxn, levId, kIndexVersion);
                        wtxn.commit();
                    }
                }

                if (indexed > 0) {
                    LI << "Search indexer: indexed " << indexed << " events, now at levId " << endLevId;
                }

                // Small sleep to avoid busy loop
                if (endLevId >= mostRecentLevId) {
                    std::this_thread::sleep_for(std::chrono::seconds(10));
                } else {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
            }

            LI << "Search catch-up indexer stopped";

        } catch (std::exception &e) {
            LE << "Search catch-up indexer failed: " << e.what();
        }
    }
};
