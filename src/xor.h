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
        std::cerr << "ADDELEM " << to_hex(elems.back().getCompare(idSize)) << std::endl;
    }

    void finalise() {
        std::reverse(elems.begin(), elems.end()); // typically pushed in approximately descending order so this may speed up the sort

        std::sort(elems.begin(), elems.end(), [&](const auto &a, const auto &b) { return a.getCompare(idSize) < b.getCompare(idSize); });

        ready = true;

        for (auto &e : elems) std::cerr << "FIN " << to_hex(e.getCompare(idSize)) << std::endl;
    }

    std::string initialQuery() {
        if (!ready) throw herr("xor view not ready");

        std::string output;
        splitRange(elems.begin(), elems.end(), std::string(1, '\x00'), std::string(1, '\xFF'), output);
        return output;
    }

    // FIXME: try/catch everywhere that calls this
    std::string handleQuery(std::string_view query, std::vector<std::string> &haveIds, std::vector<std::string> &needIds) {
    std::cerr << "-------------------" << std::endl;
        if (!ready) throw herr("xor view not ready");

        std::string output;

        auto cmp = [&](const auto &a, const auto &b){ return a.getCompare(idSize) < b.getCompare(idSize); };

        while (query.size()) {
    std::cerr << "=========" << std::endl;
            uint64_t lowerLength = decodeVarInt(query);
            if (lowerLength > idSize + 5) throw herr("lower too long: ", lowerLength);
            auto lowerKeyRaw = getBytes(query, lowerLength);
            XorElem lowerKey(lowerKeyRaw);
            std::cerr << "LWCMP: " << to_hex(lowerKey.getCompare(idSize)) << std::endl;

            uint64_t upperLength = decodeVarInt(query);
            if (upperLength > idSize + 5) throw herr("upper too long");
            auto upperKeyRaw = getBytes(query, upperLength);
            XorElem upperKey(upperKeyRaw);
            std::cerr << "UPCMP: " << to_hex(upperKey.getCompare(idSize)) << std::endl;

            auto lower = std::lower_bound(elems.begin(), elems.end(), lowerKey, cmp); // FIXME: start at prev upper?
            auto upper = std::upper_bound(elems.begin(), elems.end(), upperKey, cmp); // FIXME: start at lower?
            std::cerr << "FOUND: " << size_t(upper-lower) << std::endl;
            for (auto it = lower; it < upper; ++it) std::cerr << "MMM: " << to_hex(it->getId(idSize)) << std::endl;

            uint64_t mode = decodeVarInt(query); // 0 = range, 8 and above = n-8 inline IDs

            if (mode == 0) {
            std::cerr << "MODE 0" << std::endl;
                XorElem theirXorSet(0, getBytes(query, idSize), idSize);

                XorElem ourXorSet;
                for (auto i = lower; i < upper; ++i) ourXorSet.doXor(*i);

                std::cerr << "XSETS " << to_hex(theirXorSet.getId(idSize)) << " / " << to_hex(ourXorSet.getId(idSize)) << std::endl;
                if (theirXorSet.getId(idSize) != ourXorSet.getId(idSize)) splitRange(lower, upper, lowerKeyRaw, upperKeyRaw, output);
            } else if (mode >= 8) {
            std::cerr << "MODE " << (mode - 8) << std::endl;
                flat_hash_map<XorElem, bool> theirElems;
                for (uint64_t i = 0; i < mode - 8; i++) {
                    auto bb = getBytes(query, idSize);
                    std::cerr << "INSERTED THEIR " << to_hex(bb) << std::endl;
                    theirElems.emplace(XorElem(0, bb, idSize), false);
                }

                for (auto it = lower; it < upper; ++it) {
                    std::cerr << "SEARCHING " << to_hex(it->getId(idSize)) << std::endl;
                    auto e = theirElems.find(*it);

                    if (e == theirElems.end()) {
                        // ID exists on our side, but not their side
                        haveIds.emplace_back(it->getId(idSize));
                    } else {
                        // ID exists on both sides
                        e->second = true;
                        std::cerr << "ERMM " << to_hex(e->first.getId(idSize)) << std::endl;
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

    void splitRange(std::vector<XorElem>::iterator lower, std::vector<XorElem>::iterator upper, const std::string &lowerKey, const std::string &upperKey, std::string &output) {
        // Split our range
        uint64_t numElems = upper - lower;
        const uint64_t buckets = 16;

        if (numElems < buckets * 2) {
            std::cerr << "DUMPING IDs" << std::endl;
            appendBoundKey(lowerKey /*getLowerKey(lower)*/, output);
            appendBoundKey(upperKey /*getUpperKey(upper)*/, output);

            output += encodeVarInt(numElems + 8);
            for (auto it = lower; it < upper; ++it) output += it->getId(idSize);
            for (auto it = lower; it < upper; ++it) std::cerr << "DUMP ID: " << to_hex(it->getId(idSize)) << std::endl;
        } else {
            std::cerr << "DOING SPLIT" << std::endl;
            uint64_t elemsPerBucket = numElems / buckets;
            uint64_t bucketsWithExtra = numElems % buckets;
            auto curr = lower;

            for (uint64_t i = 0; i < buckets; i++) {
                appendBoundKey(i == 0 ? lowerKey : getLowerKey(curr), output);

                XorElem ourXorSet;
                for (auto bucketEnd = curr + elemsPerBucket + (i < bucketsWithExtra ? 1 : 0); curr != bucketEnd; curr++) {
                std::cerr << "XORING IN " << to_hex(curr->getId(idSize)) << std::endl;
                    ourXorSet.doXor(*curr);
                }

                appendBoundKey(i == buckets - 1 ? upperKey : getUpperKey(curr), output);

                output += encodeVarInt(0); // mode = 0
                output += ourXorSet.getId(idSize);
                std::cerr << "FULL XOR " << to_hex(ourXorSet.getId(idSize)) << std::endl;
            }
        }
    }

    void appendBoundKey(std::string k, std::string &output) {
        std::cerr << "ABK: " << to_hex(k) << std::endl;
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
