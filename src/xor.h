#pragma once

#include "transport.h"


struct XorElem {
    uint64_t timestamp;
    char id[32];

    XorElem() : timestamp(0) {
        memset(id, '\0', sizeof(id));
    }

    XorElem(uint64_t timestamp, std::string_view id_) : timestamp(timestamp) {
        memset(id, '\0', sizeof(id));
        memcpy(id, id_.data(), std::min(id_.size(), sizeof(id)));
    }

    std::string_view getId(uint64_t idSize) const {
        return std::string_view(id, idSize);
    }

    XorElem& operator^=(const XorElem &other) {
        for (size_t i = 0; i < 32; i++) id[i] ^= other.id[i];
        return *this;
    }

    bool operator==(const XorElem &other) const {
        return getId(32) == other.getId(32); // ignore timestamp
    }
};

inline bool operator<(const XorElem &a, const XorElem &b) {
    return a.timestamp != b.timestamp ? a.timestamp < b.timestamp : a.getId(32) < b.getId(32);
};


struct XorView {
    uint64_t idSize;

    std::vector<XorElem> elems;
    bool ready = false;

    XorView(uint64_t idSize) : idSize(idSize) {
        if (idSize < 8 || idSize > 32) throw herr("idSize out of range");
    }

    void addElem(uint64_t createdAt, std::string_view id) {
        elems.emplace_back(createdAt, id);
    }

    void finalise() {
        std::reverse(elems.begin(), elems.end()); // typically pushed in approximately descending order so this may speed up the sort
        std::sort(elems.begin(), elems.end());
        ready = true;
    }

    std::string initial() {
        if (!ready) throw herr("xor view not ready");

        std::string output;
        uint64_t lastTimestampOut = 0;
        splitRange(elems.begin(), elems.end(), 0, "", MAX_U64, "", lastTimestampOut, output);
        return output;
    }

    // FIXME: better name for this function, check try/catch everywhere that calls this
    std::string reconcile(std::string_view query, std::vector<std::string> &haveIds, std::vector<std::string> &needIds) {
        if (!ready) throw herr("xor view not ready");

        std::string output;
        auto prevUpper = elems.begin();
        uint64_t lastTimestampIn = 0;
        uint64_t lastTimestampOut = 0;

        auto decodeTimestampIn = [&]{
            uint64_t timestamp = decodeVarInt(query);
            timestamp = timestamp == 0 ? MAX_U64 : timestamp - 1;
            timestamp += lastTimestampIn;
            if (timestamp < lastTimestampIn) timestamp = MAX_U64; // saturate
            lastTimestampIn = timestamp;
            return timestamp;
        };

        auto decodeBoundKey = [&](uint64_t &timestamp, std::string &key){
            timestamp = decodeTimestampIn();
            uint64_t len = decodeVarInt(query);
            if (len > idSize) throw herr("bound key too long");
            key = getBytes(query, len);
        };

        while (query.size()) {
            uint64_t lowerTimestamp, upperTimestamp;
            std::string lowerKey, upperKey;

            decodeBoundKey(lowerTimestamp, lowerKey);
            decodeBoundKey(upperTimestamp, upperKey);

            auto lower = std::lower_bound(prevUpper, elems.end(), XorElem(lowerTimestamp, lowerKey));
            auto upper = std::upper_bound(lower, elems.end(), XorElem(upperTimestamp, upperKey));
            prevUpper = upper;

            uint64_t mode = decodeVarInt(query); // 0 = range, 8 and above = n-8 inline IDs

            if (mode == 0) {
                XorElem theirXorSet(0, getBytes(query, idSize));

                XorElem ourXorSet;
                for (auto i = lower; i < upper; ++i) ourXorSet ^= *i;

                if (theirXorSet.getId(idSize) != ourXorSet.getId(idSize)) splitRange(lower, upper, lowerTimestamp, lowerKey, upperTimestamp, upperKey, lastTimestampOut, output);
            } else if (mode >= 8) {
                flat_hash_map<XorElem, bool> theirElems;
                for (uint64_t i = 0; i < mode - 8; i++) {
                    auto e = getBytes(query, idSize);
                    theirElems.emplace(XorElem(0, e), false);
                }

                for (auto it = lower; it < upper; ++it) {
                    auto e = theirElems.find(*it);

                    if (e == theirElems.end()) {
                        // ID exists on our side, but not their side
                        haveIds.emplace_back(it->getId(idSize));
                    } else {
                        // ID exists on both sides
                        e->second = true;
                    }
                }

                for (const auto &[k, v] : theirElems) {
                    if (!v) {
                        // ID exists on their side, but not our side
                        needIds.emplace_back(k.getId(idSize));
                    }
                }
            } else {
                throw herr("unexpected mode");
            }
        }

        return output;
    }

