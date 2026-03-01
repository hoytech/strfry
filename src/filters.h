#pragma once

#include "golpe.h"


struct FilterSetBytes {
    struct Item {
        uint16_t offset;
        uint8_t size;
        uint8_t firstByte;
    };

    std::vector<Item> items;
    std::string buf;

    // Sizes are post-hex decode 

    FilterSetBytes(const tao::json::value &arrHex, bool hexDecode, size_t minSize, size_t maxSize) {
        if (maxSize > MAX_INDEXED_TAG_VAL_SIZE) throw herr("maxSize bigger than max indexed tag size");

        std::vector<std::string> arr;

        for (const auto &i : arrHex.get_array()) {
            arr.emplace_back(hexDecode ? from_hex(i.get_string(), false) : i.get_string());
            size_t itemSize = arr.back().size();
            if (itemSize < minSize) throw herr("filter item too small");
            if (itemSize > maxSize) throw herr("filter item too large");
        }

        std::sort(arr.begin(), arr.end());

        for (size_t i = 0; i < arr.size(); i++) {
            const auto &item = arr[i];
            if (i > 0 && item == arr[i - 1]) continue; // remove duplicates
            items.emplace_back(Item{ (uint16_t)buf.size(), (uint8_t)item.size(), (uint8_t)item[0] });
            buf += item;
        }

        if (buf.size() > 65535) throw herr("total filter items too large");
    }

    // Direct constructor from already-decoded values
    FilterSetBytes(const std::vector<std::string> &arrBytes, size_t minSize, size_t maxSize) {
        if (maxSize > MAX_INDEXED_TAG_VAL_SIZE) throw herr("maxSize bigger than max indexed tag size");

        std::vector<std::string> arr = arrBytes;
        std::sort(arr.begin(), arr.end());

        for (size_t i = 0; i < arr.size(); i++) {
            const auto &item = arr[i];
            if (item.size() < minSize) throw herr("filter item too small");
            if (item.size() > maxSize) throw herr("filter item too large");
            if (i > 0 && item == arr[i - 1]) continue; // remove duplicates
            items.emplace_back(Item{ (uint16_t)buf.size(), (uint8_t)item.size(), (uint8_t)item[0] });
            buf += item;
        }

        if (buf.size() > 65535) throw herr("total filter items too large");
    }

    std::string at(size_t n) const {
        if (n >= items.size()) throw herr("FilterSetBytes access out of bounds");
        auto &item = items[n];
        return buf.substr(item.offset, item.size);
    }

    size_t size() const {
        return items.size();
    }

    bool doesMatch(std::string_view candidate) const {
        // Binary search for upper-bound: https://en.cppreference.com/w/cpp/algorithm/upper_bound

        ssize_t first = 0, last = items.size(), curr;
        ssize_t count = last - first, step;

        while (count > 0) {
            curr = first; 
            step = count / 2;
            curr += step;

            bool comp = (candidate.size() && items[curr].size && (uint8_t)candidate[0] != items[curr].firstByte)
                        ? (uint8_t)candidate[0] < items[curr].firstByte
                        : candidate < std::string_view(buf.data() + items[curr].offset, items[curr].size);
     
            if (!comp) {
                first = ++curr;
                count -= step + 1;
            } else {
                count = step;
            }
        }

        if (first == 0) return false;
        if (candidate == std::string_view(buf.data() + items[first - 1].offset, items[first - 1].size)) return true;

        return false;
    }
};

struct FilterSetUint {
    std::vector<uint64_t> items;

    FilterSetUint(const tao::json::value &arr) {
        for (const auto &i : arr.get_array()) {
            items.push_back(i.get_unsigned());
        }

        std::sort(items.begin(), items.end());

        items.erase(std::unique(items.begin(), items.end()), items.end()); // remove duplicates
    }

    uint64_t at(size_t n) const {
        if (n >= items.size()) throw herr("FilterSetBytes access out of bounds");
        return items[n];
    }

    size_t size() const {
        return items.size();
    }

    bool doesMatch(uint64_t candidate) const {
        return std::binary_search(items.begin(), items.end(), candidate);
    }
};

struct NostrFilter {
    std::optional<FilterSetBytes> ids;
    std::optional<FilterSetBytes> authors;
    std::optional<FilterSetUint> kinds;
    flat_hash_map<char, FilterSetBytes> tags;
    flat_hash_map<char, FilterSetBytes> tagsAnd;

    uint64_t since = 0;
    uint64_t until = MAX_U64;
    uint64_t limit = MAX_U64;
    bool neverMatch = false;
    bool indexOnlyScans = false;

