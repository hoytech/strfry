#pragma once

#include "filters.h"


struct SubId {
    char buf[40];

    SubId(std::string_view val) {
        static_assert(MAX_SUBID_SIZE == 39, "MAX_SUBID_SIZE mismatch");
        if (val.size() > 39) throw herr("subscription id too long");
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
};

inline bool operator <(const SubId &s1, const SubId &s2) {
    return s1.sv() < s2.sv();
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
