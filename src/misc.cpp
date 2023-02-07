#include <arpa/inet.h>

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
