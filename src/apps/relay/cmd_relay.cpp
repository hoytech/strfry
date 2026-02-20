#include <pthread.h>
#include <signal.h>

#include "RelayServer.h"



static void checkConfig() {
    if (cfg().relay__info__pubkey.size()) {
        try {
            auto p = from_hex(cfg().relay__info__pubkey);
            if (p.size() != 32) throw herr("bad size");
        } catch (std::exception &e) {
            LW << "Your relay.info.pubkey is incorrectly formatted. It should be 64 hex digits.";
        }
    }

    if (cfg().events__rejectEphemeralEventsOlderThanSeconds >= cfg().events__ephemeralEventsLifetimeSeconds) {
        LW << "rejectEphemeralEventsOlderThanSeconds is >= ephemeralEventsLifetimeSeconds, which could result in unnecessary disk activity";
    }

    if (cfg().relay__auth__enabled) {
        if (cfg().relay__auth__relayUrl.empty()) {
            LW << "relay.auth.enabled is true but relay.auth.relayUrl is empty. AUTH relay tag will not be verified.";
        }
        if (cfg().relay__auth__required) {
            LI << "NIP-42 authentication is REQUIRED for all operations";
        } else {
            LI << "NIP-42 authentication is enabled but optional";
        }
        if (cfg().relay__auth__sessionTokenEnabled) {
            LI << "Session tokens enabled (lifetime: " << cfg().relay__auth__sessionTokenLifetimeSeconds << "s)";
        }
    }
}


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

    if (cfg().relay__auth__enabled && cfg().relay__auth__sessionTokenEnabled) {
        sessionSecret = SessionToken::generateSecret();
        LI << "Generated session token secret (valid for this process lifetime)";
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

    checkConfig();

    auto configFileChangeWatcher = hoytech::file_change_monitor(configFile);

    configFileChangeWatcher.setDebounce(100);

    configFileChangeWatcher.run([&](){
        loadConfig(configFile);
        checkConfig();
    });


    tpWebsocket.join();
}
