#include "golpe.h"
#include "ReadRestrictor.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <thread>
#include <vector>

std::atomic<ConfigValues*> currentCfg;

namespace {

void publishConfig(uint64_t ver) {
    auto *c = new ConfigValues(ver);
    c->relay__auth__restrictedReadKinds = "4, 1059";
    currentCfg.store(c); // old configs are leaked, like a live reload
}

// Readers on several threads check that kind 4 stays restricted while the
// config version keeps changing. The restricted kinds never change, so a
// reader must never see kind 4 as unrestricted.
void concurrentReloadKeepsKindsRestricted() {
    publishConfig(1);

    std::atomic<bool> stop = false;
    std::atomic<uint64_t> misses = 0;
    std::vector<std::thread> readers;

    for (int i = 0; i < 4; i++) {
        readers.emplace_back([&]{
            while (!stop) {
                if (!ReadRestrictor::restrictedKinds().contains(4)) misses++;
            }
        });
    }

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    for (uint64_t ver = 2; std::chrono::steady_clock::now() < deadline && misses == 0; ver++) {
        publishConfig(ver);
        std::this_thread::yield();
    }

    stop = true;
    for (auto &t : readers) t.join();

    if (misses) {
        std::cerr << "concurrent reload: kind 4 seen as unrestricted " << misses << " times\n";
        std::exit(EXIT_FAILURE);
    }
}

} // namespace

int main() {
    concurrentReloadKeepsKindsRestricted();
    std::cout << "ReadRestrictor tests passed\n";
    return EXIT_SUCCESS;
}
