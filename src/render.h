#pragma once

#include <stdio.h>


inline std::string renderSize(uint64_t si) {
    if (si < 1024) return std::to_string(si) + "b";

    double s = si;
    char buf[128];
    char unit;

    do {
        s /= 1024;
        if (s < 1024) {
            unit = 'K';
            break;
        }

        s /= 1024;
        if (s < 1024) {
            unit = 'M';
            break;
        }

        s /= 1024;
        if (s < 1024) {
            unit = 'G';
            break;
        }

        s /= 1024;
        unit = 'T';
    } while(0);

    ::snprintf(buf, sizeof(buf), "%.2f%c", s, unit);
    return std::string(buf);
}

inline std::string renderPercent(double p) {
    char buf[128];
    ::snprintf(buf, sizeof(buf), "%.1f%%", p * 100);
    return std::string(buf);
}
