#include <thread>
#include <chrono>

#include <uWebSockets/src/uWS.h>

#include "golpe.h"


class WSConnection : NonCopyable {
    std::string url;

    uWS::Hub hub;
    uWS::Group<uWS::CLIENT> *hubGroup = nullptr;
    uS::Async *hubTrigger = nullptr;

    uWS::WebSocket<uWS::CLIENT> *currWs = nullptr;
    uS::Timer *reconnectTimer = nullptr; // pending non-blocking reconnect scheduled by scheduleReconnect()


  public:

    struct Stats {
        uint64_t bytesUp = 0;
        uint64_t bytesUpCompressed = 0;
        uint64_t bytesDown = 0;
        uint64_t bytesDownCompressed = 0;
    } stats;

    WSConnection(const std::string &url) : url(url) {}

    std::function<void()> onConnect;
    std::function<void(std::string_view, uWS::OpCode, size_t)> onMessage;
    std::function<void()> onTrigger;
    std::function<void()> onDisconnect;
    std::function<void()> onError;
    bool reconnect = true;
    std::atomic<bool> shutdown = false;
    uint64_t reconnectDelayMilliseconds = 5'000;
    std::string remoteAddr;

    ~WSConnection() {
        if (hubGroup || hubTrigger || currWs || reconnectTimer) LW << "WSConnection destroyed before close";
    }

    void close() {
        shutdown = true;
        trigger();
    }

    // Should only be called from the websocket thread (ie within an onConnect or onMessage callback)
    void send(std::string_view msg, uWS::OpCode op = uWS::OpCode::TEXT, size_t *compressedSize = nullptr) {
        if (currWs) {
            size_t localCompressedSize;
            size_t *csPtr = compressedSize ? compressedSize : &localCompressedSize;
            currWs->send(msg.data(), msg.size(), op, nullptr, nullptr, true, csPtr);
            stats.bytesUp += msg.size();
            stats.bytesUpCompressed += *csPtr;
        } else {
            LI << "Tried to send message, but websocket is disconnected";
        }
    }

    // Can be called from any thread, invokes onTrigger in websocket thread context
    void trigger() {
        if (hubTrigger) hubTrigger->send();
    }

    // Perform the actual connection attempt. Must run on the websocket (hub) thread.
    void connectNow() {
        if (shutdown) return;
        LI << "Attempting to connect to " << url;
        hub.connect(url, nullptr, {}, 5000, hubGroup);
    }

    // Schedule a reconnect after `delay` ms WITHOUT blocking the event loop.
    // Previously doConnect() did std::this_thread::sleep_for(delay), which blocks the
    // hub thread that also runs the uv loop. That stalls the loop's cached clock by
    // `delay` ms; the subsequent hub.connect() then arms its connection-timeout timer
    // against the stale clock, so the timer is already expired and fires immediately on
    // the next loop tick, tearing the freshly-connected socket down before the WS
    // handshake completes. The very first connect used doConnect(0) (no sleep) so it
    // worked, but every reconnect after a disconnect wedged permanently (the process
    // keeps retrying and failing forever, never exiting, so a supervisor never restarts
    // it). Deferring via a non-blocking uS::Timer keeps the loop clock accurate.
    void scheduleReconnect(uint64_t delay) {
        if (shutdown) return;
        if (reconnectTimer) return; // a reconnect is already pending; don't stack another timer
        reconnectTimer = new uS::Timer(hub.getLoop());
        reconnectTimer->setData(this);
        reconnectTimer->start([](uS::Timer *t){
            WSConnection *self = (WSConnection *) t->getData();
            self->reconnectTimer = nullptr;
            t->stop();
            t->close();
            self->connectNow();
        }, delay, 0);
    }

    void run() {
        hubGroup = hub.createGroup<uWS::CLIENT>(uWS::PERMESSAGE_DEFLATE | uWS::SLIDING_DEFLATE_WINDOW);

        auto doConnect = [&](uint64_t delay = 0){
            if (shutdown) return;
            if (delay) scheduleReconnect(delay);
            else connectNow();
        };


        hubGroup->onConnection([&](uWS::WebSocket<uWS::CLIENT> *ws, uWS::HttpRequest req) {
            if (shutdown) return;

            if (currWs) {
                currWs->terminate();
                currWs = nullptr;
            }

            stats = Stats{};
            remoteAddr = ws->getAddress().address;
            LI << "Connected to " << url << " (" << remoteAddr << ")";

            {
                int optval = 1;
                if (setsockopt(ws->getFd(), SOL_SOCKET, SO_KEEPALIVE, &optval, sizeof(optval))) {
                    LW << "Failed to enable TCP keepalive: " << strerror(errno);
                }
            }

            currWs = ws;

            if (!onConnect) return;
            try {
                onConnect();
            } catch (std::exception &e) {
                LW << "onConnect failure: " << e.what();
            }
        });

        hubGroup->onDisconnection([&](uWS::WebSocket<uWS::CLIENT> *ws, int code, char *message, size_t length) {
            auto upComp = renderPercent(stats.bytesUp ? 1.0 - (double)stats.bytesUpCompressed / stats.bytesUp : 0);
            auto downComp = renderPercent(stats.bytesDown ? 1.0 - (double)stats.bytesDownCompressed / stats.bytesDown : 0);

            LI << "Disconnected from " << url << " : " << code << "/" << (message ? std::string_view(message, length) : "-")
               << " UP: " << renderSize(stats.bytesUp) << " (" << upComp << " compressed)"
               << " DN: " << renderSize(stats.bytesDown) << " (" << downComp << " compressed)";

            if (shutdown) return;

            if (ws == currWs) {
                currWs = nullptr;

                if (onDisconnect) onDisconnect();
                if (reconnect) doConnect(reconnectDelayMilliseconds);
            } else {
                LI << "Got disconnect for unexpected connection?";
            }
        });

        hubGroup->onError([&](void *) {
            LI << "Websocket connection error";

            if (onError) onError();
            if (reconnect) doConnect(reconnectDelayMilliseconds);
        });

        hubGroup->onMessage2([&](uWS::WebSocket<uWS::CLIENT> *ws, char *message, size_t length, uWS::OpCode opCode, size_t compressedSize) {
            stats.bytesDown += length;
            stats.bytesDownCompressed += compressedSize;

            if (!onMessage) return;

            try {
                onMessage(std::string_view(message, length), opCode, compressedSize);
            } catch (std::exception &e) {
                LW << "onMessage failure: " << e.what();
            }
        });

        std::function<void()> asyncCb = [&]{
            if (shutdown) {
                terminate();
                return;
            }

            if (!onTrigger) return;

            try {
                onTrigger();
            } catch (std::exception &e) {
                LW << "onTrigger failure: " << e.what();
            }
        };

        hubTrigger = new uS::Async(hub.getLoop());
        hubTrigger->setData(&asyncCb);

        hubTrigger->start([](uS::Async *a){
            auto *r = static_cast<std::function<void()> *>(a->getData());
            (*r)();
        });


        doConnect();

        hub.run();
    }


  private:

    void terminate() {
        if (hubGroup) {
            hubGroup->close();
            hubGroup = nullptr;
        }

        if (hubTrigger) {
            hubTrigger->close();
            hubTrigger = nullptr;
        }

        if (currWs) {
            currWs->terminate();
            currWs = nullptr;
        }

        if (reconnectTimer) {
            reconnectTimer->stop();
            reconnectTimer->close();
            reconnectTimer = nullptr;
        }
    }
};
