#include <iostream>

#include "golpe.h"
#include "xor.h"


int main() {
    XorView x1(16);
    x1.addElem(1000, std::string(16, 'a'));
    x1.addElem(2000, std::string(16, 'b'));
    x1.finalise();

    XorView x2(16);
    x2.addElem(2000, std::string(16, 'b'));
    x2.addElem(3000, std::string(16, 'c'));
    x2.finalise();

    {
        auto q = x1.initialQuery();
        std::cout << to_hex(q) << std::endl;

        std::vector<std::string> have, need;
        auto q2 = x2.handleQuery(q, have, need);

        for (auto &s : have) std::cout << "HAVE: " << to_hex(s) << std::endl;
        for (auto &s : need) std::cout << "NEED: " << to_hex(s) << std::endl;
        std::cout << to_hex(q2) << std::endl;
    }

    return 0;
}
