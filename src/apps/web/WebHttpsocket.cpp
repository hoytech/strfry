#include <queue>

#include "WebServer.h"

#include "app_git_version.h"



void WebServer::runHttpsocket(ThreadPool<MsgHttpsocket>::Thread &thr) {
    uWS::Hub hub;
    uWS::Group<uWS::SERVER> *hubGroup;
    flat_hash_map<uint64_t, Connection*> connIdToConnection;
    uint64_t nextConnectionId = 1;
    uint64_t requestCounter = 0;

    flat_hash_map<uWS::HttpResponse *, HTTPRequest> receivingRequests;


    {
        int extensionOptions = 0;

        hubGroup = hub.createGroup<uWS::SERVER>(extensionOptions);
    }


    hubGroup->onHttpConnection([&](uWS::HttpSocket<uWS::SERVER> *hs) {
        uint64_t connId = nextConnectionId++;
        Connection *c = new Connection(hs, connId);

        hs->setUserData((void*)c);
        connIdToConnection.emplace(connId, c);
    });

    hubGroup->onHttpDisconnection([&](uWS::HttpSocket<uWS::SERVER> *hs) {
        auto *c = (Connection*)hs->getUserData();

        connIdToConnection.erase(c->connId);
        delete c;
    });

    hubGroup->onHttpRequest([&](uWS::HttpResponse *res, uWS::HttpRequest reqRaw, char *data, size_t length, size_t remainingBytes){
        auto *c = (Connection*)res->httpSocket->getUserData();

        HTTPRequest req(c->connId, res, reqRaw);
        req.body = std::string(data, length);

        c->pendingRequests.insert(res);

        if (req.method == uWS::HttpMethod::METHOD_GET) {
            auto m = MsgWebReader{MsgWebReader::Request{std::move(req)}};
            tpReader.dispatch(requestCounter++, std::move(m));
        } else if (req.method == uWS::HttpMethod::METHOD_POST) {
            if (remainingBytes) {
                receivingRequests.emplace(res, std::move(req));
            } else {
                tpWriter.dispatch(0, MsgWebWriter{MsgWebWriter::Request{std::move(req)}});
            }
        } else {
            sendHttpResponse(req, "Method Not Allowed", "405 Method Not Allowed");
        }
    });

    hubGroup->onHttpData([&](uWS::HttpResponse *res, char *data, size_t length, size_t remainingBytes){
        auto &req = receivingRequests.at(res);

        req.body += std::string_view(data, length);

        if (remainingBytes) {
            auto m = MsgWebWriter{MsgWebWriter::Request{std::move(req)}};
            receivingRequests.erase(res);
            tpWriter.dispatch(0, std::move(m));
        }
    });


    std::function<void()> asyncCb = [&]{
        auto newMsgs = thr.inbox.pop_all_no_wait();

        for (auto &newMsg : newMsgs) {
            if (auto msg = std::get_if<MsgHttpsocket::Send>(&newMsg.msg)) {
                auto it = connIdToConnection.find(msg->connId);
                if (it == connIdToConnection.end()) continue;
                auto &c = *it->second;

                if (!c.pendingRequests.contains(msg->res)) {
                    LW << "Couldn't find request in pendingRequests set";
                    continue;
                }

                c.pendingRequests.erase(msg->res);

                msg->res->end(msg->payload.data(), msg->payload.size());
            }
        }
    };

    hubTrigger = std::make_unique<uS::Async>(hub.getLoop());
    hubTrigger->setData(&asyncCb);

    hubTrigger->start([](uS::Async *a){
        auto *r = static_cast<std::function<void()> *>(a->data);
        (*r)();
    });



    int port = cfg().web__port;

    std::string bindHost = cfg().web__bind;

    if (!hub.listen(bindHost.c_str(), port, nullptr, uS::REUSE_PORT, hubGroup)) throw herr("unable to listen on port ", port);

    LI << "Started http server on " << bindHost << ":" << port;

    hub.run();
}
