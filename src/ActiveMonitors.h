#pragma once

#include "golpe.h"

#include "Subscription.h"
#include "filters.h"



struct ActiveMonitors : NonCopyable {
  private:
    struct Monitor : NonCopyable {
        Subscription sub;

        Monitor(Subscription &sub_) : sub(std::move(sub_)) {}
    };

    using ConnMonitor = std::map<SubId, Monitor>;
    std::map<uint64_t, ConnMonitor> conns; // connId -> subId -> Monitor

    struct MonitorItem {
        Monitor *mon;
        uint64_t latestEventId;
    };

    using MonitorSet = std::map<NostrFilter*, MonitorItem>; // FIXME: flat_map here
    std::map<std::string, MonitorSet> allIds;
    std::map<std::string, MonitorSet> allAuthors;
    std::map<std::string, MonitorSet> allTags;
    std::map<uint64_t, MonitorSet> allKinds;
    MonitorSet allOthers;


  public:
    void addSub(lmdb::txn &txn, Subscription &&sub, uint64_t currEventId) {
        if (sub.latestEventId != currEventId) throw herr("sub not up to date");

        {
            auto *existing = findMonitor(sub.connId, sub.subId);
            if (existing) removeSub(sub.connId, sub.subId);
        }

        auto res = conns.try_emplace(sub.connId);
        auto &connMonitors = res.first->second;

        auto subId = sub.subId;
        auto *m = &connMonitors.try_emplace(subId, sub).first->second;

        installLookups(m, currEventId);
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

                if (f->doesMatch(ev.flat_nested())) {
                    recipients.emplace_back(item.mon->sub.connId, item.mon->sub.subId);
                    item.mon->sub.latestEventId = ev.primaryKeyId;
                    continue;
                }
            }
        };

        auto processMonitorsPrefix = [&](std::map<std::string, MonitorSet> &m, const std::string &key, std::function<bool(const std::string&)> matches){
            auto it = m.lower_bound(key.substr(0, 1));

            if (it == m.end()) return;

            while (it != m.end() && it->first[0] == key[0]) {
                if (matches(it->first)) processMonitorSet(it->second);
                it = std::next(it);
            }
        };

        auto processMonitorsExact = [&]<typename T>(std::map<T, MonitorSet> &m, const T &key, std::function<bool(const T &)> matches){
            auto it = m.upper_bound(key);

            if (it == m.begin()) return;
            it = std::prev(it);

            while (matches(it->first)) {
                processMonitorSet(it->second);
                if (it == m.begin()) break;
                it = std::prev(it);
            }
        };

        auto *flat = ev.flat_nested();

        {
            auto id = std::string(sv(flat->id()));
            processMonitorsPrefix(allIds, id, static_cast<std::function<bool(const std::string&)>>([&](const std::string &val){
                return id.starts_with(val);
            }));
        }

        {
            auto pubkey = std::string(sv(flat->pubkey()));
            processMonitorsPrefix(allAuthors, pubkey, static_cast<std::function<bool(const std::string&)>>([&](const std::string &val){
                return pubkey.starts_with(val);
            }));
        }

        for (const auto &tag : *flat->tags()) {
            // FIXME: can avoid this allocation:
            auto tagSpec = std::string(1, (char)tag->key()) + std::string(sv(tag->val()));

            processMonitorsExact(allTags, tagSpec, static_cast<std::function<bool(const std::string&)>>([&](const std::string &val){
                return tagSpec == val;
            }));
        }

        {
            auto kind = flat->kind();
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
                        std::string tagSpec = std::string(1, tagName) + filterSet.at(i);
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
                        std::string tagSpec = std::string(1, tagName) + filterSet.at(i);
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
