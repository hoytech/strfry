#pragma once

#include "re2/re2.h"

#include "Bech32Utils.h"
#include "WebUtils.h"
#include "AlgoScanner.h"
#include "WebTemplates.h"
#include "DBQuery.h"




std::string stripUrls(std::string &content);




struct User {
    std::string pubkey;

    std::string npubId;
    std::string username;
    std::optional<tao::json::value> kind0Json;
    std::optional<tao::json::value> kind3Event;

    User(lmdb::txn &txn, Decompressor &decomp, const std::string &pubkey) : pubkey(pubkey) {
        npubId = encodeBech32Simple("npub", pubkey);

        kind0Json = loadKindJson(txn, decomp, 0);

        try {
            if (kind0Json) username = kind0Json->at("name").get_string();
        } catch (std::exception &e) {
        }

        if (username.size() == 0) username = to_hex(pubkey.substr(0,4));
        if (username.size() > 50) username = username.substr(0, 50) + "...";
    }

    std::optional<tao::json::value> loadKindJson(lmdb::txn &txn, Decompressor &decomp, uint64_t kind) {
        std::optional<tao::json::value> output;

        env.generic_foreachFull(txn, env.dbi_Event__pubkeyKind, makeKey_StringUint64Uint64(pubkey, kind, 0), "", [&](std::string_view k, std::string_view v){
            ParsedKey_StringUint64Uint64 parsedKey(k);

            if (parsedKey.s == pubkey && parsedKey.n1 == kind) {
                auto levId = lmdb::from_sv<uint64_t>(v);
                tao::json::value json = tao::json::from_string(getEventJson(txn, decomp, levId));

                try {
                    output = tao::json::from_string(json.at("content").get_string());
                    if (!output->is_object()) output = std::nullopt;
                } catch (std::exception &e) {
                }
            }

            return false;
        });

        return output;
    }

    std::optional<tao::json::value> loadKindEvent(lmdb::txn &txn, Decompressor &decomp, uint64_t kind) {
        std::optional<tao::json::value> output;

        env.generic_foreachFull(txn, env.dbi_Event__pubkeyKind, makeKey_StringUint64Uint64(pubkey, kind, 0), "", [&](std::string_view k, std::string_view v){
            ParsedKey_StringUint64Uint64 parsedKey(k);

            if (parsedKey.s == pubkey && parsedKey.n1 == kind) {
                auto levId = lmdb::from_sv<uint64_t>(v);
                output = tao::json::from_string(getEventJson(txn, decomp, levId));
            }

            return false;
        });

        return output;
    }

    bool kind0Found() const {
        return !!kind0Json;
    }

    std::string getMeta(std::string_view field) const {
        if (!kind0Json) throw herr("can't getMeta because user doesn't have kind 0");
        if (kind0Json->get_object().contains(field) && kind0Json->at(field).is_string()) return kind0Json->at(field).get_string();
        return "";
    }

    void populateContactList(lmdb::txn &txn, Decompressor &decomp) {
        kind3Event = loadKindEvent(txn, decomp, 3);
    }

    std::vector<std::string> getFollowers(lmdb::txn &txn, Decompressor &decomp, const std::string &pubkey) {
        std::vector<std::string> output;
        flat_hash_set<std::string> alreadySeen;

        std::string prefix = "p";
        prefix += pubkey;

        env.generic_foreachFull(txn, env.dbi_Event__tag, prefix, "", [&](std::string_view k, std::string_view v){
            ParsedKey_StringUint64 parsedKey(k);
            if (parsedKey.s != prefix) return false;

            auto levId = lmdb::from_sv<uint64_t>(v);
            auto ev = lookupEventByLevId(txn, levId);

            if (ev.flat_nested()->kind() == 3) {
                auto pubkey = std::string(sv(ev.flat_nested()->pubkey()));

                if (!alreadySeen.contains(pubkey)) {
                    alreadySeen.insert(pubkey);
                    output.emplace_back(std::move(pubkey));
                }
            }

            return true;
        });

        return output;
    }
};

struct UserCache {
    std::unordered_map<std::string, User> cache;

    const User *getUser(lmdb::txn &txn, Decompressor &decomp, const std::string &pubkey) {
        auto u = cache.find(pubkey);
        if (u != cache.end()) return &u->second;

        cache.emplace(pubkey, User(txn, decomp, pubkey));
        return &cache.at(pubkey);
    }
};


