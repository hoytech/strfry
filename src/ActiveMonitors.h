#pragma once

#include <unordered_map>

#include "golpe.h"

#include "Subscription.h"
#include "filters.h"



struct ActiveMonitors : NonCopyable {
  private:
    struct Monitor : NonCopyable {
        Subscription sub;

        Monitor(Subscription &sub_) : sub(std::move(sub_)) {}
        Monitor(const Monitor&) = delete; // pointers to filters inside sub must be stable because they are stored in MonitorSets
    };

    using ConnMonitor = std::unordered_map<SubId, Monitor>;
    flat_hash_map<uint64_t, ConnMonitor> conns; // connId -> subId -> Monitor

    struct MonitorItem {
        Monitor *mon;
        uint64_t latestEventId;
    };

    using MonitorSet = flat_hash_map<NostrFilter*, MonitorItem>;
    btree_map<std::string, MonitorSet> allIds;
    btree_map<std::string, MonitorSet> allAuthors;
    btree_map<std::string, MonitorSet> allTags;
    btree_map<uint64_t, MonitorSet> allKinds;
    MonitorSet allOthers;

    std::string tagSpecBuf = std::string(256, '\0');
    const std::string &getTagSpec(uint8_t k, std::string_view val) {
        tagSpecBuf.clear();
        tagSpecBuf += (char)k;
        tagSpecBuf += val;
        return tagSpecBuf;
    }


  public:
    bool addSub(lmdb::txn &txn, Subscription &&sub, uint64_t currEventId) {
        if (sub.latestEventId != currEventId) throw herr("sub not up to date");

        {
            auto *existing = findMonitor(sub.connId, sub.subId);
            if (existing) removeSub(sub.connId, sub.subId);
        }

        auto res = conns.try_emplace(sub.connId);
        auto &connMonitors = res.first->second;

        if (connMonitors.size() >= cfg().relay__maxSubsPerConnection) {
            return false;
        }

        auto subId = sub.subId;
        auto *m = &connMonitors.try_emplace(subId, sub).first->second;

        installLookups(m, currEventId);
        return true;
    }

    void removeSub(uint64_t connId, const SubId &subId) {
        auto *monitor = findMonitor(connId, subId);
        if (!monitor) return;

        uninstallLookups(monitor);

        conns[connId].erase(subId);
        if (conns[connId].empty()) conns.erase(connId);
    }

    void closeConn(uint64_t connId) {
        auto f1 = conns.find(connId);
        if (f1 == conns.end()) return;

        for (auto &[k, v] : f1->second) uninstallLookups(&v);

        conns.erase(connId);
    }

    void process(lmdb::txn &txn, defaultDb::environment::View_Event &ev, std::function<void(RecipientList &&, uint64_t)> cb) {
        RecipientList recipients;

        auto processMonitorSet = [&](MonitorSet &ms){
            for (auto &[f, item] : ms) {
                if (item.latestEventId >= ev.primaryKeyId || item.mon->sub.latestEventId >= ev.primaryKeyId) continue;
                item.latestEventId = ev.primaryKeyId;

                if (f->doesMatch(PackedEventView(ev.packed()))) {
                    recipients.emplace_back(item.mon->sub.connId, item.mon->sub.subId);
                    item.mon->sub.latestEventId = ev.primaryKeyId;
                    continue;
                }
            }
        };

        auto processMonitorsExact = [&]<typename T>(btree_map<T, MonitorSet> &m, const T &key, std::function<bool(const T &)> matches){
            auto it = m.upper_bound(key);

            if (it == m.begin()) return;
            it = std::prev(it);

            while (matches(it->first)) {
                processMonitorSet(it->second);
                if (it == m.begin()) break;
                it = std::prev(it);
            }
        };

        auto packed = PackedEventView(ev.packed());

        {
            auto id = std::string(packed.id());
            processMonitorsExact(allIds, id, static_cast<std::function<bool(const std::string&)>>([&](const std::string &val){
                return id == val;
            }));
        }

        {
            auto pubkey = std::string(packed.pubkey());
            processMonitorsExact(allAuthors, pubkey, static_cast<std::function<bool(const std::string&)>>([&](const std::string &val){
                return pubkey == val;
            }));
        }

        packed.foreachTag([&](char tagName, std::string_view tagVal){
            auto &tagSpec = getTagSpec(tagName, tagVal);
            processMonitorsExact(allTags, tagSpec, static_cast<std::function<bool(const std::string&)>>([&](const std::string &val){
                return tagSpec == val;
            }));
            return true;
        });

        {
            auto kind = packed.kind();
            processMonitorsExact(allKinds, kind, static_cast<std::function<bool(const uint64_t&)>>([&](const uint64_t &val){
                return kind == val;
            }));
        }

        processMonitorSet(allOthers);

        if (recipients.size()) {
            cb(std::move(recipients), ev.primaryKeyId);
        }
    }


