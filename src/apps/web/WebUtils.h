#pragma once

#include <string>


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
