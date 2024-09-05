#pragma once

#include <tao/json.hpp>

#include "golpe.h"


inline const std::string &jsonGetString(const tao::json::value &v, std::string_view errMsg) {
    if (v.is_string()) return v.get_string();
    throw herr(errMsg);
}

inline uint64_t jsonGetUnsigned(const tao::json::value &v, std::string_view errMsg) {
    if (v.is_unsigned()) return v.get_unsigned();
    throw herr(errMsg);
}

inline const std::vector<tao::json::value> &jsonGetArray(const tao::json::value &v, std::string_view errMsg) {
    if (v.is_array()) return v.get_array();
    throw herr(errMsg);
}
