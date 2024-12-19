#include "WebServer.h"
#include "WebData.h"

#include "FeedReader.h"
#include "WebStaticFiles.h"



std::string exportUserEvents(lmdb::txn &txn, Decompressor &decomp, std::string_view pubkey) {
    std::string output;

    env.generic_foreachFull(txn, env.dbi_Event__pubkey, makeKey_StringUint64(pubkey, MAX_U64), "", [&](std::string_view k, std::string_view v){
        ParsedKey_StringUint64 parsedKey(k);
        if (parsedKey.s != pubkey) return false;

        uint64_t levId = lmdb::from_sv<uint64_t>(v);
        output += getEventJson(txn, decomp, levId);
        output += "\n";

        return true;
    }, true);

    return output;
}


std::string exportEventThread(lmdb::txn &txn, Decompressor &decomp, std::string_view rootId) {
    std::string output;

    {
        auto rootEv = lookupEventById(txn, rootId);
        if (rootEv) {
            output += getEventJson(txn, decomp, rootEv->primaryKeyId);
            output += "\n";
        }
    }

    std::string prefix = "e";
    prefix += rootId;

    env.generic_foreachFull(txn, env.dbi_Event__tag, prefix, "", [&](std::string_view k, std::string_view v){
        ParsedKey_StringUint64 parsedKey(k);
        if (parsedKey.s != prefix) return false;

        auto levId = lmdb::from_sv<uint64_t>(v);

        output += getEventJson(txn, decomp, levId);
        output += "\n";

        return true;
    });

    return output;
}

void doSearch(lmdb::txn &txn, Decompressor &decomp, std::string_view search, std::vector<TemplarResult> &results) {
    auto doesPubkeyExist = [&](std::string_view pubkey){
        bool ret = false;

        env.generic_foreachFull(txn, env.dbi_Event__pubkey, makeKey_StringUint64(pubkey, 0), "", [&](std::string_view k, std::string_view v){
            ParsedKey_StringUint64 parsedKey(k);

            if (parsedKey.s == pubkey) ret = true;

            return false;
        });

        return ret;
    };

    if (search.size() == 64) {
        try {
            auto word = from_hex(search);
            if (doesPubkeyExist(word)) results.emplace_back(tmpl::search::userResult(User(txn, decomp, word)));
        } catch(...) {}
    }

    if (search.starts_with("npub1")) {
        try {
            auto word = decodeBech32Simple(search);
            if (doesPubkeyExist(word)) results.emplace_back(tmpl::search::userResult(User(txn, decomp, word)));
        } catch(...) {}
    }

    try {
        auto e = Event::fromIdExternal(txn, search);
        results.emplace_back(tmpl::search::eventResult(e));
    } catch(...) {}
}





TemplarResult renderFeed(lmdb::txn &txn, Decompressor &decomp, UserCache &userCache, const FeedReader &feedReader, uint64_t resultsPerPage, uint64_t page) {
    auto events = feedReader.getEvents(txn, decomp, resultsPerPage, page);

    std::vector<TemplarResult> rendered;
    auto now = hoytech::curr_time_s();
    uint64_t offset = (page * resultsPerPage) + 1;
    uint64_t n = 0;

    for (auto &fe : events) {
        auto ev = Event::fromLevId(txn, fe.levId);
        ev.populateJson(txn, decomp);

        auto summary = ev.summaryHtml();
        std::string url;
        if (summary.url.size()) {
            url = summary.url;
        } else {
            url += "/e/";
            url += ev.getNoteId();
        }

        struct {
            uint64_t n;
            const Event &ev;
            const std::string &text;
            const std::string &url;
            const std::string &domain;
            const User &user;
            std::string timestamp;
            FeedReader::EventInfo &info;
        } ctx = {
            offset + n,
            ev,
            summary.text,
            url,
            summary.getDomain(),
            *userCache.getUser(txn, decomp, ev.getPubkey()),
            renderTimestamp(now, ev.getCreatedAt()),
            fe.info,
        };

        rendered.emplace_back(tmpl::feed::item(ctx));
        n++;
    }

    struct {
        const std::vector<TemplarResult> &items;
        uint64_t n;
        uint64_t resultsPerPage;
        uint64_t page;
    } ctx = {
        rendered,
        n,
        resultsPerPage,
        page,
    };

    return tmpl::feed::list(ctx);
}






