#pragma once

#include <string>

#include "re2/re2.h"


struct Url {
    std::vector<std::string_view> path;
    std::string_view query;

    Url(std::string_view u) {
        size_t pos;

        if ((pos = u.find("?")) != std::string::npos) {
            query = u.substr(pos + 1);
            u = u.substr(0, pos);
        }

        while ((pos = u.find("/")) != std::string::npos) {
            if (pos != 0) path.emplace_back(u.substr(0, pos));
            u = u.substr(pos + 1);
        }

        if (u.size()) path.emplace_back(u);
    }

    std::optional<std::string_view> lookupQuery(std::string_view key) {
        std::string_view curr = query;

        while (curr.size()) {
            auto nextPos = curr.find("&");

            {
                std::string_view currKV = nextPos == std::string::npos ? curr : curr.substr(0, nextPos);
                std::string_view k, v;

                auto equalsPos = currKV.find("=");
                if (equalsPos == std::string::npos) {
                    k = currKV;
                } else {
                    k = currKV.substr(0, equalsPos);
                    v = currKV.substr(equalsPos + 1);
                }

                if (k == key) return v;
            }

            if (nextPos == std::string::npos) break;
            curr = curr.substr(nextPos + 1);
        }

        return std::nullopt;
    }
};

inline std::string renderTimestamp(uint64_t now, uint64_t ts) {
    uint64_t delta = now > ts ? now - ts : ts - now;

    const uint64_t A = 60;
    const uint64_t B = A*60;
    const uint64_t C = B*24;
    const uint64_t D = C*30.5;
    const uint64_t E = D*12;

    std::string output;

    if      (delta < B) output += std::to_string(delta / A) + " minutes";
    else if (delta < C) output += std::to_string(delta / B) + " hours";
    else if (delta < D) output += std::to_string(delta / C) + " days";
    else if (delta < E) output += std::to_string(delta / D) + " months";
    else                output += std::to_string(delta / E) + " years";

    if (now > ts) output += " ago";
    else output += " in the future";

    return output;
}

inline std::string renderPoints(double points) {
    char buf[100];

    snprintf(buf, sizeof(buf), "%g", points);

    return std::string(buf);
}

inline std::string stripUrls(std::string &content) {
    static RE2 matcher(R"((?is)(.*?)(https?://\S+))");

    std::string output;
    std::string firstUrl;

    std::string_view contentSv(content);
    re2::StringPiece input(contentSv);
    re2::StringPiece prefix, match;

    auto sv = [](re2::StringPiece s){ return std::string_view(s.data(), s.size()); };

    while (RE2::Consume(&input, matcher, &prefix, &match)) {
        output += sv(prefix);

        if (firstUrl.empty()) {
            firstUrl = std::string(sv(match));
        }
    }

    output += std::string_view(input.data(), input.size());

    std::swap(output, content);
    return firstUrl;
}

inline void abbrevText(std::string &origStr, size_t maxLen) {
    if (maxLen < 10) throw herr("abbrev too short");
    if (origStr.size() <= maxLen) return;

    std::string str = origStr.substr(0, maxLen-3);

    {
        // If string ends in a multi-byte UTF-8 encoded code-point, chop it off.
        // This avoids cutting in the middle of an encoded code-point. It's a 99%
        // solution, not perfect. See: https://metacpan.org/pod/Unicode::Truncate

        auto endsInUtf8Extension = [&](){
            return str.size() && (str.back() & 0b1100'0000) == 0b1000'0000;
        };

        if (endsInUtf8Extension()) {
            do str.pop_back(); while (endsInUtf8Extension());
            if (str.size()) str.pop_back();
        }
    }

    str += "...";

    std::swap(origStr, str);
}
