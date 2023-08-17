#include <docopt.h>
#include <tao/json.hpp>
#include <hoytech/file_change_monitor.h>

#include "golpe.h"

#include "EventStreamer.h"
#include "WriterPipeline.h"
#include "PluginWritePolicy.h"
#include "events.h"


static const char USAGE[] =
R"(
    Usage:
      router <routerConfigFile>

    Options:
)";



struct StreamGroup : NonCopyable {
    std::string dir;
    tao::json::value filter;
    std::optional<PluginWritePolicy> pluginDown;
    std::optional<PluginWritePolicy> pluginUp;

    struct StreamerInstance : NonCopyable {
        EventStreamer es;
        std::thread t;

        StreamerInstance(const std::string &url, const std::string &dir, tao::json::value filter = tao::json::empty_object) : es(url, dir, filter) {
            es.onEvent = [&](tao::json::value &&evJson, const WSConnection &ws) {
                LI << "GOT EVENT FROM " << es.url << " : " << evJson;
            };

            t = std::thread([this]{
                es.run();
            });
        }
    };

    std::map<std::string, StreamerInstance> streams; // url -> StreamerInstance

    StreamGroup(const tao::config::value &spec) {
        dir = spec.at("dir").get_string();

        for (const auto &url : spec.at("urls").get_array()) {
            streams.try_emplace(url.get_string(), url.get_string(), dir);
        }
    }
};



void cmd_router(const std::vector<std::string> &subArgs) {
    std::map<std::string, docopt::value> args = docopt::docopt(USAGE, subArgs, true, "");

    std::string routerConfigFile = args["<routerConfigFile>"].asString();


    WriterPipeline writer;
    Decompressor decomp;


    std::mutex groupMutex;
    std::map<std::string, StreamGroup> streamGroups; // group name -> StreamGroup


    auto reconcileConfig = [&](const tao::config::value &routerConfig){
        std::lock_guard<std::mutex> guard(groupMutex);

        for (const auto &[k, v] : routerConfig.at("streams").get_object()) {
            if (!streamGroups.contains(k)) {
                LI << "New stream group [" << k << "]";
                streamGroups.emplace(k, v);
            }
        }
    };

    reconcileConfig(loadRawTaoConfig(routerConfigFile));

    pause();
}
