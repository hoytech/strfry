#include <thread>
#include <chrono>

#include <uWebSockets/src/uWS.h>

#include "golpe.h"


class WSConnection {
    std::string url;

    uWS::Hub hub;
    uWS::Group<uWS::CLIENT> *hubGroup;
    std::unique_ptr<uS::Async> hubTrigger;

    uWS::WebSocket<uWS::CLIENT> *currWs = nullptr;


  public:

    WSConnection(const std::string &url) : url(url) {}

    std::function<void()> onConnect;
    std::function<void(std::string_view, uWS::OpCode, size_t)> onMessage;
    std::function<void()> onTrigger;
    bool reconnect = true;
    uint64_t reconnectDelayMilliseconds = 5'000;
    std::string remoteAddr;

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
            LI << "Attempting to connect to " << url;
            hub.connect(url, nullptr, {}, 5000, hubGroup);
        };


        hubGroup->onConnection([&](uWS::WebSocket<uWS::CLIENT> *ws, uWS::HttpRequest req) {
            if (currWs) {
                currWs->terminate();
                currWs = nullptr;
            }

            remoteAddr = ws->getAddress().address;
            LI << "Connected to " << remoteAddr;

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
            LI << "Disconnected";

            if (ws == currWs) {
                currWs = nullptr;

                if (!reconnect) ::exit(1);
                doConnect(reconnectDelayMilliseconds);
            } else {
                LI << "Got disconnect for unexpected connection?";
            }
        });

        hubGroup->onError([&](void *) {
            LI << "Websocket connection error";

            if (!reconnect) ::exit(1);
            doConnect(reconnectDelayMilliseconds);
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
            if (!onTrigger) return;

            try {
                onTrigger();
            } catch (std::exception &e) {
                LW << "onTrigger failure: " << e.what();
            }
        };

        hubTrigger = std::make_unique<uS::Async>(hub.getLoop());
        hubTrigger->setData(&asyncCb);

        hubTrigger->start([](uS::Async *a){
            auto *r = static_cast<std::function<void()> *>(a->data);
            (*r)();
        });


        doConnect();

        hub.run();
    }
};
