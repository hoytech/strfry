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

    std::vector<FeedEvent> getEvents(lmdb::txn &txn, Decompressor &decomp, const std::string &feedId) {
        size_t pos = feedId.find(".");
        if (pos == std::string_view::npos) throw herr("bad feedId: ", feedId);
        std::string pubkey = from_hex(feedId.substr(0, pos));
        std::string adminTopic = feedId.substr(pos + 1);

        tao::json::value filter = tao::json::value({
            { "authors", tao::json::value::array({ to_hex(pubkey) }) },
            { "kinds", tao::json::value::array({ uint64_t(33800) }) },
            { "#d", tao::json::value::array({ adminTopic }) },
        });

        bool found = false;
        tao::json::value feedJson;

        foreachByFilter(txn, filter, [&](uint64_t levId){
            feedJson = tao::json::from_string(getEventJson(txn, decomp, levId));
            found = true;
            return false;
        });

        if (!found) throw herr("unable to lookup feedId: ", feedId);

        std::vector<FeedEvent> output;

        const auto &tags = feedJson.at("tags").get_array();

        for (const auto &tag : tags) {
            if (tag[0] != "e") continue;
            std::string id = from_hex(tag[1].get_string());

            auto ev = lookupEventById(txn, id);
            if (!ev) continue;

            output.push_back({
                ev->primaryKeyId,
                id,
                buildEventInfo(txn, id),
            });
        }

        return output;
    }

    EventInfo buildEventInfo(lmdb::txn &txn, const std::string &id) {
        EventInfo output;

        std::string prefix = "e";
        prefix += id;

        env.generic_foreachFull(txn, env.dbi_Event__tag, prefix, "", [&](std::string_view k, std::string_view v){
            ParsedKey_StringUint64 parsedKey(k);
            if (parsedKey.s != prefix) return false;

            auto childLevId = lmdb::from_sv<uint64_t>(v);
            auto childEv = lookupEventByLevId(txn, childLevId);

            PackedEventView packed(childEv.buf);
            if (packed.kind() == 1) output.comments++;
            else if (packed.kind() == 7) output.score++;

            return true;
        });

        return output;
    }
};