struct Event {
    defaultDb::environment::View_Event ev;

    tao::json::value json = tao::json::null;
    std::string parent;
    std::string root;

    uint64_t upVotes = 0;
    uint64_t downVotes = 0;


    Event(defaultDb::environment::View_Event ev) : ev(ev) {
    }

    static Event fromLevId(lmdb::txn &txn, uint64_t levId) {
        return Event(lookupEventByLevId(txn, levId));
    }

    static Event fromId(lmdb::txn &txn, std::string_view id) {
        auto existing = lookupEventById(txn, id);
        if (!existing) throw herr("unable to find event");
        return Event(std::move(*existing));
    }

    static Event fromIdExternal(lmdb::txn &txn, std::string_view id) {
        if (id.starts_with("note1")) {
            return fromId(txn, decodeBech32Simple(id));
        } else {
            return fromId(txn, from_hex(id));
        }
    }


    std::string getId() const {
        return std::string(sv(ev.flat_nested()->id()));
    }

    uint64_t getKind() const {
        return ev.flat_nested()->kind();
    }

    uint64_t getCreatedAt() const {
        return ev.flat_nested()->created_at();
    }

    std::string getPubkey() const {
        return std::string(sv(ev.flat_nested()->pubkey()));
    }

    std::string getNoteId() const {
        return encodeBech32Simple("note", getId());
    }

    std::string getParentNoteId() const {
        return encodeBech32Simple("note", parent);
    }

    std::string getRootNoteId() const {
        return encodeBech32Simple("note", root);
    }

    // FIXME: Use "subject" tag if present?
    // FIXME: Don't truncate UTF-8 mid-sequence
    // FIXME: Don't put ellipsis if truncated text ends in punctuation

    std::string summaryHtml() const {
        std::string content = json.at("content").get_string();
        auto firstUrl = stripUrls(content);

        auto textAbbrev = [](std::string &str, size_t maxLen){
            if (str.size() > maxLen) str = str.substr(0, maxLen-3) + "...";
        };

        textAbbrev(content, 100);
        templarInternal::htmlEscape(content, true);

        if (firstUrl.size()) {
            while (content.size() && isspace(content.back())) content.pop_back();
            if (content.empty()) {
                content = firstUrl;
                textAbbrev(content, 100);
                templarInternal::htmlEscape(content, true);
            }

            return std::string("<a href=\"") + templarInternal::htmlEscape(firstUrl, true) + "\">" + content + "</a>";
        }

        return content;
    }


    void populateJson(lmdb::txn &txn, Decompressor &decomp) {
        if (!json.is_null()) return;

        json = tao::json::from_string(getEventJson(txn, decomp, ev.primaryKeyId));
    }

    void populateRootParent(lmdb::txn &txn, Decompressor &decomp) {
        populateJson(txn, decomp);

        const auto &tags = json.at("tags").get_array();

        // Try to find a e-tags with root/reply types
        for (const auto &t : tags) {
            const auto &tArr = t.get_array();
            if (tArr.at(0) == "e" && tArr.size() >= 4 && tArr.at(3) == "root") {
                root = from_hex(tArr.at(1).get_string());
            } else if (tArr.at(0) == "e" && tArr.size() >= 4 && tArr.at(3) == "reply") {
                parent = from_hex(tArr.at(1).get_string());
            }
        }

        if (!root.size()) {
            // Otherwise, assume first e tag is root

            for (auto it = tags.begin(); it != tags.end(); ++it) {
                const auto &tArr = it->get_array();
                if (tArr.at(0) == "e") {
                    root = from_hex(tArr.at(1).get_string());
                    break;
                }
            }
        }

        if (!parent.size()) {
            // Otherwise, assume last e tag is root

            for (auto it = tags.rbegin(); it != tags.rend(); ++it) {
                const auto &tArr = it->get_array();
                if (tArr.at(0) == "e") {
                    parent = from_hex(tArr.at(1).get_string());
                    break;
                }
            }
        }
    }
};



