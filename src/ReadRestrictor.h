#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string_view>

#include "PackedEvent.h"
#include "config.h"
#include "filters.h"

#include "golpe.h"

struct ReadRestrictor {
private:
    using KindSet = flat_hash_set<uint64_t>;

    // Immutable snapshots are published atomically so worker threads can read
    // the restriction set without racing a concurrent config reload. The old
    // implementation cleared and refilled a shared flat_hash_set from multiple
    // threads while others were iterating it (undefined behaviour).
    inline static std::atomic<std::shared_ptr<const KindSet>> restrictedKinds_{
        std::make_shared<const KindSet>()
    };
    inline static std::atomic<uint64_t> configVer{0};
    inline static std::mutex initMutex;

public:
    static std::shared_ptr<const KindSet> restrictedKinds() {
        uint64_t ver = cfg().version();

        auto current = restrictedKinds_.load(std::memory_order_acquire);
        if (configVer.load(std::memory_order_acquire) == ver) return current;

        std::lock_guard<std::mutex> lock(initMutex);

        // Another thread may have refreshed the snapshot while we waited.
        if (configVer.load(std::memory_order_acquire) == ver) {
            return restrictedKinds_.load(std::memory_order_acquire);
        }

        auto next = std::make_shared<KindSet>();
        parseCommaSeparatedKinds(cfg().relay__auth__restrictedReadKinds, *next);

        restrictedKinds_.store(std::move(next), std::memory_order_release);
        configVer.store(ver, std::memory_order_release);

        return restrictedKinds_.load(std::memory_order_acquire);
    }

    static bool isFilterGroupFullyRestricted(const NostrFilterGroup &fg) {
        auto restricted = restrictedKinds();
        if (restricted->empty() || fg.filters.empty()) return false;

        for (const auto& f : fg.filters) {
            if (!isFilterFullyRestricted(f, *restricted)) return false;
        }
        return true;
    }

    static bool isFilterFullyRestricted(const NostrFilter &filter, const KindSet &restricted) {
        if (!filter.kinds) return false;

        for (size_t i = 0; i < filter.kinds->size(); ++i) {
            uint64_t kind = filter.kinds->at(i);

            if (!restricted.contains(kind)) {
                return false;
            }
        }
        return true;
    }

    static bool isFilterAllowedToCount(const NostrFilterGroup &fg, Bytes32 pubkey) {
        auto restricted = restrictedKinds();
        if (restricted->empty()) return true;

        bool pubkeyIsNull = pubkey.isNull();

        for (const auto &f: fg.filters) {
            bool hasSomeRestrictedKind = false;

            if (!f.kinds) {
                // A filter without "kinds" matches every kind, including the
                // restricted ones. COUNT cannot filter individual events, so
                // treat it as containing all restricted kinds.
                hasSomeRestrictedKind = true;
            } else {
                for (size_t i = 0; i < f.kinds->size(); ++i) {
                    uint64_t kind = f.kinds->at(i);
                    if (restricted->contains(kind)) {
                        hasSomeRestrictedKind = true;
                        break;
                    }
                }
            }

            if (hasSomeRestrictedKind) {
                if (pubkeyIsNull) {
                    return false;
                }
                if (!cfg().relay__auth__restrictReadToInvolvedPubkey) {
                    continue;
                }
                bool authorScoped = f.authors && allPubkeysMatch(*f.authors, pubkey);
                bool pScoped = false;
                if (auto it = f.tags.find('p'); it != f.tags.end()) {
                    pScoped = allPubkeysMatch(it->second, pubkey);
                }
                if (!authorScoped && !pScoped) return false;
            }
        }
        return true;
    }

    static bool allPubkeysMatch(const FilterSetBytes& set, Bytes32 authed) {
        if (set.size() == 0) return false;

        for (size_t i = 0; i < set.size(); ++i) {
            Bytes32 val(set.at(i));
            if (val != authed) return false;
        }
        return true;
    }

    // Returns true if the event should be sent to the subscriber
    static bool shouldSendToSubscriber(const PackedEventView &packed, const Bytes32 &subscriberAuthedPubkey) {
        auto restricted = restrictedKinds();
        if (!restricted->contains(packed.kind())) {
            return true;
        }

        if (subscriberAuthedPubkey.isNull()) {
            return false;
        }

        if(!cfg().relay__auth__restrictReadToInvolvedPubkey) return true;

        bool involved = subscriberAuthedPubkey == packed.pubkey();

        packed.foreachTag([&](char tagName, std::string_view tagVal) {
            if (tagName == 'p' && tagVal.size() == 32) {
                if (subscriberAuthedPubkey == Bytes32(tagVal)) {
                    involved = true;
                    return false;
                }
            }

            return true;
        });

        return involved;
    }
};
