#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <string_view>

#include "Bytes32.h"

struct AuthSession {

  static constexpr size_t kChallengeSize = 22;

  std::array<char, kChallengeSize> challenge{};
  Bytes32 authed;

  AuthSession(std::string_view token) {
    if (token.size() != kChallengeSize)
      throw herr("challenge size not ", kChallengeSize, " bytes");
    ::memcpy(challenge.data(), token.data(), kChallengeSize);
  }

  void markAuthed(Bytes32 pubkey) { authed = pubkey; }

  bool isAuthed() const { return !authed.isNull(); }

  std::string_view challengeSv() const {
    return std::string_view(challenge.data(), challenge.size());
  }
};