HTTPResponse WebServer::generateReadResponse(lmdb::txn &txn, Decompressor &decomp, const HTTPRequest &req) {
    HTTPResponse httpResp;

    auto startTime = hoytech::curr_time_us();
    Url u(req.url);

    LI << "READ REQUEST: " << req.url;

    UserCache userCache;

    std::string_view code = "200 OK";
    std::string_view contentType = "text/html; charset=utf-8";

    // Normal frame:

    std::optional<TemplarResult> body;
    std::string title;

    // Or, raw:

    std::optional<std::string> rawBody;

    // Misc:

    std::optional<FeedReader> feedReader;

    auto handleFeed = [&](std::string_view feedId){
        uint64_t resultsPerPage = 30;
        uint64_t page = 0;

        try {
            auto pageStr = u.lookupQuery("p");
            if (pageStr) page = std::stoull(std::string(*pageStr));
        } catch(...) {}

        feedReader.emplace(txn, decomp, feedId);

        if (feedReader->found) {
            body = renderFeed(txn, decomp, userCache, *feedReader, resultsPerPage, page);
            httpResp.extraHeaders += "Cache-Control: max-age=30\r\n";
        } else {
            rawBody = "Feed not found.";
        }
    };

    if (u.path.size() == 0) {
        handleFeed("homepage");
    } else if (u.path[0] == "e") {
        if (u.path.size() == 2) {
            EventThread et(txn, decomp, decodeBech32Simple(u.path[1]));
            body = et.render(txn, decomp, userCache);
            title = et.getSummary();
        } else if (u.path.size() == 3) {
            if (u.path[2] == "reply") {
                auto ev = Event::fromIdExternal(txn, u.path[1]);
                ev.populateJson(txn, decomp);

                RenderedEventCtx ctx;

                ctx.timestamp = renderTimestamp(startTime / 1'000'000, ev.getCreatedAt());
                ctx.content = templarInternal::htmlEscape(ev.json.at("content").get_string(), false);
                ctx.ev = &ev;
                ctx.user = userCache.getUser(txn, decomp, ev.getPubkey());
                ctx.showActions = false;

                body = tmpl::event::reply(ctx);
            } else if (u.path[2] == "raw.json") {
                auto ev = Event::fromIdExternal(txn, u.path[1]);
                ev.populateJson(txn, decomp);
                rawBody = tao::json::to_string(ev.json, 4);
                contentType = "application/json; charset=utf-8";
            } else if (u.path[2] == "export.jsonl") {
                rawBody = exportEventThread(txn, decomp, decodeBech32Simple(u.path[1]));
                contentType = "application/jsonl+json; charset=utf-8";
            }
        }
    } else if (u.path[0] == "u") {
        if (u.path.size() == 2) {
            User user(txn, decomp, decodeBech32Simple(u.path[1]));
            title = std::string("profile: ") + user.username;
            body = tmpl::user::metadata(user);
        } else if (u.path.size() == 3) {
            std::string userPubkey;

            if (u.path[1].starts_with("npub1")) {
                userPubkey = decodeBech32Simple(u.path[1]);
            } else {
                userPubkey = from_hex(u.path[1]);
            }

            if (u.path[2] == "notes") {
                uint64_t resumeTime = MAX_U64;

                try {
                    auto resumeTimeStr = u.lookupQuery("next");
                    if (resumeTimeStr) resumeTime = std::stoull(std::string(*resumeTimeStr));
                } catch(...) {}

                UserEvents uc(txn, decomp, userPubkey, resumeTime);
                title = std::string("notes: ") + uc.u.username;
                body = uc.render(txn, decomp);
            } else if (u.path[2] == "export.jsonl") {
                rawBody = exportUserEvents(txn, decomp, userPubkey);
                contentType = "application/jsonl+json; charset=utf-8";
            } else if (u.path[2] == "metadata.json") {
                User user(txn, decomp, userPubkey);
                rawBody = user.kind0Found() ? tao::json::to_string(*user.kind0Json) : "{}";
                contentType = "application/json; charset=utf-8";
            } else if (u.path[2] == "following") {
                User user(txn, decomp, userPubkey);
                title = std::string("following: ") + user.username;
                user.populateContactList(txn, decomp);

                uint64_t numFollowing = 0;
                if (user.kind3Event) {
                    for (const auto &tagJson : user.kind3Event->at("tags").get_array()) {
                        const auto &tag = tagJson.get_array();
                        if (tag.size() >= 2 && tag.at(0).get_string() == "p") numFollowing++;
                    }
                }

                struct {
                    User &user;
                    std::function<const User*(const std::string &)> getUser;
                    uint64_t numFollowing;
                } ctx = {
                    user,
                    [&](const std::string &pubkey){ return userCache.getUser(txn, decomp, pubkey); },
                    numFollowing,
                };

                body = tmpl::user::following(ctx);
            } else if (u.path[2] == "followers") {
                uint64_t resultsPerPage = 500;
                uint64_t page = 0;

                try {
                    auto pageStr = u.lookupQuery("p");
                    if (pageStr) page = std::stoull(std::string(*pageStr));
                } catch(...) {}

                User user(txn, decomp, userPubkey);
                title = std::string("followers: ") + user.username;
                uint64_t numFollowers = 0;
                auto followers = user.getFollowers(txn, decomp, user.pubkey, page * resultsPerPage, resultsPerPage, &numFollowers);
                uint64_t numPages = (numFollowers + resultsPerPage + 1) / resultsPerPage;

                struct {
                    const User &user;
                    const std::vector<std::string> &followers;
                    uint64_t numFollowers;
                    std::function<const User*(const std::string &)> getUser;
                    uint64_t page;
                    uint64_t numPages;
                } ctx = {
                    user,
                    followers,
                    numFollowers,
                    [&](const std::string &pubkey){ return userCache.getUser(txn, decomp, pubkey); },
                    page,
                    numPages,
                };

                body = tmpl::user::followers(ctx);
            }
        }
    } else if (u.path[0] == "f") {
        if (u.path.size() == 2) {
            handleFeed(u.path[1]);
        } else if (u.path.size() == 3 && u.path[2] == "info") {
            feedReader.emplace(txn, decomp, u.path[1]);

            std::string title, description;

            try {
                auto &contentObj = feedReader->content.get_object();

                if (contentObj.contains("title")) title = contentObj["title"].get_string();
                if (contentObj.contains("description")) description = contentObj["description"].get_string();
                //if (contentObj.contains("style") && contentObj["style"].is_object()) {
                //    auto styleObj = contentObj["style"].get_object();
                //}
            } catch(...) {}

            std::string feedPath;
            if (u.path[1] == "homepage") feedPath = "/";
            else {
                feedPath = "/f/";
                feedPath += u.path[1];
            }

            if (feedReader->found) {
                struct {
                    const std::optional<FeedReader> &feedReader;
                    const User &curator;
                    const std::string &title;
                    const std::string &description;
                    const std::string &feedPath;
                } ctx = {
                    feedReader,
                    *userCache.getUser(txn, decomp, feedReader->pubkey),
                    title,
                    description,
                    feedPath,
                };

                body = tmpl::feed::info(ctx);
                title = feedReader->feedName;
            } else {
                rawBody = "Feed not found.";
            }
        }
    } else if (u.path[0] == "search") {
        std::vector<TemplarResult> results;

        if (u.query.starts_with("q=")) {
            std::string_view search = u.query.substr(2);

            doSearch(txn, decomp, search, results);

            struct {
                std::string_view search;
                const std::vector<TemplarResult> &results;
            } ctx = {
                search,
                results,
            };

            body = tmpl::searchPage(ctx);
        }
    } else if (u.path[0] == "post") {
        body = tmpl::newPost(nullptr);
    } else if (u.path[0] == "static" && u.path.size() >= 2) {
        httpResp.extraHeaders += "Cache-Control: max-age=31536000\r\n";

        if (u.path[1] == "oddbean.js") {
            rawBody = std::string(oddbeanStatic__oddbean_js());
            contentType = "application/javascript";
        } else if (u.path[1] == "oddbean.css") {
            rawBody = std::string(oddbeanStatic__oddbean_css());
            contentType = "text/css";
        } else if (u.path[1] == "oddbean.svg") {
            rawBody = std::string(oddbeanStatic__oddbean_svg());
            contentType = "image/svg+xml";
        }
    } else if (u.path[0] == "favicon.ico") {
        rawBody = std::string(oddbeanStatic__favicon_ico());
        contentType = "image/x-icon";
        httpResp.extraHeaders += "Cache-Control: max-age=2592000\r\n";
        httpResp.noCompress = true;
    } else if (u.path[0] == "login") {
        body = tmpl::login(0);
    } else if (u.path[0] == "about") {
        body = tmpl::about(0);
    }




    std::string responseData;

    if (body) {
        if (title.size()) title += " | ";

        struct {
            const TemplarResult &body;
            std::string_view title;
            std::string staticFilesPrefix;
            std::string_view staticOddbeanCssHash;
            std::string_view staticOddbeanJsHash;
            std::string_view staticOddbeanSvgHash;
            const std::optional<FeedReader> &feedReader;
        } ctx = {
            *body,
            title,
            cfg().web__staticFilesPrefix.size() ? cfg().web__staticFilesPrefix : "/static",
            oddbeanStatic__oddbean_css__hash().substr(0, 16),
            oddbeanStatic__oddbean_js__hash().substr(0, 16),
            oddbeanStatic__oddbean_svg__hash().substr(0, 16),
            feedReader,
        };

        responseData = std::move(tmpl::main(ctx).str);
    } else if (rawBody) {
        responseData = std::move(*rawBody);
    } else {
        code = "404 Not Found";
        body = TemplarResult{ "Not found" };
    }

    httpResp.body = responseData;
    httpResp.code = code;
    httpResp.contentType = contentType;

    LI << "Reply: " << code << " / " << responseData.size() << " bytes in " << (hoytech::curr_time_us() - startTime) << "us";

    return httpResp;
}


void WebServer::runReader(ThreadPool<MsgWebReader>::Thread &thr) {
    Decompressor decomp;

    while(1) {
        auto newMsgs = thr.inbox.pop_all();

        auto txn = env.txn_ro();

        for (auto &newMsg : newMsgs) {
            if (auto msg = std::get_if<MsgWebReader::Request>(&newMsg.msg)) {
                try {
                    HTTPResponse resp = generateReadResponse(txn, decomp, msg->req);
                    std::string payload = resp.encode(msg->req.acceptGzip);
                    sendHttpResponseRaw(msg->req, payload);
                } catch (std::exception &e) {
                    HTTPResponse res;
                    res.code = "500 Server Error";
                    res.body = "Server error";

                    std::string payload = res.encode(false);

                    sendHttpResponseRaw(msg->req, payload);
                    LE << "500 server error: " << e.what();
                }
            }
        }
    }
}
