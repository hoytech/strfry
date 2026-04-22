#pragma once

#include <atomic>
#include <string>
#include <sstream>
#include <map>
#include <mutex>
#include <shared_mutex>

// Simple thread-safe Prometheus metrics implementation
// Supports counters with labels

class PrometheusMetrics {
public:
    // Counter for tracking cumulative values
    class Counter {
    private:
        std::atomic<uint64_t> value{0};
        
    public:
        void inc(uint64_t n = 1) {
            value.fetch_add(n, std::memory_order_relaxed);
        }
        
        uint64_t get() const {
            return value.load(std::memory_order_relaxed);
        }
    };

    // Gauge for tracking current values that can go up and down
    class Gauge {
    private:
        std::atomic<int64_t> value{0};

    public:
        void inc(int64_t n = 1) {
            value.fetch_add(n, std::memory_order_relaxed);
        }

        void dec(int64_t n = 1) {
            value.fetch_sub(n, std::memory_order_relaxed);
        }

        void set(int64_t v) {
            value.store(v, std::memory_order_relaxed);
        }

        int64_t get() const {
            return value.load(std::memory_order_relaxed);
        }
    };

    // Labeled counter - allows multiple counters with different label values
    class LabeledCounter {
    private:
        mutable std::shared_mutex mutex;
        std::map<std::string, Counter> counters;
        
    public:
        void inc(const std::string& label, uint64_t n = 1) {
            // Try read lock first for common case
            {
                std::shared_lock<std::shared_mutex> lock(mutex);
                auto it = counters.find(label);
                if (it != counters.end()) {
                    it->second.inc(n);
                    return;
                }
            }
            
            // Need to create new counter
            std::unique_lock<std::shared_mutex> lock(mutex);
            counters[label].inc(n);
        }
        
        std::map<std::string, uint64_t> getAll() const {
            std::shared_lock<std::shared_mutex> lock(mutex);
            std::map<std::string, uint64_t> result;
            for (const auto& [label, counter] : counters) {
                result[label] = counter.get();
            }
            return result;
        }
    };

    // Singleton instance
    static PrometheusMetrics& getInstance() {
        static PrometheusMetrics instance;
        return instance;
    }

    // Nostr client message counters (messages FROM clients TO relay)
    LabeledCounter nostrClientMessages;
    
    // Nostr relay message counters (messages FROM relay TO clients)
    LabeledCounter nostrRelayMessages;
    
    // Nostr event counters (by kind)
    LabeledCounter nostrEventsByKind;

    // Write path performance metrics
    Counter writtenEventsTotal;
    Counter rejectedEventsTotal;
    Counter dupEventsTotal;
    Counter writeTimeUs;  // total microseconds spent in write transactions
    Gauge lastWriteBatchSize;

    // Connection tracking
    Gauge activeConnections;
    Counter slowClientTerminations;

    // Generate Prometheus text format output
    std::string render() const {
        std::ostringstream out;
        
        // Client messages
        out << "# HELP nostr_client_messages_total Total number of Nostr client messages by verb\n";
        out << "# TYPE nostr_client_messages_total counter\n";
        auto clientMsgs = nostrClientMessages.getAll();
        for (const auto& [verb, count] : clientMsgs) {
            out << "nostr_client_messages_total{verb=\"" << verb << "\"} " << count << "\n";
        }
        
        // Relay messages
        out << "# HELP nostr_relay_messages_total Total number of Nostr relay messages by verb\n";
        out << "# TYPE nostr_relay_messages_total counter\n";
        auto relayMsgs = nostrRelayMessages.getAll();
        for (const auto& [verb, count] : relayMsgs) {
            out << "nostr_relay_messages_total{verb=\"" << verb << "\"} " << count << "\n";
        }
        
        // Events by kind
        out << "# HELP nostr_events_total Total number of Nostr events by kind\n";
        out << "# TYPE nostr_events_total counter\n";
        auto events = nostrEventsByKind.getAll();
        for (const auto& [kind, count] : events) {
            out << "nostr_events_total{kind=\"" << kind << "\"} " << count << "\n";
        }

        // Write path metrics
        out << "# HELP strfry_write_events_total Total events written to DB\n";
        out << "# TYPE strfry_write_events_total counter\n";
        out << "strfry_write_events_total " << writtenEventsTotal.get() << "\n";

        out << "# HELP strfry_write_rejected_total Total events rejected during write\n";
        out << "# TYPE strfry_write_rejected_total counter\n";
        out << "strfry_write_rejected_total " << rejectedEventsTotal.get() << "\n";

        out << "# HELP strfry_write_dups_total Total duplicate events skipped\n";
        out << "# TYPE strfry_write_dups_total counter\n";
        out << "strfry_write_dups_total " << dupEventsTotal.get() << "\n";

        out << "# HELP strfry_write_time_microseconds_total Total time in write transactions\n";
        out << "# TYPE strfry_write_time_microseconds_total counter\n";
        out << "strfry_write_time_microseconds_total " << writeTimeUs.get() << "\n";

        out << "# HELP strfry_write_batch_size Size of last write batch\n";
        out << "# TYPE strfry_write_batch_size gauge\n";
        out << "strfry_write_batch_size " << lastWriteBatchSize.get() << "\n";

        // Connection tracking
        out << "# HELP strfry_connections_current Current number of active WebSocket connections\n";
        out << "# TYPE strfry_connections_current gauge\n";
        out << "strfry_connections_current " << activeConnections.get() << "\n";

        out << "# HELP strfry_slow_client_terminations_total Connections closed for exceeding relay.maxPendingOutboundBytes\n";
        out << "# TYPE strfry_slow_client_terminations_total counter\n";
        out << "strfry_slow_client_terminations_total " << slowClientTerminations.get() << "\n";

        return out.str();
    }

private:
    PrometheusMetrics() = default;
    PrometheusMetrics(const PrometheusMetrics&) = delete;
    PrometheusMetrics& operator=(const PrometheusMetrics&) = delete;
};

// Convenience macros for incrementing metrics
#define PROM_INC_CLIENT_MSG(verb) PrometheusMetrics::getInstance().nostrClientMessages.inc(verb)
#define PROM_INC_RELAY_MSG(verb) PrometheusMetrics::getInstance().nostrRelayMessages.inc(verb)
#define PROM_INC_EVENT_KIND(kind) PrometheusMetrics::getInstance().nostrEventsByKind.inc(kind)
