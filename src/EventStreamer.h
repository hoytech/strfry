#pragma once

#include <hoytech/protected_queue.h>

#include "golpe.h"

#include "WSConnection.h"


struct EventStreamer {
    std::string url;
    WSConnection ws;
    std::string dir;
    tao::json::value filter;

    std::function<void(tao::json::value &&, const WSConnection &ws)> onEvent;

  private:

    hoytech::protected_queue<std::shared_ptr<std::string>> inbox;

  public:

    EventStreamer(const std::string &url, const std::string &dir, tao::json::value filter = tao::json::empty_object) : url(url), ws(url), dir(dir), filter(filter) {
        filter["limit"] = 0;
    }

    void sendEvent(std::shared_ptr<std::string> msg) {
        inbox.push_move(std::move(msg));
    }

    void trigger() {
        ws.trigger();
    }

    void run() {
        ws.onConnect = [&]{
            if (dir == "down" || dir == "both") {
                auto encoded = tao::json::to_string(tao::json::value::array({ "REQ", "X", filter }));
                ws.send(encoded);
            }
        };

        ws.onMessage = [&](auto msg, uWS::OpCode, size_t){
            auto origJson = tao::json::from_string(msg);

            if (origJson.is_array()) {
                if (origJson.get_array().size() < 2) throw herr("array too short");

                auto &msgType = origJson.get_array().at(0);
                if (msgType == "EOSE") {
                    return;
                } else if (msgType == "NOTICE") {
                    LW << "NOTICE message from " << url << " : " << tao::json::to_string(origJson);
                    return;
                } else if (msgType == "OK") {
                    if (!origJson.get_array().at(2).get_boolean()) {
                        LW << "Event not written by " << url << " : " << origJson;
                    }
                } else if (msgType == "EVENT") {
                    if (dir == "down" || dir == "both") {
                        if (origJson.get_array().size() < 3) throw herr("array too short");
                        auto &evJson = origJson.at(2);

                        // FIXME: validate that the event actually matches provided filter?
                        if (onEvent) onEvent(std::move(evJson), ws);
                    } else {
                        LW << "Unexpected EVENT from " << url;
                    }
                } else {
                    throw herr("unexpected first element");
                }
            } else {
                throw herr("unexpected message");
            }
        };

        ws.onTrigger = [&]{
            auto msgs = inbox.pop_all();
            for (auto &msg : msgs) ws.send(*msg);
        };

        ws.run();
    }
};
