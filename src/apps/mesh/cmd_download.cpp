#include <docopt.h>
#include <tao/json.hpp>
#include <hoytech/protected_queue.h>
#include <hoytech/file_change_monitor.h>

#include "golpe.h"

#include "WSConnection.h"
#include "./MeshUtils.h"


static const char USAGE[] =
R"(
    Usage:
      download <url> [--filter=<filter>] [--range=<range>]

    Options:
      --filter=<filter>  Nostr filter
      --range=<range>    Add since and until fields to filter. Format: START-END
                         Examples: 2M- (last 2 months), 1Y-3w (from 1 year 3 weeks old)
                         Units: s=seconds, m=minutes, h=hours, d=days, w=weeks, M=months, Y=years
)";



void cmd_download(const std::vector<std::string> &subArgs) {
    std::map<std::string, docopt::value> args = docopt::docopt(USAGE, subArgs, true, "");

    std::string url = args["<url>"].asString();

    std::string filterStr;
    if (args["--filter"]) filterStr = args["--filter"].asString();
    else filterStr = "{}";
    tao::json::value filterJson = tao::json::from_string(filterStr);

    if (args["--range"]) processRangeOption(args["--range"].asString(), filterJson);


    WSConnection ws(url);

    ws.onConnect = [&]{
        auto encoded = tao::json::to_string(tao::json::value::array({ "REQ", "_", filterJson }));
        ws.send(encoded);
    };

    ws.onMessage = [&](auto msg, uWS::OpCode, size_t){
        try {
            auto origJson = tao::json::from_string(msg);

            if (origJson.is_array()) {
                if (origJson.get_array().size() < 2) throw herr("array too short");

                auto &msgType = origJson.get_array().at(0);
                if (msgType == "EOSE") {
                    ::exit(0);
                } else if (msgType == "NOTICE") {
                    LW << "NOTICE message: " << tao::json::to_string(origJson);
                    return;
                } else if (msgType == "OK") {
                    if (!origJson.get_array().at(2).get_boolean()) {
                        LW << "Event not written: " << origJson;
                    }
                } else if (msgType == "EVENT") {
                    if (origJson.get_array().size() < 3) throw herr("array too short");
                    auto &evJson = origJson.at(2);

                    std::cout << tao::json::to_string(evJson) << "\n";
                } else {
                    throw herr("unexpected message type: ", msgType);
                }
            } else {
                throw herr("unexpected message");
            }
        } catch (std::exception &e) {
            LE << "Error receiving nostr message: " << e.what() << " message: " << msg;
            ::exit(1);
        }
    };

    ws.run();
}
