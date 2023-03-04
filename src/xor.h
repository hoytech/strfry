#pragma once

#include "transport.h"


struct XorElem {
    char data[5 * 8];

    XorElem() {
        memset(data, '\0', sizeof(data));
    }

    XorElem(uint64_t created, std::string_view id, uint64_t idSize) {
        memset(data, '\0', sizeof(data));
        data[3] = (created >> (4*8)) & 0xFF;
        data[4] = (created >> (3*8)) & 0xFF;
        data[5] = (created >> (2*8)) & 0xFF;
        data[6] = (created >> (1*8)) & 0xFF;
        data[7] = (created >> (0*8)) & 0xFF;
        memcpy(data + 8, id.data(), idSize);
    }

    XorElem(std::string_view id) {
        memset(data, '\0', sizeof(data));
        memcpy(data + 3, id.data(), id.size());
    }

    std::string_view getCompare(uint64_t idSize) const {
        return std::string_view(data + 3, idSize + 5);
    }

    std::string_view getId(uint64_t idSize) const {
        return std::string_view(data + 8, idSize);
    }

    std::string_view getIdPadded() const {
        return std::string_view(data + 8, 32);
    }

    std::string_view getFull() const {
        return std::string_view(data, sizeof(data));
    }

    bool isZero() {
        uint64_t *ours = reinterpret_cast<uint64_t*>(data + 8);
        return ours[0] == 0 && ours[1] == 0 && ours[2] == 0 && ours[3] == 0;
    }

    void doXor(const XorElem &other) {
        uint64_t *ours = reinterpret_cast<uint64_t*>(data + 8);
        const uint64_t *theirs = reinterpret_cast<const uint64_t*>(other.data + 8);

        ours[0] ^= theirs[0];
        ours[1] ^= theirs[1];
        ours[2] ^= theirs[2];
        ours[3] ^= theirs[3];
    }

    bool operator==(const XorElem &o) const {
        return o.getIdPadded() == getIdPadded();
    }
};

struct XorView {
    uint64_t idSize;

    std::vector<XorElem> elems;
    bool ready = false;

    XorView(uint64_t idSize) : idSize(idSize) {
        if (idSize < 8 || idSize > 32) throw herr("idSize out of range");
    }

    void addElem(uint64_t createdAt, std::string_view id) {
        elems.emplace_back(createdAt, id, idSize);
    }

    void finalise() {
        std::reverse(elems.begin(), elems.end()); // typically pushed in approximately descending order so this may speed up the sort

        std::sort(elems.begin(), elems.end(), [&](const auto &a, const auto &b) { return a.getCompare(idSize) < b.getCompare(idSize); });

        ready = true;
    }

    std::string initialQuery() {
        if (!ready) throw herr("xor view not ready");

        std::string output;
        splitRange(elems.begin(), elems.end(), output);
        return output;
    }

    // FIXME: try/catch everywhere that calls this
    std::string handleQuery(std::string_view query, std::vector<std::string> &haveIds, std::vector<std::string> &needIds) {
        if (!ready) throw herr("xor view not ready");

        std::string output;

        auto cmp = [&](const auto &a, const auto &b){ return a.getCompare(idSize) < b.getCompare(idSize); };

        while (query.size()) {
            uint64_t lowerLength = decodeVarInt(query);
            if (lowerLength > idSize + 5) throw herr("lower too long");
            XorElem lowerKey(getBytes(query, lowerLength));

            uint64_t upperLength = decodeVarInt(query);
            if (upperLength > idSize + 5) throw herr("upper too long");
            XorElem upperKey(getBytes(query, upperLength));

            auto lower = std::lower_bound(elems.begin(), elems.end(), lowerKey, cmp); // FIXME: start at prev upper?
            auto upper = std::upper_bound(elems.begin(), elems.end(), upperKey, cmp); // FIXME: start at lower?

            uint64_t mode = decodeVarInt(query); // 0 = range, 8 and above = n-8 inline IDs
            std::cerr << "BING MODE = " << mode << std::endl;

            if (mode == 0) {
                XorElem theirXorSet(0, getBytes(query, idSize), idSize);

                XorElem ourXorSet;
                for (auto i = lower; i < upper; ++i) ourXorSet.doXor(*i);

                if (theirXorSet.getId(idSize) != ourXorSet.getId(idSize)) splitRange(lower, upper, output);
            } else if (mode >= 8) {
                flat_hash_map<XorElem, bool> theirElems;
                for (uint64_t i = 0; i < mode - 8; i++) {
                    theirElems.emplace(XorElem(0, getBytes(query, idSize), idSize), false);
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
            }
        }

        return output;
    }

  private:

    void splitRange(std::vector<XorElem>::iterator lower, std::vector<XorElem>::iterator upper, std::string &output) {
        // Split our range
        uint64_t numElems = upper - lower;
        const uint64_t buckets = 16;

        if (numElems < buckets * 2) {
            appendBoundKey(getLowerKey(lower), output);
            appendBoundKey(getUpperKey(upper), output);

            output += encodeVarInt(numElems + 8);
            for (auto it = lower; it < upper; ++it) output += it->getId(idSize);
        } else {
            uint64_t elemsPerBucket = numElems / buckets;
            uint64_t bucketsWithExtra = numElems % buckets;
            auto curr = lower;

            for (uint64_t i = 0; i < buckets; i++) {
                appendBoundKey(getLowerKey(curr), output);

                auto bucketEnd = curr + elemsPerBucket;
                if (i < bucketsWithExtra) bucketEnd++;

                XorElem ourXorSet;
                for (auto bucketEnd = curr + elemsPerBucket + (i < bucketsWithExtra ? 1 : 0); curr != bucketEnd; curr++) {
                    ourXorSet.doXor(*curr);
                }

                appendBoundKey(getUpperKey(curr), output);

                output += ourXorSet.getId(idSize);
            }
        }
    }

    void appendBoundKey(std::string k, std::string &output) {
        output += encodeVarInt(k.size());
        output += k;
    }

    std::string getLowerKey(std::vector<XorElem>::iterator it) {
        if (it == elems.begin()) return std::string(1, '\0');
        return minimalKeyDiff(it->getCompare(idSize), std::prev(it)->getCompare(idSize));
    }

    std::string getUpperKey(std::vector<XorElem>::iterator it) {
        if (it == elems.end()) return std::string(1, '\xFF');
        return minimalKeyDiff(it->getCompare(idSize), std::prev(it)->getCompare(idSize));
    }

    std::string minimalKeyDiff(std::string_view key, std::string_view prevKey) {
        for (uint64_t i = 0; i < idSize + 5; i++) {
            if (key[i] != prevKey[i]) return std::string(key.substr(0, i + 1));
        }

        throw herr("couldn't compute shared prefix");
    }
};


namespace std {
    // inject specialization of std::hash
    template<> struct hash<XorElem> {
        std::size_t operator()(XorElem const &p) const {
            return phmap::HashState().combine(0, p.getIdPadded());
        }
    };
}
