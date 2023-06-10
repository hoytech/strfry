#include "WebServer.h"
#include "WebUtils.h"
#include "Bech32Utils.h"

#include "PluginEventSifter.h"
#include "events.h"


void WebServer::runWriter(ThreadPool<MsgWebWriter>::Thread &thr) {
    secp256k1_context *secpCtx = secp256k1_context_create(SECP256K1_CONTEXT_VERIFY);
    PluginEventSifter writePolicy;

    while(1) {
        auto newMsgs = thr.inbox.pop_all();
        auto now = hoytech::curr_time_us();

        std::vector<EventToWrite> newEvents;

        for (auto &newMsg : newMsgs) {
            if (auto msg = std::get_if<MsgWebWriter::Request>(&newMsg.msg)) {
                auto &req = msg->req;
                EventSourceType sourceType = req.ipAddr.size() == 4 ? EventSourceType::IP4 : EventSourceType::IP6;

                Url u(req.url);
                if (u.path.size() != 1 || u.path[0] != "submit-post") {
                    sendHttpResponse(req, "Not found", "404 Not Found");
                    continue;
                }

                std::string flatStr, jsonStr;

                try {
                    tao::json::value json = tao::json::from_string(req.body);
                    parseAndVerifyEvent(json, secpCtx, true, true, flatStr, jsonStr);
                } catch(std::exception &e) {
                    sendHttpResponse(req, tao::json::to_string(tao::json::value({{ "message", e.what() }})), "404 Not Found", "application/json; charset=utf-8");
                    continue;
                }

                newEvents.emplace_back(std::move(flatStr), std::move(jsonStr), now, sourceType, req.ipAddr, &req);
            }
        }

        try {
            auto txn = env.txn_rw();
            writeEvents(txn, newEvents);
            txn.commit();
        } catch (std::exception &e) {
            LE << "Error writing " << newEvents.size() << " events: " << e.what();

            for (auto &newEvent : newEvents) {
                std::string message = "Write error: ";
                message += e.what();

                HTTPRequest &req = *static_cast<HTTPRequest*>(newEvent.userData);
                sendHttpResponse(req, tao::json::to_string(tao::json::value({{ "message", message }})), "500 Server Error", "application/json; charset=utf-8");
            }

            continue;
        }


        for (auto &newEvent : newEvents) {
            auto *flat = flatbuffers::GetRoot<NostrIndex::Event>(newEvent.flatStr.data());
            auto eventIdHex = to_hex(sv(flat->id()));

            tao::json::value output = tao::json::empty_object;
            std::string message;

            if (newEvent.status == EventWriteStatus::Written) {
                LI << "Inserted event. id=" << eventIdHex << " levId=" << newEvent.levId;
                output["message"] = message = "ok";
                output["written"] = true;
                output["event"] = encodeBech32Simple("note", sv(flat->id()));
            } else if (newEvent.status == EventWriteStatus::Duplicate) {
                output["message"] = message = "duplicate: have this event";
                output["written"] = true;
            } else if (newEvent.status == EventWriteStatus::Replaced) {
                output["message"] = message = "replaced: have newer event";
            } else if (newEvent.status == EventWriteStatus::Deleted) {
                output["message"] = message = "deleted: user requested deletion";
            }

            if (newEvent.status != EventWriteStatus::Written) {
                LI << "Rejected event. " << message << ", id=" << eventIdHex;
            }

            HTTPRequest &req = *static_cast<HTTPRequest*>(newEvent.userData);
            sendHttpResponse(req, tao::json::to_string(output), "200 OK", "application/json; charset=utf-8");
        }
    }
}
