#pragma once

#include <atomic>
#include <chrono>
#include <deque>
#include <thread>

#include <hoytech/protected_queue.h>

template <typename M>
struct ThreadPool {

    struct Thread {
        uint64_t id = 0;
        std::thread thread;
        hoytech::protected_queue<M> inbox;

        // Approximate number of messages queued for this thread. Used to
        // apply back-pressure instead of letting a flood grow the queue
        // without bound.
        std::atomic<uint64_t> pending{0};

        std::deque<M> popAll() {
            auto msgs = inbox.pop_all();
            pending.fetch_sub(msgs.size(), std::memory_order_acq_rel);
            return msgs;
        }

        std::deque<M> popAllNoWait() {
            auto msgs = inbox.pop_all_no_wait();
            pending.fetch_sub(msgs.size(), std::memory_order_acq_rel);
            return msgs;
        }
    };

    // Maximum messages queued per worker thread. dispatch() blocks once this
    // is reached, which propagates back-pressure through the websocket reader
    // instead of allowing unbounded memory growth.
    static constexpr uint64_t MAX_PENDING_MESSAGES = 4096;

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
        waitForCapacity(t, 1);
        t.pending.fetch_add(1, std::memory_order_acq_rel);
        t.inbox.push_move(std::move(m));
    }

    // Non-blocking variant for the websocket reader -> ingester edge. The
    // websocket thread also drains its own inbox, so blocking it here could
    // deadlock the pipeline when the ingester and writer queues are full.
    bool tryDispatch(uint64_t key, M &&m) {
        if (pool.empty()) {
            throw herr("ThreadPool not initialized");
        }

        auto &t = pool[key % pool.size()];
        if (t.pending.load(std::memory_order_acquire) >= MAX_PENDING_MESSAGES) {
            return false;
        }

        t.pending.fetch_add(1, std::memory_order_acq_rel);
        t.inbox.push_move(std::move(m));
        return true;
    }

    // Control messages (connection teardown) must not be dropped. They are
    // small and bounded by the number of live connections, so allow them to
    // temporarily exceed the soft cap.
    void dispatchUnbounded(uint64_t key, M &&m) {
        if (pool.empty()) {
            throw herr("ThreadPool not initialized");
        }

        auto &t = pool[key % pool.size()];
        t.pending.fetch_add(1, std::memory_order_acq_rel);
        t.inbox.push_move(std::move(m));
    }

    void dispatchMulti(uint64_t key, std::vector<M> &m) {
        if (pool.empty()) {
            throw herr("ThreadPool not initialized");
        }

        auto &t = pool[key % pool.size()];
        waitForCapacity(t, m.size());
        t.pending.fetch_add(m.size(), std::memory_order_acq_rel);
        t.inbox.push_move_all(m);
    }

    void dispatchToAll(const std::function<M()> &cb) {
        if (pool.empty()) {
            throw herr("ThreadPool not initialized");
        }

        for (auto &t : pool) {
            waitForCapacity(t, 1);
            t.pending.fetch_add(1, std::memory_order_acq_rel);
            t.inbox.push_move(cb());
        }
    }

    static void waitForCapacity(Thread &t, uint64_t count) {
        while (true) {
            auto pending = t.pending.load(std::memory_order_acquire);
            // Always allow a batch to make progress if the queue is empty,
            // even when the batch itself is larger than the cap.
            if (pending == 0 || pending + count <= MAX_PENDING_MESSAGES) return;
            std::this_thread::sleep_for(std::chrono::microseconds(100));
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