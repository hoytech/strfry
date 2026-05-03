#pragma once

#include "golpe.h"

#include "jsonParseUtils.h"


struct FilterSetBytes : NonCopyable {
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

struct FilterSetUint : NonCopyable {
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

struct NostrFilter : NonCopyable {
    std::optional<FilterSetBytes> ids;
    std::optional<FilterSetBytes> authors;
    std::optional<FilterSetUint> kinds;
    flat_hash_map<char, FilterSetBytes> tags;
    std::optional<std::string> search;

    uint64_t since = 0;
    uint64_t until = MAX_U64;
    uint64_t limit = MAX_U64;
    bool neverMatch = false;
    bool indexOnlyScans = false;

    explicit NostrFilter(const tao::json::value &filterObj, uint64_t maxFilterLimit) {
        uint64_t numMajorFields = 0;

        if (!filterObj.is_object()) throw herr("provided filter is not an object");

        for (const auto &[k, v] : filterObj.get_object()) {
            auto checkArray = [&]{
                if (!v.is_array()) throw herr(k, " not an array");
            };

            if (k == "ids") {
                checkArray();
                if (v.get_array().size() == 0) {
                    neverMatch = true;
                    continue;
                }
                numMajorFields++;

                try {
                    ids.emplace(v, true, 32, 32);
                } catch (std::exception &e) {
                    throw herr("error parsing ids: ", e.what());
                }
            } else if (k == "authors") {
                checkArray();
                if (v.get_array().size() == 0) {
                    neverMatch = true;
                    continue;
                }
                numMajorFields++;

                try {
                    authors.emplace(v, true, 32, 32);
                } catch (std::exception &e) {
                    throw herr("error parsing authors: ", e.what());
                }
            } else if (k == "kinds") {
                checkArray();
                if (v.get_array().size() == 0) {
                    neverMatch = true;
                    continue;
                }
                numMajorFields++;

                try {
                    kinds.emplace(v);
                } catch (std::exception &e) {
                    throw herr("error parsing kinds: ", e.what());
                }
            } else if (k.starts_with('#')) {
                checkArray();
                if (v.get_array().size() == 0) {
                    neverMatch = true;
                    continue;
                }
                numMajorFields++;

                try {
                    if (k.size() == 2) {
                        char tag = k[1];

                        if (tag == 'p' || tag == 'e') {
                            tags.emplace(tag, FilterSetBytes(v, true, 32, 32));
                        } else {
                            tags.emplace(tag, FilterSetBytes(v, false, 0, MAX_INDEXED_TAG_VAL_SIZE));
                        }
                    } else {
                        throw herr("unindexed tag filter");
                    }
                } catch (std::exception &e) {
                    throw herr("error parsing ", k, ": ", e.what());
                }
            } else if (k == "since") {
                since = jsonGetUnsigned(v, "error parsing since");
            } else if (k == "until") {
                until = jsonGetUnsigned(v, "error parsing until");
            } else if (k == "limit") {
                limit = jsonGetUnsigned(v, "error parsing limit");
            } else if (k == "search") {
                if (!v.is_string()) throw herr("search must be a string");
                search.emplace(v.get_string());
                // Note: search is not counted in numMajorFields to preserve indexOnlyScans heuristics
            } else {
                throw herr("unrecognised filter item: ", k);
            }
        }

        if (tags.size() > cfg().relay__maxTagsPerFilter) throw herr("too many tags in filter"); // O(N^2) in matching, so prevent it from being too large

        if (limit > maxFilterLimit) limit = maxFilterLimit;

        indexOnlyScans = (numMajorFields <= 1) || (numMajorFields == 2 && authors && kinds);
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

    bool hasSearch() const {
        return search.has_value() && !search->empty();
    }
};

struct NostrFilterGroup : NonCopyable {
    std::vector<NostrFilter> filters;

    NostrFilterGroup() {}

