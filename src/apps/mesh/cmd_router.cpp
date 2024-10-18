#include <docopt.h>
#include <tao/json.hpp>
#include <hoytech/file_change_monitor.h>
#include <hoytech/timer.h>
#include <uWebSockets/src/uWS.h>

#include "golpe.h"

#include "WriterPipeline.h"
#include "PluginEventSifter.h"
#include "events.h"
#include "filters.h"


static const char USAGE[] =
R"(
    Usage:
      router <routerConfigFile>

    Options:
)";



struct RouterEvent : NonCopyable {
    struct ConfigFileChange {
    };

    struct DBChange {
    };

    struct ReconnectCron {
    };

    using Var = std::variant<ConfigFileChange, DBChange, ReconnectCron>;
    Var msg;
    RouterEvent(Var &&msg_) : msg(std::move(msg_)) {}
};


struct ConnDesignator {
    std::string groupName;
    std::string url;
};



struct Router {
    struct StreamGroup : NonCopyable {
        std::string groupName;
        Router *router;

        std::string dir;
        std::string filterStr;
        std::string pluginDownCmd;
        std::string pluginUpCmd;
        std::vector<std::string> urls;

        struct Connection {
            uWS::WebSocket<uWS::CLIENT> *ws = nullptr;
            uint64_t started = 0;

            ~Connection() {
                if (ws) {
                    ws->close();
                    ws = nullptr;
                }
            }
        };
        std::map<std::string, Connection> conns; // url -> Connection

        tao::json::value filter;
        NostrFilterGroup filterCompiled;
        PluginEventSifter pluginDown;
        PluginEventSifter pluginUp;

        StreamGroup(std::string groupName, Router *router) : groupName(groupName), router(router) {
        }

        void configure(const tao::config::value &spec) {
            bool needsReconnect = false;

            {
                if (!spec.find("dir")) throw herr("no dir field");
                auto newDir = spec.at("dir").get_string();
                if (newDir != dir) needsReconnect = true;

                dir = newDir;
            }

            {
                tao::json::value newFilter = tao::json::empty_object;
                // FIXME: Must be better way to go from config object to json, instead of round-trip through string
                if (spec.find("filter")) newFilter = tao::json::from_string(tao::json::to_string(spec.at("filter")));

                std::string newFilterStr = tao::json::to_string(newFilter);
                if (newFilterStr != filterStr) needsReconnect = true;

                filterStr = newFilterStr;
                filterCompiled = NostrFilterGroup::unwrapped(newFilter);
                filter = newFilter;
            }


            pluginDownCmd = pluginUpCmd = "";
            if (spec.find("pluginDown")) pluginDownCmd = spec.at("pluginDown").get_string();
            if (spec.find("pluginUp")) pluginUpCmd = spec.at("pluginUp").get_string();


            if (!spec.find("urls")) throw herr("no urls field");
            urls.clear();
            for (const auto &url : spec.at("urls").get_array()) {
                urls.push_back(url.get_string());
            }

            // Disconnect any urls that were deleted

            {
                std::set<std::string> unneededUrls;
                for (auto &[url, c] : conns) unneededUrls.insert(url);
                for (const auto &url : urls) unneededUrls.erase(url);
                for (const auto &url : unneededUrls) conns.erase(url);
            }


            if (needsReconnect) {
                for (auto &[url, c] : conns) {
                    if (c.ws) c.ws->close();
                }
                conns.clear();
            }

            tryConnects();
        }


