#pragma once

#include "golpe.h"

#include "events.h"

#include "AlgoParser.h"



struct AlgoScanner {
    struct EventInfo {
        uint64_t comments = 0;
        double score = 0.0;
    };

    struct FilteredEvent {
        uint64_t levId;
        std::string id;

        EventInfo info;
    };

    AlgoCompiled a;


    AlgoScanner(lmdb::txn &txn, std::string_view algoText) : a(parseAlgo(txn, algoText)) {
    }


    std::vector<FilteredEvent> getEvents(lmdb::txn &txn, Decompressor &decomp, uint64_t limit) {
        flat_hash_map<std::string, EventInfo> eventInfoCache;
        std::vector<FilteredEvent> output;

        env.generic_foreachFull(txn, env.dbi_Event__created_at, lmdb::to_sv<uint64_t>(MAX_U64), lmdb::to_sv<uint64_t>(MAX_U64), [&](auto k, auto v) {
            if (output.size() > limit) return false;

            auto ev = lookupEventByLevId(txn, lmdb::from_sv<uint64_t>(v));
            PackedEventView packed(ev.buf);

            auto kind = packed.kind();
            auto id = packed.id();

            if (kind == 1) {
                auto pubkey = std::string(packed.pubkey());

                bool foundETag = false;
                packed.foreachTag([&](char tagName, std::string_view tagVal){
                    if (tagName == 'e') {
                        auto tagEventId = std::string(tagVal);
                        eventInfoCache.emplace(tagEventId, EventInfo{});
                        eventInfoCache[tagEventId].comments++;
                        foundETag = true;
                    }
                    return true;
                });
                if (foundETag) return true; // not root event

                eventInfoCache.emplace(id, EventInfo{});
                auto &eventInfo = eventInfoCache[id];

                if (a.voters && !a.voters->contains(pubkey)) return true;
                a.updateScore(txn, decomp, ev, eventInfo.score);
                if (eventInfo.score < a.threshold) return true;

                output.emplace_back(FilteredEvent{ev.primaryKeyId, std::string(id), eventInfo});
            } else if (kind == 7) {
                auto pubkey = std::string(packed.pubkey());
                //if (a.voters && !a.voters->contains(pubkey)) return true;

                std::optional<std::string_view> lastETag;
                packed.foreachTag([&](char tagName, std::string_view tagVal){
                    if (tagName == 'e') lastETag = tagVal;
                    return true;
                });

                if (lastETag) {
                    auto tagEventId = std::string(*lastETag);
                    eventInfoCache.emplace(tagEventId, EventInfo{});
                    eventInfoCache[tagEventId].score++;
                }
            }

            return true;
        }, true);

        //for (auto &o : output) {
        //}

        return output;
    }


    void loadFollowing(lmdb::txn &txn, std::string_view pubkey, flat_hash_set<std::string> &output) {
        const uint64_t kind = 3;

        env.generic_foreachFull(txn, env.dbi_Event__pubkeyKind, makeKey_StringUint64Uint64(pubkey, kind, 0), "", [&](std::string_view k, std
::string_view v){
            ParsedKey_StringUint64Uint64 parsedKey(k);

            if (parsedKey.s == pubkey && parsedKey.n1 == kind) {
                auto levId = lmdb::from_sv<uint64_t>(v);
                auto ev = lookupEventByLevId(txn, levId);
                PackedEventView packed(ev.buf);

                packed.foreachTag([&](char tagName, std::string_view tagVal){
                    if (tagName != 'p') return true;
                    output.insert(std::string(tagVal));
                    return true;
                });
            }

            return false;
        });
    }
};
