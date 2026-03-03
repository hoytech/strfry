#include "Subscription.h"

#include <cstdlib>
#include <exception>
#include <iostream>
#include <string>
#include <string_view>

namespace {

void expectSuccess(std::string_view name, std::string subId) {
    try {
        SubId s(subId);
        if (s.sv() != std::string_view(subId)) {
            std::cerr << name << ": round-trip mismatch\n";
            std::exit(EXIT_FAILURE);
        }
    } catch (const std::exception &e) {
        std::cerr << name << ": expected success but threw: " << e.what() << "\n";
        std::exit(EXIT_FAILURE);
    }
}

void expectFailure(std::string_view name, std::string subId) {
    try {
        SubId s(subId);
        std::cerr << name << ": expected failure but constructed successfully\n";
        std::exit(EXIT_FAILURE);
    } catch (const std::exception &) {
    }
}

} // namespace

int main() {
    expectSuccess("max length", std::string(64, 'a'));
    expectFailure("too long", std::string(65, 'a'));
    expectFailure("empty", std::string());
    expectFailure("control char", std::string(1, '\x1F'));
    expectFailure("quote char", std::string("\""));
    expectFailure("backslash", std::string("\\"));

    std::cout << "SubId tests passed\n";
    return EXIT_SUCCESS;
}