        void tryConnects() {
            for (const auto &url : urls) {
                if (conns.find(url) == conns.end()) conns.try_emplace(url);
                auto &c = conns.at(url);

                if (!c.ws && c.started + (router->connectionTimeoutUs * 2) < hoytech::curr_time_us()) {
                    LI << groupName << ": Connecting to " << url;
                    router->hub.connect(url, (void*)(new ConnDesignator(groupName, url)), {}, router->connectionTimeoutUs / 1'000, router->hubGroup);
                    c.started = hoytech::curr_time_us();
                }
            }
        }

        void connOpen(const std::string &url, uWS::WebSocket<uWS::CLIENT> *ws) {
            if (!conns.contains(url)) return;
            auto &c = conns.at(url);

            if (c.ws) {
                LI << "Already had open connection to " << url << ", closing";
                ws->close();
                return;
            }

            c.ws = ws;

            if (dir == "down" || dir == "both") {
                tao::json::value filterToSend = filter;
                filterToSend["limit"] = 0;

                auto msg = tao::json::to_string(tao::json::value::array({ "REQ", "X", filterToSend }));
                ws->send(msg.data(), msg.size(), uWS::OpCode::TEXT, nullptr, nullptr, true);
            }
        }

        void connClose(const std::string &url, uWS::WebSocket<uWS::CLIENT> *ws) {
            if (!conns.contains(url)) return;
            auto &c = conns.at(url);

            if (c.ws == ws) {
                c.ws = nullptr;
                c.started = 0;
            }
        }

        void incomingEvent(const std::string &url, tao::json::value &evJson) {
            if (dir == "up") return;

            std::string okMsg;

            auto res = pluginDown.acceptEvent(pluginDownCmd, evJson, EventSourceType::Stream, url, okMsg);
            if (res == PluginEventSifterResult::Accept) {
                router->writer.write({ std::move(evJson), });
            } else {
                if (okMsg.size()) LI << groupName << " / " << url << " : pluginDown blocked event " << evJson.at("id").get_string() << ": " << okMsg;
            }
        }

        void outgoingEvent(lmdb::txn &txn, defaultDb::environment::View_Event &ev, std::string &responseStr, tao::json::value &evJson) {
            if (dir == "down") return;
            if (!filterCompiled.doesMatch(PackedEventView(ev.buf))) return;

            if (responseStr.size() == 0) {
                auto evStr = getEventJson(txn, router->decomp, ev.primaryKeyId);
                evJson = tao::json::from_string(evStr);

                responseStr = std::string("[\"EVENT\",");
                responseStr += evStr;
                responseStr += "]";
            }

            std::string okMsg;

            auto res = pluginUp.acceptEvent(pluginUpCmd, evJson, EventSourceType::Stored, "", okMsg);
            if (res == PluginEventSifterResult::Accept) {
                for (auto &[url, c] : conns) {
                    if (c.ws) c.ws->send(responseStr.data(), responseStr.size(), uWS::OpCode::TEXT, nullptr, nullptr, true);
                }
            } else {
                if (okMsg.size()) LI << groupName << " : pluginUp blocked event " << evJson.at("id").get_string() << ": " << okMsg;
            }
        }
    };

    std::string routerConfigFile;
    const uint64_t defaultConnectionTimeoutUs = 20'000'000;
    uint64_t connectionTimeoutUs = 0;

    WriterPipeline writer;
    Decompressor decomp;
    hoytech::protected_queue<RouterEvent> inbox;
    uWS::Hub hub;
    uWS::Group<uWS::CLIENT> *hubGroup = nullptr;
    uS::Async *hubTrigger = nullptr;

    std::map<std::string, StreamGroup> streamGroups; // group name -> StreamGroup
    uint64_t currEventId = 0;
    bool firstConfigLoadSuccess = false;


    Router(std::string routerConfigFile) : routerConfigFile(routerConfigFile) {
        {
            auto txn = env.txn_ro();
            currEventId = getMostRecentLevId(txn);
        }

        hubGroup = hub.createGroup<uWS::CLIENT>(uWS::PERMESSAGE_DEFLATE | uWS::SLIDING_DEFLATE_WINDOW);

        hubGroup->onConnection([&](uWS::WebSocket<uWS::CLIENT> *ws, uWS::HttpRequest req) {
            auto *desig = (ConnDesignator*) ws->getUserData();
            LI << desig->groupName << ": Connected to " << desig->url;

            if (!streamGroups.contains(desig->groupName)) {
                // Connection to streamGroup that no longer exists
                ws->close();
                return;
            }

            streamGroups.at(desig->groupName).connOpen(desig->url, ws);
        });

        hubGroup->onDisconnection([&](uWS::WebSocket<uWS::CLIENT> *ws, int code, char *message, size_t length) {
            auto *desig = (ConnDesignator*) ws->getUserData();
            LI << desig->groupName << ": Disconnected from " << desig->url;

            if (streamGroups.contains(desig->groupName)) {
                streamGroups.at(desig->groupName).connClose(desig->url, ws);
            }

            delete desig;
        });

        hubGroup->onError([&](void *userData) {
            auto *desig = (ConnDesignator*) userData;
            LI << desig->groupName << ": Error connecting to " << desig->url;

            delete desig;
        });

        hubGroup->onMessage2([&](uWS::WebSocket<uWS::CLIENT> *ws, char *message, size_t length, uWS::OpCode, size_t) {
            auto *desig = (ConnDesignator*) ws->getUserData();

            if (!streamGroups.contains(desig->groupName)) {
                ws->close();
                return;
            }

            try {
                handleIncomingMessage(ws, desig, std::string_view(message, length));
            } catch (std::exception &e) {
                LW << "Failed to handle incoming message config: " << e.what();
            }
        });

        reconcileConfig();
    }


