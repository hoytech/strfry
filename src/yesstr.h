#pragma once

#include "golpe.h"


inline void verifyYesstrRequest(std::string_view msg) {
    if (!msg.starts_with("Y")) throw herr("invalid yesstr magic char");
    msg = msg.substr(1);
    auto verifier = flatbuffers::Verifier(reinterpret_cast<const uint8_t*>(msg.data()), msg.size());
    bool ok = verifier.VerifyBuffer<Yesstr::Request>(nullptr);
    if (!ok) throw herr("yesstr request verification failed");
}

inline void verifyYesstrResponse(std::string_view msg) {
    if (!msg.starts_with("Y")) throw herr("invalid yesstr magic char");
    msg = msg.substr(1);
    auto verifier = flatbuffers::Verifier(reinterpret_cast<const uint8_t*>(msg.data()), msg.size());
    bool ok = verifier.VerifyBuffer<Yesstr::Response>(nullptr);
    if (!ok) throw herr("yesstr response verification failed");
}


inline const Yesstr::Request *parseYesstrRequest(std::string_view msg) {
    return flatbuffers::GetRoot<Yesstr::Request>(msg.substr(1).data());
}

inline const Yesstr::Response *parseYesstrResponse(std::string_view msg) {
    return flatbuffers::GetRoot<Yesstr::Response>(msg.substr(1).data());
}
