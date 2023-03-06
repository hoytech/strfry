#include <iostream>
#include <sstream>

#include "golpe.h"
#include "xor.h"



std::vector<std::string> split(const std::string &s, char delim) {
    std::vector<std::string> result;
    std::stringstream ss (s);
    std::string item;

    while (getline (ss, item, delim)) {
        result.push_back (item);
    }

    return result;
}



int main() {
    const uint64_t idSize = 16;

    // x1 is client, x2 is relay
    XorView x1(idSize);
    XorView x2(idSize);

    std::string line;
    while (std::cin) {
        std::getline(std::cin, line);
        if (!line.size()) continue;

        auto items = split(line, ',');
        if (items.size() != 3) throw herr("too few items");

        int mode = std::stoi(items[0]);
        uint64_t created = std::stoull(items[1]);
        auto id = from_hex(items[2]);
        if (id.size() != idSize) throw herr("unexpected id size");

        if (mode == 1) {
            x1.addElem(created, id);
        } else if (mode == 2) {
            x2.addElem(created, id);
        } else if (mode == 3) {
            x1.addElem(created, id);
            x2.addElem(created, id);
        } else {
            throw herr("unexpected mode");
        }
    }

    x1.finalise();
    x2.finalise();

    std::string q = x1.initialQuery();

    uint64_t round = 0;

    while (q.size()) {
        round++;
        std::cerr << "ROUND A " << round << std::endl;
        std::cerr << "CLIENT -> RELAY: " << q.size() << " bytes" << std::endl;
        {
            std::vector<std::string> have, need;
            q = x2.handleQuery(q, have, need);

            // q and have are returned to client
            for (auto &id : have) {
                std::cout << "xor,2,HAVE," << to_hex(id) << "\n";
            }
            for (auto &id : need) {
                std::cout << "xor,2,NEED," << to_hex(id) << "\n";
            }
            std::cerr << "HAVE " << (have.size() * idSize) << " bytes "
                      << "NEED " << (need.size() * idSize) << " bytes " << std::endl;
        }

        if (q.size()) {
            std::cerr << "ROUND B " << round << std::endl;
            std::cerr << "RELAY -> CLIENT: " << q.size() << " bytes" << std::endl;

            std::vector<std::string> have, need;
            q = x1.handleQuery(q, have, need);

            for (auto &id : need) {
                std::cout << "xor,1,NEED," << to_hex(id) << "\n";
            }
            for (auto &id : have) {
                std::cout << "xor,1,HAVE," << to_hex(id) << "\n";
            }
        }
    }

    return 0;
}