inline void preprocessContent(lmdb::txn &txn, Decompressor &decomp, const Event &ev, UserCache &userCache, std::string &content) {
    static RE2 matcher(R"((?is)(.*?)(https?://\S+|#\[\d+\]))");

    std::string output;

    std::string_view contentSv(content);
    re2::StringPiece input(contentSv);
    re2::StringPiece prefix, match;

    auto sv = [](re2::StringPiece s){ return std::string_view(s.data(), s.size()); };
    auto appendLink = [&](std::string_view url, std::string_view text){
        output += "<a href=\"";
        output += url;
        output += "\">";
        output += text;
        output += "</a>";
    };

    while (RE2::Consume(&input, matcher, &prefix, &match)) {
        output += sv(prefix);

        if (match.starts_with("http")) {
            appendLink(sv(match), sv(match));
        } else if (match.starts_with("#[")) {
            bool didTransform = false;
            auto offset = std::stoull(std::string(sv(match)).substr(2, match.size() - 3));

            const auto &tags = ev.json.at("tags").get_array();

            try {
                const auto &tag = tags.at(offset).get_array();

                if (tag.at(0) == "p") {
                    const auto *u = userCache.getUser(txn, decomp, from_hex(tag.at(1).get_string()));
                    appendLink(std::string("/u/") + u->npubId, u->username);
                    didTransform = true;
                } else if (tag.at(0) == "e") {
                    appendLink(std::string("/e/") + encodeBech32Simple("note", from_hex(tag.at(1).get_string())), sv(match));
                    didTransform = true;
                }
            } catch(std::exception &e) {
                //LW << "tag parse error: " << e.what();
            }

            if (!didTransform) output += sv(match);
        }
    }

    if (output.size()) {
        output += std::string_view(input.data(), input.size());
        std::swap(output, content);
    }
}


inline std::string stripUrls(std::string &content) {
    static RE2 matcher(R"((?is)(.*?)(https?://\S+))");

    std::string output;
    std::string firstUrl;

    std::string_view contentSv(content);
    re2::StringPiece input(contentSv);
    re2::StringPiece prefix, match;

    auto sv = [](re2::StringPiece s){ return std::string_view(s.data(), s.size()); };

    while (RE2::Consume(&input, matcher, &prefix, &match)) {
        output += sv(prefix);

        if (firstUrl.empty()) {
            firstUrl = std::string(sv(match));
        }
    }

    output += std::string_view(input.data(), input.size());

    std::swap(output, content);
    return firstUrl;
}


struct ReplyCtx {
    uint64_t timestamp;
    TemplarResult rendered;
};

struct RenderedEventCtx {
    std::string content;
    std::string timestamp;
    const Event *ev = nullptr;
    const User *user = nullptr;
    bool isFullThreadLoaded = false;
    bool eventPresent = true;
    bool abbrev = false;
    bool highlight = false;
    bool showActions = true;
    std::vector<ReplyCtx> replies;
};


struct EventThread {
    std::string rootEventId;
    bool isRootEventThreadRoot;
    flat_hash_map<std::string, Event> eventCache;

    flat_hash_map<std::string, flat_hash_set<std::string>> children; // parentEventId -> childEventIds
    std::string pubkeyHighlight;
    bool isFullThreadLoaded = false;


    // Load all events under an eventId

    EventThread(std::string rootEventId, bool isRootEventThreadRoot, flat_hash_map<std::string, Event> &&eventCache)
        : rootEventId(rootEventId), isRootEventThreadRoot(isRootEventThreadRoot), eventCache(eventCache) {}

    EventThread(lmdb::txn &txn, Decompressor &decomp, std::string_view id_) : rootEventId(std::string(id_)) {
        try {
            eventCache.emplace(rootEventId, Event::fromId(txn, rootEventId));
        } catch (std::exception &e) {
            return;
        }


        eventCache.at(rootEventId).populateRootParent(txn, decomp);
        isRootEventThreadRoot = eventCache.at(rootEventId).root.empty();
        isFullThreadLoaded = true;


        std::vector<std::string> pendingQueue;
        pendingQueue.emplace_back(rootEventId);

        while (pendingQueue.size()) {
            auto currId = std::move(pendingQueue.back());
            pendingQueue.pop_back();

            std::string prefix = "e";
            prefix += currId;

            env.generic_foreachFull(txn, env.dbi_Event__tag, prefix, "", [&](std::string_view k, std::string_view v){
                ParsedKey_StringUint64 parsedKey(k);
                if (parsedKey.s != prefix) return false;

                auto levId = lmdb::from_sv<uint64_t>(v);
                Event e = Event::fromLevId(txn, levId);
                std::string childEventId = e.getId();

                if (eventCache.contains(childEventId)) return true;

                eventCache.emplace(childEventId, std::move(e));
                if (!isRootEventThreadRoot) pendingQueue.emplace_back(childEventId);

                return true;
            });
        }

        for (auto &[id, e] : eventCache) {
            e.populateRootParent(txn, decomp);

            auto kind = e.getKind();

            if (e.parent.size()) {
                if (kind == 1) {
                    if (!children.contains(e.parent)) children.emplace(std::piecewise_construct, std::make_tuple(e.parent), std::make_tuple());
                    children.at(e.parent).insert(id);
                } else if (kind == 7) {
                    auto p = eventCache.find(e.parent);
                    if (p != eventCache.end()) {
                        auto &parent = p->second;

                        if (e.json.at("content").get_string() == "-") {
                            parent.downVotes++;
                        } else {
                            parent.upVotes++;
                        }
                    }
                }
            }
        }
    }


