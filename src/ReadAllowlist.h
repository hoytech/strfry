#pragma once

#include <sys/stat.h>

#include <fstream>
#include <string>

#include <hoytech/hex.h>
#include <hoytech/time.h>

#include "golpe.h"

#include "Bytes32.h"


// Optional per-pubkey read gating (relay.auth.readAllowlistFile).
//
// When configured, every REQ, COUNT and NEG-OPEN requires the connection to be
// NIP-42 authenticated as a pubkey listed in the file (one 64-char hex pubkey
// per line, '#' comments allowed). The file is maintained by an external tool,
// such as a payment plugin, and re-read when its modification time changes.
// One instance lives in each ingester thread, so no locking is needed.

struct ReadAllowlist {
    enum class Result { Allowed, AuthRequired, NotAllowed };

    Result check(const Bytes32 &authed) {
        const auto &path = cfg().relay__auth__readAllowlistFile;
        if (path.empty()) return Result::Allowed;

        refresh(path);

        if (authed.isNull()) return Result::AuthRequired;
        return pubkeys.contains(authed) ? Result::Allowed : Result::NotAllowed;
    }

  private:
    flat_hash_set<Bytes32> pubkeys;
    std::string loadedPath;
    struct timespec loadedMtime = {};
    uint64_t lastCheck = 0;

    void refresh(const std::string &path) {
        uint64_t now = hoytech::curr_time_us();
        if (path == loadedPath && now - lastCheck < 1'000'000) return; // stat at most once per second
        lastCheck = now;

        struct stat st;
        if (::stat(path.c_str(), &st) != 0) {
            if (path != loadedPath || !pubkeys.empty()) LW << "readAllowlistFile not readable, denying all reads: " << path;
            pubkeys.clear();
            loadedPath = path;
            loadedMtime = {};
            return;
        }

        if (path == loadedPath && st.st_mtim.tv_sec == loadedMtime.tv_sec && st.st_mtim.tv_nsec == loadedMtime.tv_nsec) return;

        flat_hash_set<Bytes32> next;
        std::ifstream f(path);
        std::string line;
        while (std::getline(f, line)) {
            auto start = line.find_first_not_of(" \t\r");
            if (start == std::string::npos || line[start] == '#') continue;
            auto hex = line.substr(start, 64);
            if (hex.size() != 64) continue;
            try {
                next.insert(Bytes32(hoytech::from_hex(hex)));
            } catch (std::exception &e) {
                LW << "readAllowlistFile: skipping bad line: " << line;
            }
        }

        pubkeys = std::move(next);
        loadedPath = path;
        loadedMtime = st.st_mtim;
    }
};
