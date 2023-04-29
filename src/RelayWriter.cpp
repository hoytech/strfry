#include "RelayServer.h"

#include "PluginWritePolicy.h"


void RelayServer::runWriter(ThreadPool<MsgWriter>::Thread &thr) {
    auto qdb = getQdbInstance();

    PluginWritePolicy writePolicy;

    while(1) {
        auto newMsgs = thr.inbox.pop_all();

        // Prepare messages

        std::vector<EventToWrite> newEvents;

        for (auto &newMsg : newMsgs) {
            if (auto msg = std::get_if<MsgWriter::AddEvent>(&newMsg.msg)) {
                tao::json::value evJson = tao::json::from_string(msg->jsonStr);
                EventSourceType sourceType = msg->ipAddr.size() == 4 ? EventSourceType::IP4 : EventSourceType::IP6;
                std::string okMsg;
                auto res = writePolicy.acceptEvent(evJson, msg->receivedAt, sourceType, msg->ipAddr, okMsg);

                if (res == WritePolicyResult::Accept) {
                    newEvents.emplace_back(std::move(msg->flatStr), std::move(msg->jsonStr), msg->receivedAt, sourceType, std::move(msg->ipAddr), msg);
                } else {
                    auto *flat = flatbuffers::GetRoot<NostrIndex::Event>(msg->flatStr.data());
                    auto eventIdHex = to_hex(sv(flat->id()));

                    LI << "[" << msg->connId << "] write policy blocked event " << eventIdHex << ": " << okMsg;

                    sendOKResponse(msg->connId, eventIdHex, res == WritePolicyResult::ShadowReject, okMsg);
                }
            }
        }

        {
            auto txn = env.txn_rw();
            writeEvents(txn, qdb, newEvents);
            txn.commit();
        }

        // Log

        for (auto &newEvent : newEvents) {
            auto *flat = flatbuffers::GetRoot<NostrIndex::Event>(newEvent.flatStr.data());
            auto eventIdHex = to_hex(sv(flat->id()));
            std::string message;
            bool written = false;

            if (newEvent.status == EventWriteStatus::Written) {
                LI << "Inserted event. id=" << eventIdHex << " levId=" << newEvent.levId;
                written = true;
            } else if (newEvent.status == EventWriteStatus::Duplicate) {
                message = "duplicate: have this event";
                written = true;
            } else if (newEvent.status == EventWriteStatus::Replaced) {
                message = "replaced: have newer event";
            } else if (newEvent.status == EventWriteStatus::Deleted) {
                message = "deleted: user requested deletion";
            }

            if (newEvent.status != EventWriteStatus::Written) {
                LI << "Rejected event. " << message << ", id=" << eventIdHex;
            }

            MsgWriter::AddEvent *addEventMsg = static_cast<MsgWriter::AddEvent*>(newEvent.userData);

            sendOKResponse(addEventMsg->connId, eventIdHex, written, message);
        }
    }
}
