#pragma once

#include <string_view>
#include "PackedEvent.h"
#include "Bytes32.h"
#include "config.h"
#include "global.h"
#include "parallel_hashmap/phmap.h"
#include "filters.h"

struct ReadRestrictor {
public:
    inline static flat_hash_set<uint64_t>restrictedKinds;

    static void init(){
        parseCommaSeparatedKinds(cfg().relay__auth__restrictedReadKinds, restrictedKinds);
    }

   static bool includesRestrictedKind(const NostrFilterGroup& fg) {
    for (const auto& f : fg.filters) {
        if (!f.kinds) return true;

        for (const auto& kind : restrictedKinds) {
            if (f.kinds->doesMatch(kind)) {
                return true;
            }
        }
    }
    return false;
} 

    // Returns true if the event should be sent to the subscriber
    static bool shouldSendToSubscriber(const PackedEventView &packed, const Bytes32 &subscriberAuthedPubkey) {
        if (!(restrictedKinds.contains(packed.kind()) && cfg().relay__auth__restrictReadToInvolvedPubkey)) {
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

    static bool shouldSendToSubscriber(std::string_view eventPayload, const Bytes32 &subscriberAuthedPubkey) {
        try {
            PackedEventView packed(eventPayload);
            return shouldSendToSubscriber(packed, subscriberAuthedPubkey);
        } catch (...) {
            //todo: think about this
            return true;
        }
    }
};
