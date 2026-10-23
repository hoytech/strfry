#pragma once

#include <optional>
#include <string>

#include <tao/json.hpp>

#include "golpe.h"


// Helpers for the optional NIP-11 config params (relay.info.fees and the
// limitation overrides). Used when rendering the NIP-11 document and by
// checkConfig() at startup, so a bad value is reported before the relay runs.

namespace nip11 {

// Parses relay.info.fees. Returns nullopt when empty. Throws on invalid input.
//
// Expected shape (NIP-11 "Pay-to-Relay"):
//   {"admission":    [{"amount": 1000000, "unit": "msats"}],
//    "subscription": [{"amount": 5000000, "unit": "msats", "period": 2592000}],
//    "publication":  [{"kinds": [4], "amount": 100, "unit": "msats"}]}
inline std::optional<tao::json::value> parseFees(const std::string &str) {
    if (str.empty()) return std::nullopt;

    auto fees = tao::json::from_string(str);
    if (!fees.is_object()) throw herr("fees must be a JSON object");

    for (const auto &[category, entries] : fees.get_object()) {
        if (category != "admission" && category != "subscription" && category != "publication") {
            throw herr("unknown fee category '", category, "' (expected admission, subscription or publication)");
        }
        if (!entries.is_array()) throw herr("fees.", category, " must be an array");

        for (const auto &e : entries.get_array()) {
            if (!e.is_object()) throw herr("fees.", category, " entries must be objects");
            if (!e.find("amount") || !e.at("amount").is_integer() || e.at("amount").as<int64_t>() < 0) throw herr("fees.", category, ": amount must be a non-negative integer");
            if (!e.find("unit") || !e.at("unit").is_string()) throw herr("fees.", category, ": unit must be a string");

            if (auto *p = e.find("period"); p && (!p->is_integer() || p->as<int64_t>() <= 0)) throw herr("fees.", category, ": period must be a positive integer (seconds)");

            if (auto *k = e.find("kinds")) {
                if (!k->is_array()) throw herr("fees.", category, ": kinds must be an array");
                for (const auto &kind : k->get_array()) {
                    if (!kind.is_integer() || kind.as<int64_t>() < 0) throw herr("fees.", category, ": kinds must be non-negative integers");
                }
            }
        }
    }

    return fees;
}

// Parses a tri-state override: "" = automatic (nullopt), "true", "false". Throws otherwise.
inline std::optional<bool> parseOverride(const std::string &str) {
    if (str.empty()) return std::nullopt;
    if (str == "true") return true;
    if (str == "false") return false;
    throw herr("expected \"true\", \"false\" or \"\" (automatic), got \"", str, "\"");
}

}
