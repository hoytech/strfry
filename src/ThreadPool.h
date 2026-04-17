#pragma once

#include <hoytech/protected_queue.h>

template <typename M>
struct ThreadPool {

    struct Thread {
        uint64_t id = 0;
        std::thread thread;
        hoytech::protected_queue<M> inbox;
    };

    std::deque<Thread> pool;

    ~ThreadPool() {
        join();
    }

    size_t size() const {
        return pool.size();
    }

    bool empty() const {
        return pool.empty();
    }

    void init(const std::string& name,
              uint64_t numThreads,
              const std::function<void(Thread &)> &cb)
    {
        if (numThreads == 0) {
            throw herr("must have more than 0 threads");
        }

        if (!pool.empty()) {
            throw herr("ThreadPool already initialized");
        }

        for (uint64_t i = 0; i < numThreads; i++) {
            std::string myName = name;
            if (numThreads != 1) {
                myName += ' ';
                myName += std::to_string(i);
            }

            pool.emplace_back();
            Thread* t = &pool.back();

            t->id = i;

            t->thread = std::thread([t, cb, myName]() {
                setThreadName(myName.c_str());
                cb(*t);
            });
        }
    }

    void dispatch(uint64_t key, M &&m) {
        if (pool.empty()) {
            throw herr("ThreadPool not initialized");
        }

        auto &t = pool[key % pool.size()];
        t.inbox.push_move(std::move(m));
    }

    void dispatchMulti(uint64_t key, std::vector<M> &m) {
        if (pool.empty()) {
            throw herr("ThreadPool not initialized");
        }

        auto &t = pool[key % pool.size()];
        t.inbox.push_move_all(m);
    }

    void dispatchToAll(const std::function<M()> &cb) {
        if (pool.empty()) {
            throw herr("ThreadPool not initialized");
        }

        for (auto &t : pool) {
            t.inbox.push_move(cb());
        }
    }

    void join() {
        for (auto &t : pool) {
            if (t.thread.joinable()) {
                t.thread.join();
            }
        }
        pool.clear();
    }
};