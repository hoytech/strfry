#include <iostream>

#include <docopt.h>
#include "golpe.h"

#include "ActiveMonitors.h"
#include "events.h"


static const char USAGE[] =
R"(
    Usage:
      monitor
)";


// echo '["sub",1,"mysub",{"authors":["47f7163b"]}]' | ./strfry monitor

void cmd_monitor(const std::vector<std::string> &subArgs) {
    std::map<std::string, docopt::value> args = docopt::docopt(USAGE, subArgs, true, "");

    auto txn = env.txn_ro();

    Decompressor decomp;
    ActiveMonitors monitors;

    std::string line;
    uint64_t interestConnId = 0;
    std::string interestSubId;

    while (std::cin) {
        std::getline(std::cin, line);
        if (!line.size()) continue;

        auto msg = tao::json::from_string(line);
        auto &msgArr = msg.get_array();

        auto cmd = msgArr.at(0).get_string();

        if (cmd == "sub") {
            Subscription sub(msgArr.at(1).get_unsigned(), msgArr.at(2).get_string(), NostrFilterGroup::unwrapped(msgArr.at(3)));
            sub.latestEventId = 0;
            monitors.addSub(txn, std::move(sub), 0);
        } else if (cmd == "removeSub") {
            monitors.removeSub(msgArr.at(1).get_unsigned(), SubId(msgArr.at(2).get_string()));
        } else if (cmd == "closeConn") {
            monitors.closeConn(msgArr.at(1).get_unsigned());
        } else if (cmd == "interest") {
            if (interestConnId) throw herr("interest already set");
            interestConnId = msgArr.at(1).get_unsigned();
            interestSubId = msgArr.at(2).get_string();
        } else {
            throw herr("unknown cmd");
        }
    }

    env.foreach_Event(txn, [&](auto &ev){
        monitors.process(txn, ev, [&](RecipientList &&recipients, uint64_t levId){
            for (auto &r : recipients) {
                if (r.connId == interestConnId && r.subId.str() == interestSubId) {
                    std::cout << getEventJson(txn, decomp, levId) << "\n";
                }
            }
        });
        return true;
    });
}
