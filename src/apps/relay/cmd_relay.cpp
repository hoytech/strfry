#include <pthread.h>
#include <signal.h>

#include "RelayServer.h"



void cmd_relay(const std::vector<std::string> &subArgs) {
    RelayServer s;
    s.run();
}

void RelayServer::run() {
    {
        sigset_t set;
        sigemptyset(&set);
        sigaddset(&set, SIGUSR1);
        int s = pthread_sigmask(SIG_BLOCK, &set, NULL);
        if (s != 0) throw herr("Unable to set sigmask: ", strerror(errno));
    }

    tpWebsocket.init("Websocket", 1, [this](auto &thr){
        runWebsocket(thr);
    });

    tpIngester.init("Ingester", cfg().relay__numThreads__ingester, [this](auto &thr){
        runIngester(thr);
    });

    tpWriter.init("Writer", 1, [this](auto &thr){
        runWriter(thr);
    });

    tpReqWorker.init("ReqWorker", cfg().relay__numThreads__reqWorker, [this](auto &thr){
        runReqWorker(thr);
    });

    tpReqMonitor.init("ReqMonitor", cfg().relay__numThreads__reqMonitor, [this](auto &thr){
        runReqMonitor(thr);
    });

    tpNegentropy.init("Negentropy", cfg().relay__numThreads__negentropy, [this](auto &thr){
        runNegentropy(thr);
    });

    cronThread = std::thread([this]{
        runCron();
    });

    signalHandlerThread = std::thread([this]{
        runSignalHandler();
    });

    // Monitor for config file reloads

    auto configFileChangeWatcher = hoytech::file_change_monitor(configFile);

    configFileChangeWatcher.setDebounce(100);

    configFileChangeWatcher.run([&](){
        loadConfig(configFile);
    });


    tpWebsocket.join();
}
