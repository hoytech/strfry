#pragma once

#include <string_view>
#include "PackedEvent.h"
#include "Bytes32.h"

struct DMFilter {
    // Returns true if the event should be sent to the subscriber
    static bool shouldSendToSubscriber(const PackedEventView &packed, const Bytes32 &subscriberAuthedPubkey) {
        if (packed.kind() != 4 && packed.kind() != 1059) {
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
