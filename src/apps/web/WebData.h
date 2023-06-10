#pragma once

#include "re2/re2.h"

#include "hoytech/parser.h"
#include "hoytech/truncate.h"

#include "Bech32Utils.h"
#include "WebUtils.h"
#include "WebTemplates.h"
#include "DBQuery.h"




std::string stripUrls(std::string &content);


inline void preprocessMetaFieldContent(std::string &content) {
    static RE2 matcher(R"((?is)(.*?)(https?://\S+))");

    std::string output;

    re2::StringPiece input(content.data(), content.size());
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
        }
    }

    if (output.size()) {
        output += std::string_view(input.data(), input.size());
        std::swap(output, content);
    }
}


inline bool isEventPostedByOddbean(const PackedEventView &packed) {
    bool isOddbean = false;

    // Assumes C-tag is always first in array
    packed.foreachTag([&](char tagName, std::string_view tagVal){
        if (tagName == 'C' && tagVal == "oddbean") isOddbean = true;
        return false;
    });

    return isOddbean;
}


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
        else hoytech::truncateInPlace(username, 50);
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
        std::string output;

        if (!kind0Json) throw herr("can't getMeta because user doesn't have kind 0");
        if (kind0Json->get_object().contains(field) && kind0Json->at(field).is_string()) output = kind0Json->at(field).get_string();

        output = templarInternal::htmlEscape(output, false);
        preprocessMetaFieldContent(output);

        return output;
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
            PackedEventView packed(ev.buf);

            if (packed.kind() == 3) {
                auto pubkey = std::string(packed.pubkey());

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
        PackedEventView packed(ev.buf);
        return std::string(packed.id());
    }

    uint64_t getKind() const {
        PackedEventView packed(ev.buf);
        return packed.kind();
    }

    uint64_t getCreatedAt() const {
        PackedEventView packed(ev.buf);
        return packed.created_at();
    }

    std::string getPubkey() const {
        PackedEventView packed(ev.buf);
        return std::string(packed.pubkey());
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

    std::string summaryHtml(std::string_view noteIdAltLink = "", bool rssMode = false) const {
        std::string content = json.at("content").get_string();
        auto firstUrl = stripUrls(content);

        auto textAbbrev = [](std::string &str, size_t maxLen){
            hoytech::truncateInPlace(str, maxLen);
        };

        textAbbrev(content, 100);
        content = templarInternal::htmlEscape(content, true);

        auto fillEmptyContent = [&]{
            if (std::all_of(content.begin(), content.end(), [](unsigned char c) { return std::isspace(c); })) content = "[no content]";
        };

        if (firstUrl.size()) {
            while (content.size() && isspace(content.back())) content.pop_back();
            if (content.empty()) {
                content = firstUrl;
                textAbbrev(content, 100);
                content = templarInternal::htmlEscape(content, true);
            }

            fillEmptyContent();

            if (rssMode) return content;
            else return std::string("<a href=\"") + templarInternal::htmlEscape(firstUrl, true) + "\">" + content + "</a>";
        }

        fillEmptyContent();

        if (!rssMode && noteIdAltLink.size() != 0) {
            std::string link = "<a href=\"/e/";
            link += templarInternal::htmlEscape(noteIdAltLink, true);
            link += "\">";
            link += content;
            link += "</a>";
            std::swap(content, link);
        }

        return content;
    }

    std::vector<std::string> getTopics() const {
        std::vector<std::string> output;
        PackedEventView packed(ev.buf);

        packed.foreachTag([&](char tagName, std::string_view tagVal){
            if (tagName == 't') {
                output.emplace_back(tagVal);
            }

            return true;
        });

        return output;
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


inline std::string decodeNip19Tag0(std::string_view v) {
    hoytech::Parser parser(v);

    while (!parser.isEof()) {
        auto tag = parser.getByte();
        auto len = parser.getByte();
        auto val = parser.getBytes(len);
        if (tag == 0) {
            if (val.size() != 32) throw herr("invalid length for tag 0");
            return std::string(val);
        }
    }

    throw herr("couldn't find tag 0");
}


void trimInPlace(std::string &s) {
    if (s.empty()) return;

    auto start = std::find_if_not(s.begin(), s.end(), [](unsigned char c) { return std::isspace(c); });
    auto end = std::find_if_not(s.rbegin(), s.rend(), [](unsigned char c) { return std::isspace(c); }).base();

    if (start >= end) {
        s.clear();
        return;
    }

    s.erase(end, s.end());
    s.erase(s.begin(), start);
}


inline void preprocessEventContent(lmdb::txn &txn, Decompressor &decomp, const Event &ev, UserCache &userCache, std::string &content) {
    static RE2 matcher(R"((?is)(.*?)(\bhttps?://\S+|\bnostr:(?:note|npub|nevent|nprofile)1\w+|#\[\d+\]|#\w+))");

    std::string output;

    re2::StringPiece input(content.data(), content.size());
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
        } else if (match.starts_with("nostr:note1")) {
            std::string path = "/e/";
            path += sv(match).substr(6);
            appendLink(path, sv(match));
        } else if (match.starts_with("nostr:npub1")) {
            bool didTransform = false;

            try {
                const auto *u = userCache.getUser(txn, decomp, decodeBech32Simple(sv(match).substr(6)));
                appendLink(std::string("/u/") + u->npubId, std::string("@") + u->username);
                didTransform = true;
            } catch(std::exception &e) {
                //LW << "parse error: " << e.what();
            }

            if (!didTransform) output += sv(match);
        } else if (match.starts_with("nostr:nevent1")) {
            bool didTransform = false;

            try {
                std::string decoded = decodeBech32(sv(match.substr(6)));
                auto note = encodeBech32Simple("note", decodeNip19Tag0(decoded));
                appendLink(std::string("/e/") + note, std::string(sv(match).substr(0, 24)) + "...");
                didTransform = true;
            } catch(std::exception &e) {
                //LW << "parse error: " << e.what();
            }

            if (!didTransform) output += sv(match);
        } else if (match.starts_with("nostr:nprofile1")) {
            bool didTransform = false;

            try {
                std::string decoded = decodeBech32(sv(match.substr(6)));
                auto pubkey = decodeNip19Tag0(decoded);

                const auto *u = userCache.getUser(txn, decomp, pubkey);
                appendLink(std::string("/u/") + u->npubId, std::string("@") + u->username);

                didTransform = true;
            } catch(std::exception &e) {
                //LW << "parse error: " << e.what();
            }

            if (!didTransform) output += sv(match);
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
                //LW << "parse error: " << e.what();
            }

            if (!didTransform) output += sv(match);
        } else if (match.starts_with("#")) {
            std::string url = "/t/";
            url += sv(match).substr(1);
            appendLink(url, sv(match));
        }
    }

    if (output.size()) {
        output += std::string_view(input.data(), input.size());
        std::swap(output, content);
    }

    trimInPlace(content);
}


