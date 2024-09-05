#pragma once

#include <hoytech/protected_queue.h>


template <typename M>
struct ThreadPool {
    uint64_t numThreads;

    struct Thread {
        uint64_t id;
        std::thread thread;
        hoytech::protected_queue<M> inbox;
    };

    std::deque<Thread> pool;

    ~ThreadPool() {
        join();
    }

    void init(std::string name, uint64_t numThreads_, const std::function<void(Thread &t)> &cb) {
        if (numThreads_ == 0) throw herr("must have more than 0 threads");

        numThreads = numThreads_;

        for (size_t i = 0; i < numThreads; i++) {
            std::string myName = name;
            if (numThreads != 1) myName += std::string(" ") + std::to_string(i);

            pool.emplace_back();
            auto &t = pool.back();

            t.id = i;
            t.thread = std::thread([&t, cb, myName]() {
                setThreadName(myName.c_str());
                cb(t);
            });
        }
    }

    void dispatch(uint64_t key, M &&m) {
        uint64_t who = key % numThreads;
        pool[who].inbox.push_move(std::move(m));
    }

    void dispatchMulti(uint64_t key, std::vector<M> &m) {
        uint64_t who = key % numThreads;
        pool[who].inbox.push_move_all(m);
    }

    void dispatchToAll(const std::function<M()> &cb) {
        for (size_t i = 0; i < numThreads; i++) pool[i].inbox.push_move(cb());
    }

    void join() {
        for (size_t i = 0; i < numThreads; i++) {
            pool[i].thread.join();
        }
    }
};
