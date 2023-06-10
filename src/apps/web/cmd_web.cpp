#include "WebServer.h"



void cmd_web(const std::vector<std::string> &subArgs) {
    WebServer s;
    s.run();
}

void WebServer::run() {
    tpHttpsocket.init("Httpsocket", 1, [this](auto &thr){
        runHttpsocket(thr);
    });


    // FIXME: cfg().web__numThreads__*

    tpReader.init("Reader", 3, [this](auto &thr){
        runReader(thr);
    });

    tpWriter.init("Writer", 1, [this](auto &thr){
        runWriter(thr);
    });


    // Monitor for config file reloads

    auto configFileChangeWatcher = hoytech::file_change_monitor(configFile);

    configFileChangeWatcher.setDebounce(100);

    configFileChangeWatcher.run([&](){
        loadConfig(configFile);
    });


    tpHttpsocket.join();
}
