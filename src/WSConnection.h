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


  public:

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
        if (hubGroup || hubTrigger || currWs) LW << "WSConnection destroyed before close";
    }

    void close() {
        shutdown = true;
        trigger();
    }

    // Should only be called from the websocket thread (ie within an onConnect or onMessage callback)
    void send(std::string_view msg, uWS::OpCode op = uWS::OpCode::TEXT, size_t *compressedSize = nullptr) {
        if (currWs) {
            currWs->send(msg.data(), msg.size(), op, nullptr, nullptr, true, compressedSize);
        } else {
            LI << "Tried to send message, but websocket is disconnected";
        }
    }

    // Can be called from any thread, invokes onTrigger in websocket thread context
    void trigger() {
        if (hubTrigger) hubTrigger->send();
    }

    void run() {
        hubGroup = hub.createGroup<uWS::CLIENT>(uWS::PERMESSAGE_DEFLATE | uWS::SLIDING_DEFLATE_WINDOW);

        auto doConnect = [&](uint64_t delay = 0){
            if (delay) std::this_thread::sleep_for(std::chrono::milliseconds(delay));
            if (shutdown) return;
            LI << "Attempting to connect to " << url;
            hub.connect(url, nullptr, {}, 5000, hubGroup);
        };


        hubGroup->onConnection([&](uWS::WebSocket<uWS::CLIENT> *ws, uWS::HttpRequest req) {
            if (shutdown) return;

            if (currWs) {
                currWs->terminate();
                currWs = nullptr;
            }

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
            LI << "Disconnected from " << url << " : " << code << "/" << (message ? std::string_view(message, length) : "-");

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
    }
};