inline std::string stripUrls(std::string &content) {
    static RE2 matcher(R"((?is)(.*?)(\bhttps?://\S+))");

    std::string output;
    std::string firstUrl;

    re2::StringPiece input(content.data(), content.size());
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
    bool isOddbean = false;
    std::vector<ReplyCtx> replies;
    std::vector<std::string> topics;
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
                    preprocessEventContent(txn, decomp, elem, userCache, ctx.content);
                }

                ctx.isOddbean = isEventPostedByOddbean(PackedEventView(elem.ev.buf));

                ctx.ev = &elem;

                ctx.topics = elem.getTopics();
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
    uint64_t totalEvents = 0;
    std::optional<uint64_t> timestampCutoff;
    std::optional<uint64_t> nextResumeTime;

    UserEvents(lmdb::txn &txn, Decompressor &decomp, const std::string &pubkey, uint64_t resumeTime) : u(txn, decomp, pubkey) {
        flat_hash_map<std::string, EventCluster> eventClusters; // eventId (root) -> EventCluster

        env.generic_foreachFull(txn, env.dbi_Event__pubkeyKind, makeKey_StringUint64Uint64(pubkey, 1, resumeTime), "", [&](std::string_view k, std::string_view v){
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
                totalEvents++;
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
                totalEvents++;
            } else {
                // Event is root

                if (!eventClusters.contains(ev.root)) {
                    installRoot(id, std::move(ev));
                }
            }

            if (timestampCutoff) {
                if (*timestampCutoff != parsedKey.n2) {
                    nextResumeTime = *timestampCutoff - 1;
                    return false;
                }
            } else if (totalEvents > 100) {
                timestampCutoff = parsedKey.n2;
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
            std::optional<uint64_t> nextResumeTime;
        } ctx = {
            renderedThreads,
            u,
            nextResumeTime,
        };

        return tmpl::user::comments(ctx);
    }
};




