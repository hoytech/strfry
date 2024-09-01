#include <iostream>

#include <docopt.h>
#include "golpe.h"

#include "events.h"
#include "PackedEvent.h"


static const char USAGE[] =
R"(
    Usage:
      export [--since=<since>] [--until=<until>] [--reverse] [--fried]
)";


std::string getPackedEventBuf(lmdb::txn &txn, uint64_t levId) {
    auto ev = lookupEventByLevId(txn, levId);
    auto *flat = ev.flat_nested();

    PackedEventTagBuilder tagBuilder;

    // Unfortunately we lose the original ordering...

    for (const auto &tagPair : *(flat->tagsGeneral())) {
        auto tagName = (char)tagPair->key();
        auto tagVal = sv(tagPair->val());
        tagBuilder.add(tagName, tagVal);
    }

    for (const auto &tagPair : *(flat->tagsFixed32())) {
        auto tagName = (char)tagPair->key();
        auto tagVal = sv(tagPair->val());
        tagBuilder.add(tagName, tagVal);
    }

    PackedEventBuilder builder(sv(flat->id()), sv(flat->pubkey()), flat->created_at(), flat->kind(), flat->expiration(), tagBuilder);

    return std::move(builder.buf);
}


void cmd_export(const std::vector<std::string> &subArgs) {
    std::map<std::string, docopt::value> args = docopt::docopt(USAGE, subArgs, true, "");

    uint64_t since = 0, until = MAX_U64;
    if (args["--since"]) since = args["--since"].asLong();
    if (args["--until"]) until = args["--until"].asLong();
    bool reverse = args["--reverse"].asBool();
    bool fried = args["--fried"].asBool();

    Decompressor decomp;

    auto txn = env.txn_ro();

    auto dbVersion = getDBVersion(txn);

    if (dbVersion == 0) throw herr("migration from DB version 0 not supported by this version of strfry");

    uint64_t start = reverse ? until : since;
    uint64_t startDup = reverse ? MAX_U64 : 0;

    exitOnSigPipe();

    std::string o;

    env.generic_foreachFull(txn, env.dbi_Event__created_at, lmdb::to_sv<uint64_t>(start), lmdb::to_sv<uint64_t>(startDup), [&](auto k, auto v) {
        if (reverse) {
            if (lmdb::from_sv<uint64_t>(k) < since) return false;
        } else {
            if (lmdb::from_sv<uint64_t>(k) > until) return false;
        }

        auto levId = lmdb::from_sv<uint64_t>(v);
        std::string_view json = getEventJson(txn, decomp, levId);

        if (fried) {
            std::string friedBuf = getPackedEventBuf(txn, levId);

            o.clear();
            o.reserve(json.size() + friedBuf.size() * 2 + 100);
            o = json;
            o.resize(o.size() - 1);
            o += ",\"fried\":\"";
            o += to_hex(friedBuf);
            o += "\"}\n";

            std::cout << o;
        } else {
            std::cout << json << "\n";
        }

        return true;
    }, reverse);
}
