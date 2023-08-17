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



void cmd_router(const std::vector<std::string> &subArgs) {
    std::map<std::string, docopt::value> args = docopt::docopt(USAGE, subArgs, true, "");

    std::string routerConfigFile = args["<routerConfigFile>"].asString();

    tao::config::value configJson = loadRawTaoConfig(routerConfigFile);

    std::cout << configJson << std::endl;
}
