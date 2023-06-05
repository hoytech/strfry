#include <signal.h>

#include "RelayServer.h"


void RelayServer::runSignalHandler() {
    setThreadName("signalHandler");

    sigset_t sigset;
    sigemptyset(&sigset);
    sigaddset(&sigset, SIGUSR1);

    while (1) {
        int sig;
        int s = sigwait(&sigset, &sig);
        if (s != 0) throw herr("unable to sigwait: ", strerror(errno));

        if (sig == SIGUSR1) {
            tpWebsocket.dispatch(0, MsgWebsocket{MsgWebsocket::GracefulShutdown{}});
            hubTrigger->send();
        } else {
            LW << "Got unexpected signal: " << sig;
        }
    }
}
