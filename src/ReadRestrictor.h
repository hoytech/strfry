#pragma once

#include <string_view>
#include "PackedEvent.h"
#include "Bytes32.h"
#include "config.h"
#include "filters.h"
#include "global.h"
#include "parallel_hashmap/phmap.h"

struct ReadRestrictor {
private:
    inline static uint64_t configVer = 0;
    inline static flat_hash_set<uint64_t> restrictedKinds_;

public:
    static void init(){
        parseCommaSeparatedKinds(cfg().relay__auth__restrictedReadKinds, restrictedKinds_);
    }

    static flat_hash_set<uint64_t>& restrictedKinds() {
        if (configVer != cfg().version()) {
            init();
            configVer = cfg().version();
        }
        return restrictedKinds_;
    }

    static bool isFilterGroupFullyRestricted(const NostrFilterGroup &fg) {
        if (restrictedKinds().empty() || fg.filters.empty()) return false;
        for (const auto& f : fg.filters) {
            if (!isFilterFullyRestricted(f)) return false;
        }
        return true;
    }

    static bool isFilterFullyRestricted(const NostrFilter &filter) {
        if (!filter.kinds) return false;

        for (size_t i = 0; i < filter.kinds->size(); ++i) {
            uint64_t kind = filter.kinds->at(i);

            if (!restrictedKinds().contains(kind)) {
                return false;
            }
        }
        return true;
    }

    static bool isFilterAllowedToCount(const NostrFilterGroup &fg, Bytes32 pubkey) {
        if (restrictedKinds().empty()) return true;
        for (const auto &f: fg.filters) {
            if (!f.kinds) continue;
            bool hasSomeRestrictedKind = false;
            for(size_t i = 0; i<f.kinds->size(); ++i) {
                uint64_t kind = f.kinds->at(i);
                if (restrictedKinds().contains(kind)) {
                    hasSomeRestrictedKind = true;
                    break;
                }
            }
            if (hasSomeRestrictedKind) {
                if (pubkey.isNull()) {
                    return false;
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
        if (!(restrictedKinds().contains(packed.kind()) && cfg().relay__auth__restrictReadToInvolvedPubkey)) {
            return true;
        }

        if (subscriberAuthedPubkey.isNull()) {
            return false;
        }

        Bytes32 recipientPubkey;
        bool foundRecipient = false;

        packed.foreachTag([&](char tagName, std::string_view tagVal) {
            if (tagName == 'p' && tagVal.size() == 32) {
                recipientPubkey = Bytes32(tagVal);
                foundRecipient = true;
                return false;
            }
            return true;
        });

        if (!foundRecipient) {
            return false;
        }

        return subscriberAuthedPubkey == recipientPubkey || subscriberAuthedPubkey == packed.pubkey();
    }
};
