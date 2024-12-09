#pragma once

struct FeedReader {
    struct EventInfo {
        uint64_t comments = 0;
        double score = 0.0;
    };

    struct FeedEvent {
        uint64_t levId;
        std::string id;

        EventInfo info;
    };

    tao::json::value feedJson;

    FeedReader(lmdb::txn &txn, const std::string &feedId) {
        size_t pos = feedId.find(".");
        if (pos == std::string_view::npos) throw herr("bad feedId");
        std::string pubkey = FeedId.substr(0, pos);
        std::string adminTopic = feedId.substr(pos + 1);

        tao::json::value filter = tao::json::value({
            { "authors", tao::json::value::array({ to_hex(authorPubkey) }) },
            { "kinds", tao::json::value::array({ uint64_t(33800) }) },
            { "#d", tao::json::value::array({ adminTopic }) },
        });

        bool found = false;

        foreachByFilter(txn, filter, [&](uint64_t levId){
            feedJson = tao::json::from_string(getEventJson(txn, decomp, levId));
            found = true;
            return false;
        });

        if (!found) throw herr("unable to lookup feedId: ", feedId);
    }

    std::vector<FeedEvent> getEvents(lmdb::txn &txn, Decompressor &decomp) {
        return {};
    }
};