  private:

    void splitRange(std::vector<XorElem>::iterator lower, std::vector<XorElem>::iterator upper, uint64_t lowerTimestamp, const std::string &lowerKey, uint64_t upperTimestamp, const std::string &upperKey, uint64_t &lastTimestampOut, std::string &output) {
        auto encodeTimestampOut = [&](uint64_t timestamp){
            if (timestamp == MAX_U64) {
                lastTimestampOut = MAX_U64;
                return encodeVarInt(0);
            }

            uint64_t temp = timestamp;
            timestamp -= lastTimestampOut;
            lastTimestampOut = temp;
            return encodeVarInt(timestamp + 1);
        };

        auto appendBoundKey = [&](uint64_t t, std::string k) {
            output += encodeTimestampOut(t);
            output += encodeVarInt(k.size());
            output += k;
        };

        auto appendMinimalBoundKey = [&](const XorElem &curr, const XorElem &prev) {
            output += encodeTimestampOut(curr.timestamp);

            if (curr.timestamp != prev.timestamp) {
                output += encodeVarInt(0);
            } else {
                uint64_t sharedPrefixBytes = 0;
                auto currKey = curr.getId(idSize);
                auto prevKey = prev.getId(idSize);

                for (uint64_t i = 0; i < idSize; i++) {
                    if (currKey[i] != prevKey[i]) break;
                    sharedPrefixBytes++;
                }

                output += encodeVarInt(sharedPrefixBytes + 1);
                output += currKey.substr(0, sharedPrefixBytes + 1);
            }
        };

        // Split our range
        uint64_t numElems = upper - lower;
        const uint64_t buckets = 16;

        if (numElems < buckets * 2) {
            appendBoundKey(lowerTimestamp, lowerKey);
            appendBoundKey(upperTimestamp, upperKey);

            output += encodeVarInt(numElems + 8);
            for (auto it = lower; it < upper; ++it) output += it->getId(idSize);
        } else {
            uint64_t elemsPerBucket = numElems / buckets;
            uint64_t bucketsWithExtra = numElems % buckets;
            auto curr = lower;

            for (uint64_t i = 0; i < buckets; i++) {
                if (i == 0) appendBoundKey(lowerTimestamp, lowerKey);
                else appendMinimalBoundKey(*curr, *std::prev(curr));

                XorElem ourXorSet;
                for (auto bucketEnd = curr + elemsPerBucket + (i < bucketsWithExtra ? 1 : 0); curr != bucketEnd; curr++) {
                    ourXorSet ^= *curr;
                }

                if (i == buckets - 1) appendBoundKey(upperTimestamp, upperKey);
                else appendMinimalBoundKey(*curr, *std::prev(curr));

                output += encodeVarInt(0); // mode = 0
                output += ourXorSet.getId(idSize);
            }
        }
    }
};


namespace std {
    // inject specialization of std::hash
    template<> struct hash<XorElem> {
        std::size_t operator()(XorElem const &p) const {
            return phmap::HashState().combine(0, p.getId(32));
        }
    };
}
