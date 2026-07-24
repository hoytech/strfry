#pragma once

#include <string>
#include <vector>
#include <algorithm>
#include <sstream>
#include <cctype>
#include "golpe.h"


class KindMatcher {
private:
    std::vector<std::pair<uint64_t, uint64_t>> inclusionRanges;
    std::vector<std::pair<uint64_t, uint64_t>> exclusionRanges;
    bool hasWildcard = false;
    bool parseError = false;
    std::string errorMessage;

    static void mergeRanges(std::vector<std::pair<uint64_t, uint64_t>> &ranges) {
        if (ranges.empty()) return;

        std::sort(ranges.begin(), ranges.end());

        std::vector<std::pair<uint64_t, uint64_t>> merged;
        merged.push_back(ranges[0]);

        for (size_t i = 1; i < ranges.size(); i++) {
            auto &last = merged.back();
            auto &curr = ranges[i];

            if (curr.first <= last.second + 1) {
                last.second = std::max(last.second, curr.second);
            } else {
                merged.push_back(curr);
            }
        }

        ranges = std::move(merged);
    }

    static bool parseToken(const std::string &token, bool &isWildcard, bool &isExclusion,
                           uint64_t &start, uint64_t &end, std::string &error) {
        std::string trimmed;
        for (char c : token) {
            if (!std::isspace(c)) trimmed += c;
        }

        if (trimmed.empty()) return false;

        if (trimmed == "*") {
            isWildcard = true;
            return true;
        }

        isExclusion = false;
        size_t pos = 0;
        if (trimmed[0] == '-') {
            isExclusion = true;
            pos = 1;
        }

        size_t dashPos = trimmed.find('-', pos);

        if (dashPos == std::string::npos) {
            try {
                start = end = std::stoull(trimmed.substr(pos));
                return true;
            } catch (...) {
                error = "Invalid kind number: " + trimmed;
                return false;
            }
        } else {
            try {
                start = std::stoull(trimmed.substr(pos, dashPos - pos));
                end = std::stoull(trimmed.substr(dashPos + 1));

                if (start > end) {
                    error = "Invalid range (start > end): " + trimmed;
                    return false;
                }

                return true;
            } catch (...) {
                error = "Invalid range format: " + trimmed;
                return false;
            }
        }
    }

public:
    KindMatcher() = default;

    static KindMatcher parse(const std::string &config) {
        KindMatcher matcher;

        if (config.empty()) {
            matcher.inclusionRanges.push_back({1, 1});
            matcher.inclusionRanges.push_back({30023, 30023});
            return matcher;
        }

        std::vector<std::string> tokens;
        std::stringstream ss(config);
        std::string token;
        while (std::getline(ss, token, ',')) {
            tokens.push_back(token);
        }

        for (const auto &token : tokens) {
            bool isWildcard = false;
            bool isExclusion = false;
            uint64_t start, end;
            std::string error;

            if (!parseToken(token, isWildcard, isExclusion, start, end, error)) {
                if (!error.empty()) {
                    matcher.parseError = true;
                    matcher.errorMessage = error;
                    LE << "KindMatcher parse error: " << error;
                    return matcher;
                }
                continue;
            }

            if (isWildcard) {
                matcher.hasWildcard = true;
            } else if (isExclusion) {
                matcher.exclusionRanges.push_back({start, end});
            } else {
                matcher.inclusionRanges.push_back({start, end});
            }
        }

        mergeRanges(matcher.inclusionRanges);
        mergeRanges(matcher.exclusionRanges);

        return matcher;
    }

    bool matches(uint64_t kind) const {
        if (parseError) return false;

        bool included = false;

        if (hasWildcard) {
            included = true;
        } else {
            for (const auto &[start, end] : inclusionRanges) {
                if (kind >= start && kind <= end) {
                    included = true;
                    break;
                }
            }
        }

        if (!included) return false;

        for (const auto &[start, end] : exclusionRanges) {
            if (kind >= start && kind <= end) {
                return false;
            }
        }

        return true;
    }

    bool hasError() const { return parseError; }
    const std::string& getError() const { return errorMessage; }

    std::string toString() const {
        std::ostringstream oss;
        if (parseError) {
            oss << "ERROR: " << errorMessage;
            return oss.str();
        }

        if (hasWildcard) {
            oss << "* ";
        }
        for (const auto &[start, end] : inclusionRanges) {
            if (start == end) {
                oss << start << " ";
            } else {
                oss << start << "-" << end << " ";
            }
        }
        for (const auto &[start, end] : exclusionRanges) {
            if (start == end) {
                oss << "-" << start << " ";
            } else {
                oss << "-" << start << "-" << end << " ";
            }
        }
        return oss.str();
    }
};
