#include <arpa/inet.h>
#ifdef __FreeBSD__
#include <sys/socket.h>
#include <netinet/in.h>
#endif
#include <stdio.h>
#include <signal.h>

#include <algorithm>
#include <string>

#include "golpe.h"


std::string renderIP(std::string_view ipBytes) {
    char buf[128];

    if (ipBytes.size() == 4) {
        inet_ntop(AF_INET, ipBytes.data(), buf, sizeof(buf));
    } else if (ipBytes.size() == 16) {
        inet_ntop(AF_INET6, ipBytes.data(), buf, sizeof(buf));
    } else {
        throw herr("invalid size of ipBytes, unable to render IP");
    }

    return std::string(buf);
}

std::string parseIP(const std::string &ip) {
    int af = ip.find(':') != std::string::npos ? AF_INET6 : AF_INET;
    unsigned char buf[16];

    int ret = inet_pton(af, ip.c_str(), &buf[0]);
    if (ret == 0) return "";

    return std::string((const char*)&buf[0], af == AF_INET6 ? 16 : 4);
}


std::string renderSize(uint64_t si) {
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



std::string renderPercent(double p) {
    char buf[128];
    ::snprintf(buf, sizeof(buf), "%.1f%%", p * 100);
    return std::string(buf);
}



uint64_t parseUint64(const std::string &s) {
    auto digitChar = [](char c){
        return c >= '0' && c <= '9';
    };

    if (!std::all_of(s.begin(), s.end(), digitChar)) throw herr("non-digit character");

    return std::stoull(s);
}



uint64_t getDBVersion(lmdb::txn &txn) {
    uint64_t dbVersion;

    {
        auto s = env.lookup_Meta(txn, 1);

        if (s) {
            dbVersion = s->dbVersion();
        } else {
            dbVersion = 0;
        }
    }

    return dbVersion;
}


void exitOnSigPipe() {
    struct sigaction act;
    memset(&act, 0, sizeof act);
    act.sa_sigaction = [](int, siginfo_t*, void*){ ::exit(1); };
    if (sigaction(SIGPIPE, &act, nullptr)) throw herr("couldn't run sigaction(): ", strerror(errno));
}
