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


struct StreamGroup : NonCopyable {
    std::string dir;
    std::string filterStr;
    NostrFilterGroup filterCompiled;

    std::optional<PluginEventSifter> pluginDown;
    std::optional<PluginEventSifter> pluginUp;

    struct StreamerInstance : NonCopyable {
        EventStreamer es;
        std::thread t;

        StreamerInstance(const std::string &url, const std::string &dir, tao::json::value filter) : es(url, dir, filter) {
            es.onEvent = [&](tao::json::value &&evJson, const WSConnection &ws) {
                globalRouterWriter->write({ std::move(evJson), EventSourceType::Stream, es.url });
            };

            t = std::thread([this]{
                es.run();
            });
        }
    };

    std::map<std::string, StreamerInstance> streams; // url -> StreamerInstance

    StreamGroup(const tao::config::value &spec) {
        if (!spec.find("dir")) throw herr("no dir field");
        dir = spec.at("dir").get_string();


        tao::json::value filter = tao::json::empty_object;
        // FIXME: Must be better way to go from config object to json, instead of round-trip through string
        if (spec.find("filter")) filter = tao::json::from_string(tao::json::to_string(spec.at("filter")));

        filterStr = tao::json::to_string(filter);
        filterCompiled = NostrFilterGroup::unwrapped(filter);


        if (!spec.find("urls")) throw herr("no urls field");
        for (const auto &url : spec.at("urls").get_array()) {
            streams.try_emplace(url.get_string(), url.get_string(), dir, filter);
        }
    }
};



void cmd_router(const std::vector<std::string> &subArgs) {
    std::map<std::string, docopt::value> args = docopt::docopt(USAGE, subArgs, true, "");

    std::string routerConfigFile = args["<routerConfigFile>"].asString();


    globalRouterWriter = std::make_unique<WriterPipeline>();
    Decompressor decomp;

    std::mutex groupMutex;
    std::map<std::string, StreamGroup> streamGroups; // group name -> StreamGroup


    // Config

    auto reconcileConfig = [&]{
        LI << "Loading router config file: " << routerConfigFile;

        try {
            auto routerConfig = loadRawTaoConfig(routerConfigFile);

            std::lock_guard<std::mutex> guard(groupMutex);

            for (const auto &[k, v] : routerConfig.at("streams").get_object()) {
                if (!streamGroups.contains(k)) {
                    LI << "New stream group [" << k << "]";
                    streamGroups.emplace(k, v);
                }
            }
        } catch (std::exception &e) {
            LE << "Failed to parse router config: " << e.what();
            return;
        }
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
        std::lock_guard<std::mutex> guard(groupMutex);

        auto txn = env.txn_ro();

        env.foreach_Event(txn, [&](auto &ev){
            currEventId = ev.primaryKeyId;

            std::string msg = std::string("[\"EVENT\",");
            msg += getEventJson(txn, decomp, ev.primaryKeyId);
            msg += "]";

            auto msgPtr = std::make_shared<std::string>(std::move(msg));

            {
                for (auto &[groupName, streamGroup] : streamGroups) {
                    if (streamGroup.dir == "down") continue;
                    if (!streamGroup.filterCompiled.doesMatch(ev.flat_nested())) continue;

                    for (auto &[url, streamer] : streamGroup.streams) {
                        streamer.es.sendEvent(msgPtr);
                        streamer.es.trigger(); // FIXME: do once at end
                    }
                }
            }

            return true;
        }, false, currEventId + 1);
    });


    pause();
}