struct TopicEventScores {
    defaultDb::environment::View_Event eventView;
    uint64_t timestamp;

    uint64_t comments = 0;
    uint64_t upVotes = 0;
    uint64_t flags = 0;
    double score = 0.0;
    bool isOddbean = false;

    TopicEventScores(lmdb::txn &txn, const defaultDb::environment::View_Event &eventView, const PackedEventView &packed) : eventView(eventView) {
        timestamp = packed.created_at();

        std::string prefix = "e";
        prefix += packed.id();

        env.generic_foreachFull(txn, env.dbi_Event__tag, makeKey_StringUint64(prefix, MAX_U64), "", [&](std::string_view k, std::string_view v){
            ParsedKey_StringUint64 parsedKey(k);
            if (parsedKey.s != prefix) return false;

            PackedEventView packed2(lookupEventByLevId(txn, lmdb::from_sv<uint64_t>(v)).buf);

            auto kind = packed2.kind();

            if (kind == 1) comments++;
            else if (kind == 7) upVotes++;
            else if (kind == 1984) flags++;

            return true;
        }, true);

        isOddbean = isEventPostedByOddbean(packed);
    }
};

struct TopicEventRendered {
    TopicEventScores scores;

    uint64_t n;
    std::string noteId;
    uint64_t createdAt;
    std::string summaryHtml;
    std::string userNpub;
    std::string username;
    std::string timestamp;

    TopicEventRendered(const TopicEventScores &scores) : scores(scores) {
    }

    std::string pubDate() const {
        return renderTimestampToHTTPTime(createdAt);
    }
};

struct TopicEvents {
    std::string topic;
    bool showAll;
    uint64_t startN;
    uint64_t resumeTime;
    std::string externalUrl = cfg().web__externalUrl;

    bool rssMode = false;

    std::string rssHeader = R"(<?xml version="1.0"?><rss version="2.0" xmlns:atom="http://www.w3.org/2005/Atom">)";
    std::string rssFooter = R"(</rss>)";

    std::vector<TopicEventRendered> events;
    std::optional<uint64_t> nextResumeTime;
    std::string urlPath;
    std::string topicRendered;

    uint64_t EVENTS_PER_PAGE = 30;
    uint64_t LOOKBACK_SECONDS = 86400*2;
    uint64_t MAX_EVENTS_TO_SCAN = 200'000;

    TopicEvents(const std::string &topic, bool showAll, uint64_t startN, uint64_t resumeTime) : topic(topic), showAll(showAll), startN(startN), resumeTime(resumeTime) {
    }

