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
