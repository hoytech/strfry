#pragma once

#include "golpe.h"

#include "Subscription.h"
#include "filters.h"


struct DBScan {
    const NostrFilter &f;
    uint64_t remainingLimit;

    struct NullState {
    };

    struct IdScan {
        size_t index = 0;
        std::string prefix;
    };

    struct PubkeyKindScan {
        size_t indexAuthor = 0;
        size_t indexKind = 0;
        std::string prefix;
    };

    struct PubkeyScan {
        size_t index = 0;
        std::string prefix;
    };

    struct TagScan {
        std::map<char, FilterSetBytes>::const_iterator indexTagName;
        size_t indexTagVal = 0;
        std::string search;
    };

    struct KindScan {
        size_t index = 0;
        uint64_t kind;
    };

    struct CreatedAtScan {
        bool done = false;
    };

    std::variant<NullState, IdScan, PubkeyKindScan, PubkeyScan, TagScan, KindScan, CreatedAtScan> scanState = NullState{};
    lmdb::dbi indexDbi;
    std::string resumeKey;
    uint64_t resumeVal;

    std::function<bool()> isComplete;
    std::function<void()> nextFilterItem;
    std::function<void()> resetResume;
    std::function<bool(std::string_view, bool&)> keyMatch;

    DBScan(const NostrFilter &f_) : f(f_) {
        remainingLimit = f.limit;

        if (f.ids.size()) {
            LI << "ID Scan";

            scanState = IdScan{};
            auto *state = std::get_if<IdScan>(&scanState);
            indexDbi = env.dbi_Event__id;

            isComplete = [&, state]{
                return state->index >= f.ids.size();
            };
            nextFilterItem = [&, state]{
                state->index++;
            };
            resetResume = [&, state]{
                state->prefix = f.ids.at(state->index);
                resumeKey = padBytes(state->prefix, 32 + 8, '\xFF');
                resumeVal = MAX_U64;
            };
            keyMatch = [&, state](std::string_view k, bool&){
                return k.starts_with(state->prefix);
            };
        } else if (f.authors.size() && f.kinds.size()) {
            LI << "PubkeyKind Scan";

            scanState = PubkeyKindScan{};
            auto *state = std::get_if<PubkeyKindScan>(&scanState);
            indexDbi = env.dbi_Event__pubkeyKind;

            isComplete = [&, state]{
                return state->indexAuthor >= f.authors.size();
            };
            nextFilterItem = [&, state]{
                state->indexKind++;
                if (state->indexKind >= f.kinds.size()) {
                    state->indexAuthor++;
                    state->indexKind = 0;
                }
            };
            resetResume = [&, state]{
                state->prefix = f.authors.at(state->indexAuthor);
                if (state->prefix.size() == 32) state->prefix += lmdb::to_sv<uint64_t>(f.kinds.at(state->indexKind));
                resumeKey = padBytes(state->prefix, 32 + 8 + 8, '\xFF');
                resumeVal = MAX_U64;
            };
            keyMatch = [&, state](std::string_view k, bool &skipBack){
                if (!k.starts_with(state->prefix)) return false;
                if (state->prefix.size() == 32 + 8) return true;

                ParsedKey_StringUint64Uint64 parsedKey(k);
                if (parsedKey.n1 <= f.kinds.at(state->indexKind)) return true;

                resumeKey = makeKey_StringUint64Uint64(parsedKey.s, f.kinds.at(state->indexKind), MAX_U64);
                resumeVal = MAX_U64;
                skipBack = true;
                return false;
            };
        } else if (f.authors.size()) {
            LI << "Pubkey Scan";

            scanState = PubkeyScan{};
            auto *state = std::get_if<PubkeyScan>(&scanState);
            indexDbi = env.dbi_Event__pubkey;

            isComplete = [&, state]{
                return state->index >= f.authors.size();
            };
            nextFilterItem = [&, state]{
                state->index++;
            };
            resetResume = [&, state]{
                state->prefix = f.authors.at(state->index);
                resumeKey = padBytes(state->prefix, 32 + 8, '\xFF');
                resumeVal = MAX_U64;
            };
            keyMatch = [&, state](std::string_view k, bool&){
                return k.starts_with(state->prefix);
            };
        } else if (f.tags.size()) {
            LI << "Tag Scan";

            scanState = TagScan{f.tags.begin()};
            auto *state = std::get_if<TagScan>(&scanState);
            indexDbi = env.dbi_Event__tag;

            isComplete = [&, state]{
                return state->indexTagName == f.tags.end();
            };
            nextFilterItem = [&, state]{
                state->indexTagVal++;
                if (state->indexTagVal >= state->indexTagName->second.size()) {
                    state->indexTagName = std::next(state->indexTagName);
                    state->indexTagVal = 0;
                }
            };
            resetResume = [&, state]{
                state->search = state->indexTagName->first;
                state->search += state->indexTagName->second.at(state->indexTagVal);
                resumeKey = state->search + std::string(8, '\xFF');
                resumeVal = MAX_U64;
            };
            keyMatch = [&, state](std::string_view k, bool&){
                return k.substr(0, state->search.size()) == state->search;
            };
        } else if (f.kinds.size()) {
            LI << "Kind Scan";

            scanState = KindScan{};
            auto *state = std::get_if<KindScan>(&scanState);
            indexDbi = env.dbi_Event__kind;

            isComplete = [&, state]{
                return state->index >= f.kinds.size();
            };
            nextFilterItem = [&, state]{
                state->index++;
            };
            resetResume = [&, state]{
                state->kind = f.kinds.at(state->index);
                resumeKey = std::string(lmdb::to_sv<uint64_t>(state->kind)) + std::string(8, '\xFF');
                resumeVal = MAX_U64;
            };
            keyMatch = [&, state](std::string_view k, bool&){
                ParsedKey_Uint64Uint64 parsedKey(k);
                return parsedKey.n1 == state->kind;
            };
        } else {
            LI << "CreatedAt Scan";

            scanState = CreatedAtScan{};
            auto *state = std::get_if<CreatedAtScan>(&scanState);
            indexDbi = env.dbi_Event__created_at;

            isComplete = [&, state]{
                return state->done;
            };
            nextFilterItem = [&, state]{
                state->done = true;
            };
            resetResume = [&, state]{
                resumeKey = std::string(8, '\xFF');
                resumeVal = MAX_U64;
            };
            keyMatch = [&, state](std::string_view k, bool&){
                return true;
            };
        }
    }