    void process(lmdb::txn &txn, Decompressor &decomp) {
        std::vector<TopicEventScores> eventsAll;

        UserCache userCache;
        auto now = hoytech::curr_time_s();

        if (resumeTime > now) resumeTime = now;

        auto cb = [&](uint64_t levId){
            auto ev = lookupEventByLevId(txn, levId);
            PackedEventView packed(ev.buf);

            if (packed.kind() != 1) return true;
            if (packed.created_at() > now) return true;
            if (!isRootEvent(txn, packed)) return true;

            eventsAll.push_back(TopicEventScores(txn, ev, packed));

            if (showAll) {
                if (eventsAll.size() >= EVENTS_PER_PAGE) return false;
            } else {
                if (now - resumeTime > LOOKBACK_SECONDS && eventsAll.size() >= EVENTS_PER_PAGE) return false;
                if (eventsAll.size() > MAX_EVENTS_TO_SCAN) return false;
            }

            return true;
        };

        if (topic.size()) {
            std::string prefix = "t";
            prefix += topic;
            env.generic_foreachFull(txn, env.dbi_Event__tag, makeKey_StringUint64(prefix, resumeTime), "", [&](std::string_view k, std::string_view v){
                ParsedKey_StringUint64 parsedKey(k);
                if (parsedKey.s != prefix) return false;

                auto levId = lmdb::from_sv<uint64_t>(v);
                return cb(levId);
            }, true);
        } else {
            uint64_t kind = 1;
            env.generic_foreachFull(txn, env.dbi_Event__kind, makeKey_Uint64Uint64(kind, resumeTime), lmdb::to_sv<uint64_t>(MAX_U64), [&](std::string_view k, std::string_view v){
                ParsedKey_Uint64Uint64 parsedKey(k);
                if (parsedKey.n1 != kind) return false;

                auto levId = lmdb::from_sv<uint64_t>(v);
                return cb(levId);
            }, true);
        }

        if (showAll) {
            // no scoring/filtering, just all notes sorted by time
        } else {
            double maxScore = 0.0;

            for (auto &e : eventsAll) {
                e.score = e.comments + e.upVotes;
                if (e.score > maxScore) maxScore = e.score;
            }

            for (auto &e : eventsAll) {
                if (e.isOddbean) e.score = (e.score * 10) + maxScore / 3;
            }

            for (auto &e : eventsAll) {
                // time decay
                double age = (double)now - e.timestamp;
                double scale = (double)LOOKBACK_SECONDS - age;
                if (scale < 0.0) scale = 0.0;
                e.score = e.score * scale;
            }

            std::sort(eventsAll.begin(), eventsAll.end(), [](auto &a, auto &b){ return a.score > b.score; });

            if (eventsAll.size() > EVENTS_PER_PAGE) eventsAll.erase(eventsAll.begin() + EVENTS_PER_PAGE, eventsAll.end());

            std::sort(eventsAll.begin(), eventsAll.end(), [](auto &a, auto &b){ return a.timestamp > b.timestamp; });
        }

        if (eventsAll.size()) {
            nextResumeTime = eventsAll.back().timestamp - 1; // might skip over some events with exact same timestamp.. oh well
        }

        for (const auto &eScore : eventsAll) {
            PackedEventView packed(eScore.eventView.buf);

            TopicEventRendered rendered(eScore);

            rendered.n = startN + events.size();

            {
                Event event(eScore.eventView);
                event.populateJson(txn, decomp);
                rendered.noteId = event.getNoteId();
                rendered.createdAt = event.getCreatedAt();

                rendered.summaryHtml = event.summaryHtml(rendered.noteId, rssMode);
            }

            {
                auto user = userCache.getUser(txn, decomp, std::string(packed.pubkey()));
                rendered.userNpub = user->npubId;
                rendered.username = user->username;
            }

            rendered.timestamp = renderTimestamp(now, packed.created_at());

            events.push_back(std::move(rendered));
        }

        if (topic.size()) {
            topicRendered = std::string("#") + topic;
            urlPath = std::string("/t/") + topic;
        } else {
            topicRendered = "homepage";
            urlPath = "/";
        }
    }

    bool isRootEvent(lmdb::txn &txn, PackedEventView &packed) {
        bool foundETag = false;

        packed.foreachTag([&](char tagName, std::string_view tagVal){
            if (tagName == 'e') {
                foundETag = true;
                return false;
            }

            return true;
        });

        return !foundETag;
    }

    TemplarResult render() {
        return tmpl::topic(*this);
    }

    TemplarResult renderRss() {
        return tmpl::topicRss(*this);
    }

    const char *showAllQueryString() const {
        return showAll ? "&all=1" : "";
    }
};
