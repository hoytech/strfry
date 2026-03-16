#pragma once

#include <string>

#include "golpe.h"


inline uint64_t parseTime(const std::string &str) {
    if (str.size() == 0) throw herr("invalid time");

    char unit = str.back();
    double scale;

    if (unit == 's') scale = 1;
    else if (unit == 'm') scale = 60;
    else if (unit == 'h') scale = 60 * 60;
    else if (unit == 'd') scale = 86400;
    else if (unit == 'w') scale = 86400 * 7;
    else if (unit == 'M') scale = 86400 * 30.5;
    else if (unit == 'Y') scale = 86400 * 365.2425;
    else throw herr("unknow time unit: ", unit);

    double seconds = std::stod(str.substr(0, str.size() - 1)) * scale;

    return (uint64_t)seconds;
}

inline void processRangeOption(const std::string &rangeStr, tao::json::value &filterJson) {
    if (filterJson.get_object().contains("since") || filterJson.get_object().contains("until")) throw herr("can't specify a --range AND since/until in filter");

    std::vector<std::string_view> segments;
    size_t endPos = rangeStr.find('-');

    if (endPos == std::string_view::npos) throw herr("range param is missing -");
    std::string sinceStr = rangeStr.substr(0, endPos);
    std::string untilStr = rangeStr.substr(endPos + 1);

    auto now = hoytech::curr_time_s();

    if (sinceStr.size()) filterJson["since"] = now - parseTime(sinceStr);
    if (untilStr.size()) filterJson["until"] = now - parseTime(untilStr);

    if (sinceStr.size() && untilStr.size()) {
        if (filterJson["since"].get_unsigned() > filterJson["until"].get_unsigned()) throw herr("since can't be after until");
    }
}