    // If scan is complete, returns true
    bool scan(lmdb::txn &txn, std::function<void(uint64_t)> handleEvent, std::function<bool()> doPause) {
        while (remainingLimit && !isComplete()) {
            if (resumeKey == "") resetResume();

            bool pause = false, skipBack = false;

            env.generic_foreachFull(txn, indexDbi, resumeKey, lmdb::to_sv<uint64_t>(resumeVal), [&](auto k, auto v) {
                if (doPause()) {
                    resumeKey = std::string(k);
                    resumeVal = lmdb::from_sv<uint64_t>(v);
                    LI << "SAVING resumeKey: " << to_hex(resumeKey) << " / " << resumeVal;
                    pause = true;
                    return false;
                }

                if (!keyMatch(k, skipBack)) return false;

                uint64_t created;

                {
                    ParsedKey_StringUint64 parsedKey(k);
                    created = parsedKey.n;

                    if ((f.since && created < f.since)) {
                        resumeKey = makeKey_StringUint64(parsedKey.s, 0);
                        resumeVal = 0;
                        skipBack = true;
                        return false;
                    }

                    if (f.until && created > f.until) {
                        resumeKey = makeKey_StringUint64(parsedKey.s, f.until);
                        resumeVal = MAX_U64;
                        skipBack = true;
                        return false;
                    }
                }

                bool sent = false;
                uint64_t quadId = lmdb::from_sv<uint64_t>(v);

                if (f.indexOnlyScans) {
                    if (f.doesMatchTimes(created)) {
                        handleEvent(quadId);
                        sent = true;
                    }
                } else {
                    auto view = env.lookup_Event(txn, quadId);
                    if (!view) throw herr("missing event from index, corrupt DB?");
                    if (f.doesMatch(view->flat_nested())) {
                        handleEvent(quadId);
                        sent = true;
                    }
                }

                if (sent) {
                    if (remainingLimit) remainingLimit--;
                    if (!remainingLimit) return false;
                }

                return true;
            }, true);

            if (pause) return false;

            if (!skipBack) {
                nextFilterItem();
                resumeKey = "";
            }
        }

        return true;
    }

    std::string padBytes(std::string_view str, size_t n, char padChar) {
        if (str.size() > n) throw herr("unable to pad, string longer than expected");
        return std::string(str) + std::string(n - str.size(), padChar);
    }
};


struct DBScanQuery : NonCopyable {
    Subscription sub;
    std::unique_ptr<DBScan> scanner;

    size_t filterGroupIndex = 0;
    bool dead = false;
    std::unordered_set<uint64_t> alreadySentEvents; // FIXME: flat_set here, or roaring bitmap/judy/whatever

    DBScanQuery(Subscription &sub_) : sub(std::move(sub_)) {}

    // If scan is complete, returns true
    bool process(lmdb::txn &txn, uint64_t timeBudgetMicroseconds, std::function<void(const Subscription &, uint64_t)> cb) {
        uint64_t startTime = hoytech::curr_time_us();

        while (filterGroupIndex < sub.filterGroup.size()) {
            if (!scanner) scanner = std::make_unique<DBScan>(sub.filterGroup.filters[filterGroupIndex]);

            bool complete = scanner->scan(txn, [&](uint64_t quadId){
                // If this event came in after our query began, don't send it. It will be sent after the EOSE.
                if (quadId > sub.latestEventId) return;

                // We already sent this event
                if (alreadySentEvents.find(quadId) != alreadySentEvents.end()) return;
                alreadySentEvents.insert(quadId);

                cb(sub, quadId);
            }, [&]{
                return hoytech::curr_time_us() - startTime > timeBudgetMicroseconds;
            });

            if (!complete) return false;

            filterGroupIndex++;
            scanner.reset();
        }

        return true;
    }
};
