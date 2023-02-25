#pragma once

#include <parallel_hashmap/phmap_utils.h>

#include "filters.h"


struct SubId {
    char buf[72];

    SubId(std::string_view val) {
        static_assert(MAX_SUBID_SIZE == 71, "MAX_SUBID_SIZE mismatch");
        if (val.size() > 71) throw herr("subscription id too long");
        if (val.size() == 0) throw herr("subscription id too short");

        auto badChar = [](char c){
            return c < 0x20 || c == '\\' || c == '"' || c >= 0x7F;
        };

        if (std::any_of(val.begin(), val.end(), badChar)) throw herr("invalid character in subscription id");

        buf[0] = (char)val.size();
        memcpy(&buf[1], val.data(), val.size());
    }

    std::string_view sv() const {
        return std::string_view(&buf[1], (size_t)buf[0]);
    }

    std::string str() const {
        return std::string(sv());
    }

    bool operator==(const SubId &o) const {
        return o.sv() == sv();
    }
};

namespace std {
    // inject specialization of std::hash
    template<> struct hash<SubId> {
        std::size_t operator()(SubId const &p) const {
            return phmap::HashState().combine(0, p.sv());
        }
    };
}


struct Subscription : NonCopyable {
    Subscription(uint64_t connId_, std::string subId_, NostrFilterGroup filterGroup_) : connId(connId_), subId(subId_), filterGroup(filterGroup_) {}

    // Params

    uint64_t connId;
    SubId subId;
    NostrFilterGroup filterGroup;

    // State

    uint64_t latestEventId = MAX_U64;
};


struct ConnIdSubId {
    uint64_t connId;
    SubId subId;
};

using RecipientList = std::vector<ConnIdSubId>;
