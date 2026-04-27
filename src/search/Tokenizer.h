#pragma once

#include <string>
#include <string_view>
#include <vector>
#include <cctype>
#include <algorithm>
#include "golpe.h"
#include <tao/json.hpp>


struct Token {
    std::string text;
    uint16_t tf = 1;
};

class Tokenizer {
public:
    static std::vector<Token> tokenize(std::string_view text) {
        std::vector<Token> tokens;
        flat_hash_map<std::string, uint16_t> termFreq;

        std::string currentToken;
        currentToken.reserve(64);

        for (char c : text) {
            if (std::isalnum(static_cast<unsigned char>(c))) {
                currentToken += std::tolower(static_cast<unsigned char>(c));
            } else if (!currentToken.empty()) {
                if (currentToken.size() >= 2 && currentToken.size() <= 48) {
                    auto &count = termFreq[currentToken];
                    if (count < 65535) count++;
                }
                currentToken.clear();
            }
        }

        if (!currentToken.empty() && currentToken.size() >= 2 && currentToken.size() <= 48) {
            auto &count = termFreq[currentToken];
            if (count < 65535) count++;
        }

        tokens.reserve(termFreq.size());
        for (const auto &[text, tf] : termFreq) {
            tokens.push_back(Token{text, tf});
        }

        return tokens;
    }

    static std::string extractText(const tao::json::value &eventJson) {
        std::string result;

        if (eventJson.find("content") != nullptr) {
            const auto &content = eventJson.at("content");
            if (content.is_string()) {
                result = content.get_string();
            }
        }

        if (eventJson.find("tags") != nullptr) {
            const auto &tags = eventJson.at("tags");
            if (tags.is_array()) {
                for (const auto &tag : tags.get_array()) {
                    if (tag.is_array() && tag.get_array().size() >= 2) {
                        const auto &tagName = tag.get_array()[0];
                        const auto &tagValue = tag.get_array()[1];

                        if (tagName.is_string() && tagName.get_string() == "subject" &&
                            tagValue.is_string()) {
                            if (!result.empty()) result += " ";
                            result += tagValue.get_string();
                        }
                    }
                }
            }
        }

        return result;
    }

    static std::vector<std::string> parseQuery(std::string_view query) {
        std::vector<std::string> tokens;
        std::string currentToken;
        currentToken.reserve(64);

        for (char c : query) {
            if (std::isalnum(static_cast<unsigned char>(c))) {
                currentToken += std::tolower(static_cast<unsigned char>(c));
            } else if (!currentToken.empty()) {
                if (currentToken.size() >= 2 && currentToken.size() <= 48) {
                    if (std::find(tokens.begin(), tokens.end(), currentToken) == tokens.end()) {
                        tokens.push_back(currentToken);
                    }
                }
                currentToken.clear();
            }
        }

        if (!currentToken.empty() && currentToken.size() >= 2 && currentToken.size() <= 48) {
            if (std::find(tokens.begin(), tokens.end(), currentToken) == tokens.end()) {
                tokens.push_back(currentToken);
            }
        }

        return tokens;
    }
};
