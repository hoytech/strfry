#include "RelayServer.h"


std::string preGenerateHttpResponse(const std::string &contentType, const std::string &content) {
    std::string output = "HTTP/1.1 200 OK\r\n";
    output += std::string("Content-Type: ") + contentType + "\r\n";
    output += "Access-Control-Allow-Origin: *\r\n";
    output += "Connection: keep-alive\r\n";
    output += "Server: strfry\r\n";
    output += std::string("Content-Length: ") + std::to_string(content.size()) + "\r\n";
    output += "\r\n";
    output += content;
    return output;
};


void RelayServer::runWebsocket(ThreadPool<MsgWebsocket>::Thread &thr) {
    struct Connection {
        uWS::WebSocket<uWS::SERVER> *websocket;
        uint64_t connId;
        uint64_t connectedTimestamp;
        std::string ipAddr;

        Connection(uWS::WebSocket<uWS::SERVER> *p, uint64_t connId_)
            : websocket(p), connId(connId_), connectedTimestamp(hoytech::curr_time_us()) { }
        Connection(const Connection &) = delete;
        Connection(Connection &&) = delete;
    };

    uWS::Hub hub;
    uWS::Group<uWS::SERVER> *hubGroup;
    std::map<uint64_t, Connection*> connIdToConnection;
    uint64_t nextConnectionId = 1;

    std::string tempBuf;
    tempBuf.reserve(cfg().events__maxEventSize + MAX_SUBID_SIZE + 100);



    auto getServerInfoHttpResponse = [ver = uint64_t(0), rendered = std::string("")]() mutable {
        if (ver != cfg().version()) {
            rendered = preGenerateHttpResponse("application/json", tao::json::to_string(tao::json::value({
                { "name", cfg().relay__info__name },
                { "description", cfg().relay__info__description },
                { "pubkey", cfg().relay__info__pubkey },
                { "contact", cfg().relay__info__contact },
                { "supported_nips", tao::json::value::array({ 1, 9, 11, 12, 15, 16, 20, 22 }) },
                { "software", "git+https://github.com/hoytech/strfry.git" },
                { "version", GOLPE_GIT_VER },
            })));
            ver = cfg().version();
        }

        return std::string_view(rendered);
    };

    const std::string defaultHttpResponse = preGenerateHttpResponse("text/plain", "Please use a Nostr client to connect.");



    hubGroup = hub.createGroup<uWS::SERVER>(uWS::PERMESSAGE_DEFLATE | uWS::SLIDING_DEFLATE_WINDOW, cfg().relay__maxWebsocketPayloadSize);

    hubGroup->onHttpRequest([&](uWS::HttpResponse *res, uWS::HttpRequest req, char *data, size_t length, size_t remainingBytes){
        LI << "HTTP request for [" << req.getUrl().toString() << "]";

        if (req.getHeader("accept").toString() == "application/nostr+json") {
            auto info = getServerInfoHttpResponse();
            res->write(info.data(), info.size());
        } else {
            res->write(defaultHttpResponse.data(), defaultHttpResponse.size());
        }
    });

    hubGroup->onConnection([&](uWS::WebSocket<uWS::SERVER> *ws, uWS::HttpRequest req) {
        std::string addr = ws->getAddress().address;
        uint64_t connId = nextConnectionId++;
        LI << "[" << connId << "] Connect from " << addr;
        Connection *c = new Connection(ws, connId);
        c->ipAddr = addr;
        ws->setUserData((void*)c);
        connIdToConnection.emplace(connId, c);

        {
            int optval = 1;
            if (setsockopt(ws->getFd(), SOL_SOCKET, SO_KEEPALIVE, &optval, sizeof(optval))) {
                LW << "Failed to enable TCP keepalive: " << strerror(errno);
            }
        }
    });

    hubGroup->onDisconnection([&](uWS::WebSocket<uWS::SERVER> *ws, int code, char *message, size_t length) {
        Connection *c = (Connection*)ws->getUserData();
        uint64_t connId = c->connId;

        LI << "[" << connId << "] Disconnect from " << c->ipAddr;

        tpIngester.dispatch(connId, MsgIngester{MsgIngester::CloseConn{connId}});

        connIdToConnection.erase(connId);
        delete c;
    });

    hubGroup->onMessage2([&](uWS::WebSocket<uWS::SERVER> *ws, char *message, size_t length, uWS::OpCode opCode, size_t compressedSize) {
        //LI << "Decompression: " << compressedSize << " -> " << length;

        auto &c = *(Connection*)ws->getUserData();

        tpIngester.dispatch(c.connId, MsgIngester{MsgIngester::ClientMessage{c.connId, std::string(message, length)}});
    });


    std::function<void()> asyncCb = [&]{
        auto newMsgs = thr.inbox.pop_all_no_wait();

        auto doSend = [&](uint64_t connId, std::string_view payload, uWS::OpCode opCode){
            auto it = connIdToConnection.find(connId);
            if (it == connIdToConnection.end()) return;
            auto &c = *it->second;

            size_t compressedSize;
            auto cb = [](uWS::WebSocket<uWS::SERVER> *webSocket, void *data, bool cancelled, void *reserved){};
            c.websocket->send(payload.data(), payload.size(), opCode, cb, nullptr, true, &compressedSize);
            //LI << "Compression: " << payload.size() << " -> " << compressedSize;
        };

        for (auto &newMsg : newMsgs) {
            if (auto msg = std::get_if<MsgWebsocket::Send>(&newMsg.msg)) {
                doSend(msg->connId, msg->payload, uWS::OpCode::TEXT);
            } else if (auto msg = std::get_if<MsgWebsocket::SendBinary>(&newMsg.msg)) {
                doSend(msg->connId, msg->payload, uWS::OpCode::BINARY);
            } else if (auto msg = std::get_if<MsgWebsocket::SendEventToBatch>(&newMsg.msg)) {
                for (auto &item : msg->list) {
                    tempBuf.clear();
                    tempBuf += "[\"EVENT\",\"";
                    tempBuf += item.subId.sv();
                    tempBuf += "\",";
                    tempBuf += msg->evJson;
                    tempBuf += "]";

                    auto it = connIdToConnection.find(item.connId);
                    if (it == connIdToConnection.end()) continue;
                    auto &c = *it->second;

                    size_t compressedSize;
                    auto cb = [](uWS::WebSocket<uWS::SERVER> *webSocket, void *data, bool cancelled, void *reserved){};
                    c.websocket->send(tempBuf.data(), tempBuf.size(), uWS::OpCode::TEXT, cb, nullptr, true, &compressedSize);
                    //LI << "Compression: " << msg->payload.size() << " -> " << compressedSize;
                }
            }
        }
    };

    hubTrigger = std::make_unique<uS::Async>(hub.getLoop());
    hubTrigger->setData(&asyncCb);

    hubTrigger->start([](uS::Async *a){
        auto *r = static_cast<std::function<void()> *>(a->data);
        (*r)();
    });



    int port = cfg().relay__port;

    std::string bindHost = cfg().relay__bind;

    if (!hub.listen(bindHost.c_str(), port, nullptr, uS::REUSE_PORT, hubGroup)) throw herr("unable to listen on port ", port);

    LI << "Started websocket server on " << bindHost << ":" << port;

    hub.run();
}
