#include "RelayServer.h"


void RelayServer::runWriter(ThreadPool<MsgWriter>::Thread &thr) {
    quadrable::Quadrable qdb;
    {
        auto txn = env.txn_ro();
        qdb.init(txn);
    }
    qdb.checkout("events");

    while(1) {
        auto newMsgs = thr.inbox.pop_all();

        // Prepare messages

        std::deque<EventToWrite> newEvents;

        for (auto &newMsg : newMsgs) {
            if (auto msg = std::get_if<MsgWriter::AddEvent>(&newMsg.msg)) {
                newEvents.emplace_back(std::move(msg->flatStr), std::move(msg->jsonStr), msg->receivedAt, msg);
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
                LI << "Inserted event. id=" << eventIdHex << " qdbNodeId=" << newEvent.nodeId;
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
