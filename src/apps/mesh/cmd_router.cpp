#include <docopt.h>
#include <tao/json.hpp>
#include <hoytech/file_change_monitor.h>

#include "golpe.h"

#include "EventStreamer.h"
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



static std::unique_ptr<WriterPipeline> globalRouterWriter;


struct IncomingEvent : NonCopyable {
    struct Down {
        tao::json::value evJson;
        std::string url;
    };

    struct Up {
        std::shared_ptr<std::string> evStr;
        std::shared_ptr<tao::json::value> evJson;
    };

    struct Shutdown {
    };

    using Var = std::variant<Down, Up, Shutdown>;
    Var msg;
    IncomingEvent(Var &&msg_) : msg(std::move(msg_)) {}
};

struct StreamerInstance : NonCopyable {
    hoytech::protected_queue<IncomingEvent> &inbox;
    EventStreamer es;
    std::thread t;

    StreamerInstance(hoytech::protected_queue<IncomingEvent> &inbox, const std::string &url, const std::string &dir, tao::json::value filter) : inbox(inbox), es(url, dir, filter) {
        es.onIncomingEvent = [&](tao::json::value &&evJson) {
            inbox.push_move(IncomingEvent{IncomingEvent::Down{ std::move(evJson), es.url }});
        };

        t = std::thread([this]{
            es.run();
        });
    }

    ~StreamerInstance() {
        es.close();
        t.join();
    }
};

struct StreamGroup : NonCopyable {
    std::string groupName;

    std::string dir;
    std::string filterStr;
    std::string pluginDownCmd;
    std::string pluginUpCmd;
    std::map<std::string, StreamerInstance> streams; // url -> StreamerInstance

    std::thread t;
    hoytech::protected_queue<IncomingEvent> inbox;

    NostrFilterGroup filterCompiled;
    PluginEventSifter pluginDown;
    PluginEventSifter pluginUp;

    StreamGroup(std::string groupName, const tao::config::value &spec) : groupName(groupName) {
        if (!spec.find("dir")) throw herr("no dir field");
        dir = spec.at("dir").get_string();


        tao::json::value filter = tao::json::empty_object;
        // FIXME: Must be better way to go from config object to json, instead of round-trip through string
        if (spec.find("filter")) filter = tao::json::from_string(tao::json::to_string(spec.at("filter")));

        filterStr = tao::json::to_string(filter);
        filterCompiled = NostrFilterGroup::unwrapped(filter);


        if (spec.find("pluginDown")) pluginDownCmd = spec.at("pluginDown").get_string();
        if (spec.find("pluginUp")) pluginUpCmd = spec.at("pluginUp").get_string();


        if (!spec.find("urls")) throw herr("no urls field");
        for (const auto &url : spec.at("urls").get_array()) {
            streams.try_emplace(url.get_string(), inbox, url.get_string(), dir, filter);
        }


        t = std::thread([this]{
            while (1) {
                auto newMsgs = inbox.pop_all();

                for (auto &m : newMsgs) {
                    if (std::get_if<IncomingEvent::Shutdown>(&m.msg)) return;
                    handleIncomingEvent(m);
                }
            }
        });
    }

    void sendEvent(std::shared_ptr<std::string> evStr, std::shared_ptr<tao::json::value> evJson) {
        inbox.push_move(IncomingEvent{IncomingEvent::Up{ std::move(evStr), std::move(evJson) }});
    }

    ~StreamGroup() {
        inbox.push_move(IncomingEvent{IncomingEvent::Shutdown{}});
        t.join();
    }

  private:
    void handleIncomingEvent(IncomingEvent &m) {
        if (auto ev = std::get_if<IncomingEvent::Down>(&m.msg)) {
            if (dir == "up") return;

            std::string okMsg;

            auto res = pluginDown.acceptEvent(pluginDownCmd, ev->evJson, hoytech::curr_time_s(), EventSourceType::Stream, ev->url, okMsg);
            if (res == PluginEventSifterResult::Accept) {
                globalRouterWriter->write({ std::move(ev->evJson), EventSourceType::Stream, ev->url });
            } else {
                LI << "[" << groupName << "] " << ev->url << ": pluginDown blocked event " << ev->evJson.at("id").get_string() << ": " << okMsg;
            }
        } else if (auto ev = std::get_if<IncomingEvent::Up>(&m.msg)) {
            if (dir == "down") return;

            std::string okMsg;

            auto res = pluginUp.acceptEvent(pluginUpCmd, ev->evJson, hoytech::curr_time_s(), EventSourceType::Stream, "", okMsg);
            if (res == PluginEventSifterResult::Accept) {
                for (auto &[url, streamer] : streams) {
                    streamer.es.sendEvent(ev->evStr);
                    streamer.es.trigger();
                }
            } else {
                LI << "[" << groupName << "] pluginUp blocked event " << ev->evJson->at("id").get_string() << ": " << okMsg;
            }
        }
    }
};



void cmd_router(const std::vector<std::string> &subArgs) {
    std::map<std::string, docopt::value> args = docopt::docopt(USAGE, subArgs, true, "");

    std::string routerConfigFile = args["<routerConfigFile>"].asString();


    globalRouterWriter = std::make_unique<WriterPipeline>();
    Decompressor decomp;

    std::mutex groupsMutex;
    std::map<std::string, StreamGroup> streamGroups; // group name -> StreamGroup


    // Config

    bool configLoadSuccess = false;

    auto reconcileConfig = [&]{
        LI << "Loading router config file: " << routerConfigFile;

        try {
            auto routerConfig = loadRawTaoConfig(routerConfigFile);

            std::lock_guard<std::mutex> guard(groupsMutex);

            for (const auto &[k, v] : routerConfig.at("streams").get_object()) {
                if (!streamGroups.contains(k)) {
                    LI << "New stream group [" << k << "]";
                    streamGroups.try_emplace(k, k, v);
                }
            }
        } catch (std::exception &e) {
            LE << "Failed to parse router config: " << e.what();
            if (!configLoadSuccess) ::exit(1);
            return;
        }

        configLoadSuccess = true;
    };

    hoytech::file_change_monitor configFileWatcher(routerConfigFile);

    reconcileConfig();

    configFileWatcher.run(reconcileConfig);


    // DB change monitor

    uint64_t currEventId;

    {
        auto txn = env.txn_ro();
        currEventId = getMostRecentLevId(txn);
    }

    hoytech::file_change_monitor dbChangeWatcher(dbDir + "/data.mdb");

    dbChangeWatcher.setDebounce(100);

    dbChangeWatcher.run([&](){
        std::lock_guard<std::mutex> guard(groupsMutex);

        auto txn = env.txn_ro();

        env.foreach_Event(txn, [&](auto &ev){
            currEventId = ev.primaryKeyId;

            auto evStr = getEventJson(txn, decomp, ev.primaryKeyId);

            std::string msg = std::string("[\"EVENT\",");
            msg += evStr;
            msg += "]";

            auto msgPtr = std::make_shared<std::string>(std::move(msg));
            auto jsonPtr = std::make_shared<tao::json::value>(tao::json::from_string(evStr));

            {
                for (auto &[groupName, streamGroup] : streamGroups) {
                    if (!streamGroup.filterCompiled.doesMatch(ev.flat_nested())) continue; // OK to access streamGroup innards because mutex
                    streamGroup.sendEvent(msgPtr, jsonPtr);
                }
            }

            return true;
        }, false, currEventId + 1);
    });


    pause();
}