    TemplarResult render(lmdb::txn &txn, Decompressor &decomp, UserCache &userCache, std::optional<std::string> focusOnPubkey = std::nullopt) {
        auto now = hoytech::curr_time_s();
        flat_hash_set<uint64_t> processedLevIds;


        std::function<TemplarResult(const std::string &)> process = [&](const std::string &id){
            RenderedEventCtx ctx;

            auto p = eventCache.find(id);
            if (p != eventCache.end()) {
                const auto &elem = p->second;
                processedLevIds.insert(elem.ev.primaryKeyId);

                auto pubkey = elem.getPubkey();

                ctx.timestamp = renderTimestamp(now, elem.getCreatedAt());
                ctx.user = userCache.getUser(txn, decomp, elem.getPubkey());
                ctx.isFullThreadLoaded = isFullThreadLoaded;
                ctx.eventPresent = true;
                ctx.highlight = (pubkey == pubkeyHighlight);

                ctx.abbrev = focusOnPubkey && *focusOnPubkey != pubkey;
                if (ctx.abbrev) {
                    ctx.content = elem.summaryHtml();
                } else {
                    ctx.content = templarInternal::htmlEscape(elem.json.at("content").get_string(), false);
                    preprocessContent(txn, decomp, elem, userCache, ctx.content);
                }

                ctx.ev = &elem;
            } else {
                ctx.eventPresent = false;
            }

            if (children.contains(id)) {
                for (const auto &childId : children.at(id)) {
                    auto timestamp = MAX_U64;
                    auto p = eventCache.find(childId);
                    if (p != eventCache.end()) timestamp = p->second.getCreatedAt();

                    ctx.replies.emplace_back(timestamp, process(childId));
                }

                std::sort(ctx.replies.begin(), ctx.replies.end(), [](auto &a, auto &b){ return a.timestamp < b.timestamp; });
            }

            return tmpl::event::event(ctx);
        };


        struct {
            TemplarResult foundEvents;
            std::vector<ReplyCtx> orphanNodes;
        } ctx;

        ctx.foundEvents = process(rootEventId);

        for (auto &[id, e] : eventCache) {
            if (processedLevIds.contains(e.ev.primaryKeyId)) continue;
            if (e.getKind() != 1) continue;

            ctx.orphanNodes.emplace_back(e.getCreatedAt(), process(id));
        }

        std::sort(ctx.orphanNodes.begin(), ctx.orphanNodes.end(), [](auto &a, auto &b){ return a.timestamp < b.timestamp; });

        return tmpl::events(ctx);
    }
};



struct UserEvents {
    User u;

    struct EventCluster {
        std::string rootEventId;
        flat_hash_map<std::string, Event> eventCache; // eventId (non-root) -> Event
        bool isRootEventFromUser = false;
        bool isRootPresent = false;
        uint64_t rootEventTimestamp = 0;

        EventCluster(std::string rootEventId) : rootEventId(rootEventId) {}
    };

    std::vector<EventCluster> eventClusterArr;