    explicit NostrFilter(const tao::json::value &filterObj, uint64_t maxFilterLimit) {
        uint64_t numMajorFieldsNonTag = 0;
        flat_hash_set<char> tagKeySet;
        flat_hash_map<char, std::vector<std::string>> rawTagsOr;
        flat_hash_map<char, std::vector<std::string>> rawTagsAnd;

        if (!filterObj.is_object()) throw herr("provided filter is not an object");

        for (const auto &[k, v] : filterObj.get_object()) {
            if (v.is_array() && v.get_array().size() == 0) {
                neverMatch = true;
                continue;
            }

            if (k == "ids") {
                ids.emplace(v, true, 32, 32);
                numMajorFieldsNonTag++;
            } else if (k == "authors") {
                authors.emplace(v, true, 32, 32);
                numMajorFieldsNonTag++;
            } else if (k == "kinds") {
                kinds.emplace(v);
                numMajorFieldsNonTag++;
            } else if (k.starts_with('#') || k.starts_with('&')) {
                bool isAnd = k.starts_with('&');
                if (k.size() != 2) throw herr(isAnd ? "unindexed AND tag filter" : "unindexed tag filter");

                char tag = k[1];
                tagKeySet.insert(tag);

                auto &vec = isAnd ? rawTagsAnd[tag] : rawTagsOr[tag];
                for (const auto &elem : v.get_array()) {
                    if (tag == 'p' || tag == 'e') {
                        vec.emplace_back(from_hex(elem.get_string(), false));
                    } else {
                        vec.emplace_back(elem.get_string());
                    }
                }
            } else if (k == "since") {
                since = v.get_unsigned();
            } else if (k == "until") {
                until = v.get_unsigned();
            } else if (k == "limit") {
                limit = v.get_unsigned();
            } else {
                throw herr("unrecognised filter item");
            }
        }

        // Build AND sets first
        for (const auto &[tagName, vals] : rawTagsAnd) {
            if (tagName == 'p' || tagName == 'e') {
                tagsAnd.emplace(tagName, FilterSetBytes(vals, 32, 32));
            } else {
                tagsAnd.emplace(tagName, FilterSetBytes(vals, 0, MAX_INDEXED_TAG_VAL_SIZE));
            }
        }

        // Build OR sets, skipping any values present in AND for the same tag
        for (const auto &[tagName, vals] : rawTagsOr) {
            const auto andIt = tagsAnd.find(tagName);
            std::vector<std::string> filtered;
            filtered.reserve(vals.size());

            for (const auto &v : vals) {
                if (andIt != tagsAnd.end() && andIt->second.doesMatch(v)) continue;
                filtered.emplace_back(v);
            }

            if (filtered.empty()) continue;

            if (tagName == 'p' || tagName == 'e') {
                tags.emplace(tagName, FilterSetBytes(filtered, 32, 32));
            } else {
                tags.emplace(tagName, FilterSetBytes(filtered, 0, MAX_INDEXED_TAG_VAL_SIZE));
            }
        }

        size_t tagKeyCount = 0;
        {
            // tagKeySet already contains the union of keys seen in # and &
            tagKeyCount = tagKeySet.size();
        }

        if (tagKeyCount > 3) throw herr("too many tags in filter"); // O(N^2) in matching, just prohibit it

        if (limit > maxFilterLimit) limit = maxFilterLimit;

        uint64_t numMajorFields = numMajorFieldsNonTag + tagKeyCount;

        indexOnlyScans = (numMajorFields <= 1) || (numMajorFields == 2 && authors && kinds);
        if (tagsAnd.size()) indexOnlyScans = false; // AND semantics require reading full events
    }

    bool doesMatchTimes(uint64_t created) const {
        if (created < since) return false;
        if (created > until) return false;
        return true;
    }

    bool doesMatch(PackedEventView ev) const {
        if (neverMatch) return false;

        if (!doesMatchTimes(ev.created_at())) return false;

        if (ids && !ids->doesMatch(ev.id())) return false;
        if (authors && !authors->doesMatch(ev.pubkey())) return false;
        if (kinds && !kinds->doesMatch(ev.kind())) return false;

        // AND tags: every value in tagsAnd[tag] must be present in the event
        for (const auto &[tag, filt] : tagsAnd) {
            for (size_t i = 0; i < filt.size(); i++) {
                auto requiredVal = filt.at(i);
                bool foundMatch = false;

                ev.foreachTag([&](char tagName, std::string_view tagVal){
                    if (tagName == tag && tagVal == requiredVal) {
                        foundMatch = true;
                        return false;
                    }
                    return true;
                });

                if (!foundMatch) return false;
            }
        }

        for (const auto &[tag, filt] : tags) {
            bool foundMatch = false;

            ev.foreachTag([&](char tagName, std::string_view tagVal){
                if (tagName == tag && filt.doesMatch(tagVal)) {
                    foundMatch = true;
                    return false;
                }
                return true;
            });

            if (!foundMatch) return false;
        }

        return true;
    }

    bool isFullDbQuery() {
        return !ids && !authors && !kinds && tags.size() == 0;
    }
};

struct NostrFilterGroup {
    std::vector<NostrFilter> filters;

    NostrFilterGroup() {}

    // Note that this expects the full array, so the first two items are "REQ" and the subId
    NostrFilterGroup(const tao::json::value &req, uint64_t maxFilterLimit = cfg().relay__maxFilterLimit) {
        const auto &arr = req.get_array();
        if (arr.size() < 3) throw herr("too small");

        for (size_t i = 2; i < arr.size(); i++) {
            filters.emplace_back(arr[i], maxFilterLimit);
            if (filters.back().neverMatch) filters.pop_back();
        }
    }

    // FIXME refactor: Make unwrapped the default constructor
    static NostrFilterGroup unwrapped(tao::json::value filter, uint64_t maxFilterLimit = cfg().relay__maxFilterLimit) {
        if (!filter.is_array()) {
            filter = tao::json::value::array({ filter });
        }

        tao::json::value pretendReqQuery = tao::json::value::array({ "REQ", "junkSub" });

        for (auto &e : filter.get_array()) {
            pretendReqQuery.push_back(e);
        }

        return NostrFilterGroup(pretendReqQuery, maxFilterLimit);
    }

    bool doesMatch(PackedEventView ev) const {
        for (const auto &f : filters) {
            if (f.doesMatch(ev)) return true;
        }

        return false;
    }

    size_t size() const {
        return filters.size();
    }

    bool isFullDbQuery() {
        return size() == 1 && filters[0].isFullDbQuery();
    }
};
