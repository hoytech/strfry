#include <pthread.h>
#include <signal.h>

#include <docopt.h>

#include "RelayServer.h"
#include "search/LmdbSearchProvider.h"


static const char USAGE[] =
R"(
    Usage:
      relay
)";



static void checkConfig() {
    if (cfg().relay__info__pubkey.size()) {
        try {
            if (!cfg().relay__info__pubkey.starts_with("npub1")) {
                auto p = from_hex(cfg().relay__info__pubkey);
                if (p.size() != 32) throw herr("bad size");
            }
        } catch (std::exception &e) {
            LW << "Your relay.info.pubkey is incorrectly formatted. It should be an npub or 64 hex digits.";
        }
    }

    if (cfg().events__rejectEphemeralEventsOlderThanSeconds >= cfg().events__ephemeralEventsLifetimeSeconds) {
        LW << "rejectEphemeralEventsOlderThanSeconds is >= ephemeralEventsLifetimeSeconds, which could result in unnecessary disk activity";
    }
}


void cmd_relay(const std::vector<std::string> &subArgs) {
    docopt::docopt(USAGE, subArgs, true, "");

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

    // Initialize search provider (NIP-50)
    searchProvider = makeSearchProvider();
    LI << "Search provider initialized: backend=" << cfg().relay__search__backend
       << " enabled=" << cfg().relay__search__enabled
       << " healthy=" << searchProvider->healthy();

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

    // Start search catch-up indexer if LMDB backend is enabled
    if (cfg().relay__search__enabled && cfg().relay__search__backend == "lmdb") {
        auto *lmdbProvider = dynamic_cast<LmdbSearchProvider*>(searchProvider.get());
        if (lmdbProvider) {
            searchIndexerRunning = true;
            searchIndexerThread = std::thread([this, lmdbProvider]{
                lmdbProvider->runCatchupIndexer(searchIndexerRunning);
            });
        }
    }

    // Monitor for config file reloads

    checkConfig();

    auto configFileChangeWatcher = hoytech::file_change_monitor(configFile);

    configFileChangeWatcher.setDebounce(100);

    configFileChangeWatcher.run([&](){
        try {
            loadConfig(configFile);
            checkConfig();
        } catch (std::exception &e) {
            LE << "Error parsing config file, continuing with previous config: " << e.what();
        }
    });


    tpWebsocket.join();

    // Shutdown search indexer thread if running
    if (searchIndexerThread.joinable()) {
        searchIndexerRunning = false;
        searchIndexerThread.join();
        LI << "Search indexer thread shutdown complete";
    }
}