    void reconcileConfig() {
        LI << "Loading router config file: " << routerConfigFile;

        try {
            auto routerConfig = loadRawTaoConfig(routerConfigFile);

            for (const auto &[groupName, spec] : routerConfig.at("streams").get_object()) {
                if (!streamGroups.contains(groupName)) {
                    LI << "New stream group [" << groupName << "]";
                    streamGroups.try_emplace(groupName, groupName, this);
                }

                streamGroups.at(groupName).configure(spec);
            }

            // remove streamGroups if they were deleted from config

            {
                std::set<std::string> unneededGroups;
                for (auto &[groupName, streamGroup] : streamGroups) unneededGroups.insert(groupName);
                for (const auto &[groupName, spec] : routerConfig.at("streams").get_object()) unneededGroups.erase(groupName);
                for (const auto &groupName : unneededGroups) streamGroups.erase(groupName);
            }

            // connectionTimeout

            uint64_t newTimeoutUs = defaultConnectionTimeoutUs;
            if (routerConfig.get_object().contains("connectionTimeout")) {
                newTimeoutUs = routerConfig.at("connectionTimeout").get_unsigned() * 1'000'000;
            }

            if (connectionTimeoutUs != newTimeoutUs) {
                connectionTimeoutUs = newTimeoutUs;
                LI << "Using connection timeout: " << (connectionTimeoutUs / 1'000'000) << " seconds";
            }
        } catch (std::exception &e) {
            LE << "Failed to parse router config: " << e.what();
            if (!firstConfigLoadSuccess) ::exit(1);
            return;
        }

        firstConfigLoadSuccess = true;
    }

    void onTrigger() {
        auto newMsgs = inbox.pop_all_no_wait();

        for (auto &newMsg : newMsgs) {
            if (std::get_if<RouterEvent::ConfigFileChange>(&newMsg.msg)) {
                reconcileConfig();
            } else if (std::get_if<RouterEvent::DBChange>(&newMsg.msg)) {
                handleDBChange();
            } else if (std::get_if<RouterEvent::ReconnectCron>(&newMsg.msg)) {
                for (auto &[groupName, streamGroup] : streamGroups) {
                    streamGroup.tryConnects();
                }
            }
        }
    }

    void handleIncomingMessage(uWS::WebSocket<uWS::CLIENT> *ws, ConnDesignator *desig, std::string_view msg) {
        auto origJson = tao::json::from_string(msg);

        if (!origJson.is_array()) throw herr("not an array");
        if (origJson.get_array().size() < 2) throw herr("array too short");

        auto &msgType = origJson.get_array().at(0);

        if (msgType == "EOSE") {
        } else if (msgType == "NOTICE") {
            LW << desig->groupName << " / " << desig->url << " NOTICE: " << tao::json::to_string(origJson);
        } else if (msgType == "OK") {
            if (!origJson.get_array().at(2).get_boolean()) {
                LW << desig->groupName << " / " << desig->url << " Event not written: " << origJson;
            }
        } else if (msgType == "EVENT") {
            if (origJson.get_array().size() < 3) throw herr("array too short");
            auto &evJson = origJson.at(2);
            streamGroups.at(desig->groupName).incomingEvent(desig->url, evJson);
        } else {
            LW << "Unexpected message: " << origJson;
        }
    }

    void handleDBChange() {
        auto txn = env.txn_ro();

        env.foreach_Event(txn, [&](auto &ev){
            currEventId = ev.primaryKeyId;

            std::string responseStr;
            tao::json::value json = tao::json::null;

            for (auto &[groupName, streamGroup] : streamGroups) {
                streamGroup.outgoingEvent(txn, ev, responseStr, json);
            }

            return true;
        }, false, currEventId + 1);
    }

    void run() {
        // Trigger

        hubTrigger = new uS::Async(hub.getLoop());

        std::function<void()> asyncCb = [&]{
            onTrigger();
        };

        hubTrigger->setData(&asyncCb);

        hubTrigger->start([](uS::Async *a){
            auto *r = static_cast<std::function<void()> *>(a->getData());
            (*r)();
        });

        // Config file change monitor

        hoytech::file_change_monitor configFileWatcher(routerConfigFile);

        configFileWatcher.run([&](){
            inbox.push_move(RouterEvent{RouterEvent::ConfigFileChange{}});
            hubTrigger->send();
        });


        // DB change monitor

        hoytech::file_change_monitor dbChangeWatcher(dbDir + "/data.mdb");

        dbChangeWatcher.setDebounce(100);

        dbChangeWatcher.run([&](){
            inbox.push_move(RouterEvent{RouterEvent::DBChange{}});
            hubTrigger->send();
        });


        // Reconnection timer

        hoytech::timer cron;

        cron.setupCb = []{ setThreadName("cron"); };

        cron.repeat(connectionTimeoutUs, [&]{
            inbox.push_move(RouterEvent{RouterEvent::ReconnectCron{}});
            hubTrigger->send();
        });

        cron.run();


        // Websocket

        hub.run();
    }
};


void cmd_router(const std::vector<std::string> &subArgs) {
    std::map<std::string, docopt::value> args = docopt::docopt(USAGE, subArgs, true, "");

    std::string routerConfigFile = args["<routerConfigFile>"].asString();

    Router router(routerConfigFile);

    router.run();
}