    UserEvents(lmdb::txn &txn, Decompressor &decomp, const std::string &pubkey) : u(txn, decomp, pubkey) {
        flat_hash_map<std::string, EventCluster> eventClusters; // eventId (root) -> EventCluster

        env.generic_foreachFull(txn, env.dbi_Event__pubkeyKind, makeKey_StringUint64Uint64(pubkey, 1, MAX_U64), "", [&](std::string_view k, std::string_view v){
            ParsedKey_StringUint64Uint64 parsedKey(k);
            if (parsedKey.s != pubkey || parsedKey.n1 != 1) return false;

            Event ev = Event::fromLevId(txn, lmdb::from_sv<uint64_t>(v));
            ev.populateRootParent(txn, decomp);
            auto id = ev.getId();

            auto installRoot = [&](std::string rootId, Event &&rootEvent){
                rootEvent.populateRootParent(txn, decomp);

                eventClusters.emplace(rootId, rootId);
                auto &cluster = eventClusters.at(rootId);

                cluster.isRootPresent = true;
                cluster.isRootEventFromUser = rootEvent.getPubkey() == u.pubkey;
                cluster.rootEventTimestamp = rootEvent.getCreatedAt();
                cluster.eventCache.emplace(rootId, std::move(rootEvent));
            };

            if (ev.root.size()) {
                // Event is not root

                if (!eventClusters.contains(ev.root)) {
                    try {
                        installRoot(ev.root, Event::fromId(txn, ev.root));
                    } catch (std::exception &e) {
                        // no root event
                        eventClusters.emplace(ev.root, ev.root);
                        auto &cluster = eventClusters.at(ev.root);

                        cluster.isRootPresent = true;
                    }
                }

                eventClusters.at(ev.root).eventCache.emplace(id, std::move(ev));
            } else {
                // Event is root

                if (!eventClusters.contains(ev.root)) {
                    installRoot(id, std::move(ev));
                }
            }

            return true;
        }, true);

        for (auto &[k, v] : eventClusters) {
            eventClusterArr.emplace_back(std::move(v));
        }

        std::sort(eventClusterArr.begin(), eventClusterArr.end(), [](auto &a, auto &b){ return b.rootEventTimestamp < a.rootEventTimestamp; });
    }

    TemplarResult render(lmdb::txn &txn, Decompressor &decomp) {
        std::vector<TemplarResult> renderedThreads;
        UserCache userCache;

        for (auto &cluster : eventClusterArr) {
            EventThread eventThread(cluster.rootEventId, cluster.isRootEventFromUser, std::move(cluster.eventCache));
            eventThread.pubkeyHighlight = u.pubkey;
            renderedThreads.emplace_back(eventThread.render(txn, decomp, userCache, u.pubkey));
        }

        struct {
            std::vector<TemplarResult> &renderedThreads;
            User &u;
        } ctx = {
            renderedThreads,
            u,
        };

        return tmpl::user::comments(ctx);
    }
};



struct CommunitySpec {
    bool valid = true;
    tao::json::value raw;

    std::string name;
    std::string desc;
    std::string algo;

    std::string adminNpub;
    std::string adminUsername;
    std::string adminTopic;
};

inline CommunitySpec lookupCommunitySpec(lmdb::txn &txn, Decompressor &decomp, UserCache &userCache, std::string_view algoDescriptor) {
    CommunitySpec spec;

    size_t pos = algoDescriptor.find("/");
    if (pos == std::string_view::npos) throw herr("bad algo descriptor");
    spec.adminNpub = std::string(algoDescriptor.substr(0, pos));
    std::string authorPubkey = decodeBech32Simple(spec.adminNpub);
    spec.adminTopic = algoDescriptor.substr(pos + 1);

    tao::json::value filter = tao::json::value({
        { "authors", tao::json::value::array({ to_hex(authorPubkey) }) },
        { "kinds", tao::json::value::array({ uint64_t(33700) }) },
        { "#d", tao::json::value::array({ spec.adminTopic }) },
    });

    bool found = false;

    foreachByFilter(txn, filter, [&](uint64_t levId){
        tao::json::value ev = tao::json::from_string(getEventJson(txn, decomp, levId));
        spec.algo = ev.at("content").get_string();
        found = true;
        return false;
    });

    if (!found) throw herr("unable to find algo");

    spec.raw = tao::json::from_string(spec.algo);

    spec.name = spec.raw.at("name").get_string();
    spec.desc = spec.raw.at("desc").get_string();
    spec.algo = spec.raw.at("algo").get_string();

    auto *user = userCache.getUser(txn, decomp, authorPubkey);
    spec.adminUsername = user->username;

    return spec;
}
