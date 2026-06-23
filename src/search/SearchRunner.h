#pragma once

#include "SearchProvider.h"
#include "filters.h"
#include "golpe.h"
#include "Tokenizer.h"
#include "Decompressor.h"
#include "events.h"
#include <tao/json.hpp>
#include <deque>


struct SearchRunner : NonCopyable {
    const NostrFilter &f;
    ISearchProvider *provider;
    std::deque<SearchHit> hits;
    size_t nextHitIndex = 0;
    uint64_t approxWork = 0;
    bool initialized = false;
    Decompressor decomp;

    SearchRunner(const NostrFilter &f, ISearchProvider *provider)
        : f(f), provider(provider) {}

    void initialize(lmdb::txn &txn) {
        if (initialized) return;
        initialized = true;

        if (!f.hasSearch()) return;
        if (!provider) return;

        SearchQuery q;
        q.q = *f.search;
        q.limit = std::min(f.limit, uint64_t(cfg().relay__maxFilterLimit));

        if (f.kinds) {
            q.kinds.emplace();
            for (size_t i = 0; i < f.kinds->size(); i++) {
                q.kinds->push_back(f.kinds->at(i));
            }
        }

        if (f.authors) {
            q.authors.emplace();
            for (size_t i = 0; i < f.authors->size(); i++) {
                q.authors->push_back(to_hex(f.authors->at(i)));
            }
        }

        if (f.since > 0) q.since = f.since;
        if (f.until < MAX_U64) q.until = f.until;

        auto searchResults = provider->query(q, txn);
        hits = std::deque<SearchHit>(searchResults.begin(), searchResults.end());
        approxWork = hits.size();
    }

    bool scan(lmdb::txn &txn, const std::function<bool(uint64_t)> &handleEvent, const std::function<bool(uint64_t)> &doPause) {
        if (!initialized) initialize(txn);

        while (nextHitIndex < hits.size()) {
            approxWork++;
            if (doPause(approxWork)) return false;

            const auto &hit = hits[nextHitIndex++];
            uint64_t levId = hit.levId;

            approxWork += 10;
            auto view = env.lookup_Event(txn, levId);
            if (!view) continue;

            if (f.hasSearch()) {
                try {
                    std::string_view eventPayload;
                    if (!env.dbi_EventPayload.get(txn, lmdb::to_sv<uint64_t>(levId), eventPayload)) continue;
                    std::string_view eventJson = decodeEventPayload(txn, decomp, eventPayload, nullptr, nullptr);

                    auto eventJsonObj = tao::json::from_string(eventJson);
                    std::string searchableText = Tokenizer::extractText(eventJsonObj);

                    auto queryTokens = Tokenizer::parseQuery(*f.search);
                    auto eventTokens = Tokenizer::tokenize(searchableText);

                    flat_hash_set<std::string> eventTokenSet;
                    for (const auto &token : eventTokens) {
                        eventTokenSet.insert(token.text);
                    }

                    bool allTokensMatch = true;
                    for (const auto &queryToken : queryTokens) {
                        if (eventTokenSet.find(queryToken) == eventTokenSet.end()) {
                            allTokensMatch = false;
                            break;
                        }
                    }

                    if (!allTokensMatch) continue;
                } catch (...) {
                    continue;
                }
            }

            if (!f.doesMatch(PackedEventView(view->buf))) continue;

            if (handleEvent(levId)) return true;
        }

        return true;
    }
};