  private:
    Monitor *findMonitor(uint64_t connId, const SubId &subId) {
        auto f1 = conns.find(connId);
        if (f1 == conns.end()) return nullptr;

        auto f2 = f1->second.find(subId);
        if (f2 == f1->second.end()) return nullptr;

        return &f2->second;
    }

    void installLookups(Monitor *m, uint64_t currEventId) {
        for (auto &f : m->sub.filterGroup.filters) {
            if (f.ids) {
                for (size_t i = 0; i < f.ids->size(); i++) {
                    auto res = allIds.try_emplace(f.ids->at(i));
                    res.first->second.try_emplace(&f, MonitorItem{m, currEventId});
                }
            } else if (f.authors) {
                for (size_t i = 0; i < f.authors->size(); i++) {
                    auto res = allAuthors.try_emplace(f.authors->at(i));
                    res.first->second.try_emplace(&f, MonitorItem{m, currEventId});
                }
            } else if (f.tags.size()) {
                for (const auto &[tagName, filterSet] : f.tags) {
                    for (size_t i = 0; i < filterSet.size(); i++) {
                        auto &tagSpec = getTagSpec(tagName, filterSet.at(i));
                        auto res = allTags.try_emplace(tagSpec);
                        res.first->second.try_emplace(&f, MonitorItem{m, currEventId});
                    }
                }
            } else if (f.kinds) {
                for (size_t i = 0; i < f.kinds->size(); i++) {
                    auto res = allKinds.try_emplace(f.kinds->at(i));
                    res.first->second.try_emplace(&f, MonitorItem{m, currEventId});
                }
            } else {
                allOthers.try_emplace(&f, MonitorItem{m, currEventId});
            }
        }
    }

    void uninstallLookups(Monitor *m) {
        for (auto &f : m->sub.filterGroup.filters) {
            if (f.ids) {
                for (size_t i = 0; i < f.ids->size(); i++) {
                    auto &monSet = allIds.at(f.ids->at(i));
                    monSet.erase(&f);
                    if (monSet.empty()) allIds.erase(f.ids->at(i));
                }
            } else if (f.authors) {
                for (size_t i = 0; i < f.authors->size(); i++) {
                    auto &monSet = allAuthors.at(f.authors->at(i));
                    monSet.erase(&f);
                    if (monSet.empty()) allAuthors.erase(f.authors->at(i));
                }
            } else if (f.tags.size()) {
                for (const auto &[tagName, filterSet] : f.tags) {
                    for (size_t i = 0; i < filterSet.size(); i++) {
                        auto &tagSpec = getTagSpec(tagName, filterSet.at(i));
                        auto &monSet = allTags.at(tagSpec);
                        monSet.erase(&f);
                        if (monSet.empty()) allTags.erase(tagSpec);
                    }
                }
            } else if (f.kinds) {
                for (size_t i = 0; i < f.kinds->size(); i++) {
                    auto &monSet = allKinds.at(f.kinds->at(i));
                    monSet.erase(&f);
                    if (monSet.empty()) allKinds.erase(f.kinds->at(i));
                }
            } else {
                allOthers.erase(&f);
            }
        }
    }
};
