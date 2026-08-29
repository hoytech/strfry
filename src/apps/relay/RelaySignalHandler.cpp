#ifndef _WIN32
#include <signal.h>
#endif

#include "RelayServer.h"


void RelayServer::runSignalHandler() {
    setThreadName("signalHandler");

#ifndef _WIN32
    sigset_t sigset;
    sigemptyset(&sigset);
    sigaddset(&sigset, SIGUSR1);

    while (1) {
        int sig;
        int s = sigwait(&sigset, &sig);
        if (s != 0) throw herr("unable to sigwait: ", strerror(errno));

        if (sig == SIGUSR1) {
            tpWebsocket.dispatch(0, MsgWebsocket{MsgWebsocket::GracefulShutdown{}});
            if (hubTrigger) hubTrigger->send();
        } else {
            LW << "Got unexpected signal: " << sig;
        }
    }
#else
    while (1) {
        std::this_thread::sleep_for(std::chrono::seconds(1000));
    }
#endif
}