    NostrFilterGroup(const tao::json::value &filter, uint64_t maxFilterLimit = cfg().relay__maxFilterLimit) {
        addFilters(filter, maxFilterLimit);
    }

    static NostrFilterGroup fromReq(const tao::json::value &req, uint64_t maxFilterLimit = cfg().relay__maxFilterLimit) {
        const auto &arr = req.get_array();
        if (arr.size() < 3) throw herr("too small");

        NostrFilterGroup fg;

        for (size_t i = 2; i < arr.size(); i++) {
            fg.addFilter(arr[i], maxFilterLimit);
        }

        return fg;
    }

    void addFilter(const tao::json::value &filterItem, uint64_t maxFilterLimit = cfg().relay__maxFilterLimit) {
        filters.emplace_back(filterItem, maxFilterLimit);
        if (filters.back().neverMatch) filters.pop_back();
    }

    void addFilters(const tao::json::value &filter, uint64_t maxFilterLimit = cfg().relay__maxFilterLimit) {
        if (!filter.is_array()) {
            addFilter(filter, maxFilterLimit);
        } else {
            for (const auto &e : filter.get_array()) {
                addFilter(e, maxFilterLimit);
            }
        }
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

struct FilterValidator : NonCopyable {
    uint64_t configVer = 0;
    flat_hash_set<uint64_t> allowedKinds;

    void setupValidator() {
        allowedKinds.clear();

        std::string allowedKindsStr = cfg().relay__filterValidation__allowedKinds;

        if (!allowedKindsStr.empty()) {
            size_t pos = 0;
            while (pos < allowedKindsStr.size()) {
                size_t nextComma = allowedKindsStr.find(',', pos);
                if (nextComma == std::string::npos) nextComma = allowedKindsStr.size();

                std::string kindStr = allowedKindsStr.substr(pos, nextComma - pos);
                size_t start = kindStr.find_first_not_of(" \t");
                size_t end = kindStr.find_last_not_of(" \t");
                if (start != std::string::npos && end != std::string::npos) {
                    kindStr = kindStr.substr(start, end - start + 1);
                    if (!kindStr.empty()) allowedKinds.insert(std::stoull(kindStr));
                }

                pos = nextComma + 1;
            }
        }
    }

    void validate(const NostrFilterGroup &fg) {
        if (!cfg().relay__filterValidation__enabled) return;

        if (configVer != cfg().version()) {
            setupValidator();
            configVer = cfg().version();
        }

        size_t numFilters = fg.filters.size();
        if (numFilters < cfg().relay__filterValidation__minFiltersPerReq ||
            numFilters > cfg().relay__filterValidation__maxFiltersPerReq) {
            throw herr("invalid number of filters: ", numFilters);
        }

        for (const auto &filter : fg.filters) {
            if (filter.kinds) {
                size_t numKinds = filter.kinds->size();
                if (numKinds > cfg().relay__filterValidation__maxKindsPerFilter) {
                    throw herr("too many kinds in filter: ", numKinds);
                }

                if (!allowedKinds.empty()) {
                    for (size_t i = 0; i < numKinds; i++) {
                        uint64_t kind = filter.kinds->at(i);
                        if (!allowedKinds.contains(kind)) throw herr("kind not allowed: ", kind);
                    }
                }
            }

            if (cfg().relay__filterValidation__requireAuthorOrTag) {
                bool hasValidAuthor = filter.authors && filter.authors->size() == 1;
                bool hasValidPTag = false;
                bool hasValidETag = false;

                auto pTagIt = filter.tags.find('p');
                if (pTagIt != filter.tags.end()) {
                    hasValidPTag = pTagIt->second.size() == 1;
                }

                auto eTagIt = filter.tags.find('e');
                if (eTagIt != filter.tags.end()) {
                    hasValidETag = eTagIt->second.size() == 1;
                }

                if (!hasValidAuthor && !hasValidPTag && !hasValidETag) {
                    throw herr("filter must have exactly one author, p tag, or e tag");
                }
            }
        }
    }
};
