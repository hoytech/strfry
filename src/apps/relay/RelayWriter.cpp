#include "RelayServer.h"

#include "PluginEventSifter.h"
#include "PrometheusMetrics.h"


void RelayServer::runWriter(ThreadPool<MsgWriter>::Thread &thr) {
    PluginEventSifter writePolicyPlugin;
    NegentropyFilterCache neFilterCache;

    while(1) {
        auto newMsgs = thr.inbox.pop_all();

        // Filter out messages from already closed sockets

        {
            flat_hash_set<uint64_t> closedConns;

            for (auto &newMsg : newMsgs) {
                if (auto msg = std::get_if<MsgWriter::CloseConn>(&newMsg.msg)) closedConns.insert(msg->connId);
            }

            if (closedConns.size()) {
                decltype(newMsgs) newMsgsFiltered;

                for (auto &newMsg : newMsgs) {
                    if (auto msg = std::get_if<MsgWriter::AddEvent>(&newMsg.msg)) {
                        if (!closedConns.contains(msg->connId)) newMsgsFiltered.emplace_back(std::move(newMsg));
                    }
                }

                std::swap(newMsgs, newMsgsFiltered);
            }
        }

        // Prepare messages

        std::vector<EventToWrite> newEvents;

        for (auto &newMsg : newMsgs) {
            if (auto msg = std::get_if<MsgWriter::AddEvent>(&newMsg.msg)) {
                EventSourceType sourceType = msg->ipAddr.size() == 4 ? EventSourceType::IP4 : EventSourceType::IP6;
                std::string okMsg;
                const std::string &plugin = cfg().relay__writePolicy__plugin;

                auto res = writePolicyPlugin.acceptEvent(plugin, plugin.empty() ? tao::json::empty_object : tao::json::from_string(msg->jsonStr), sourceType, msg->ipAddr, msg->authed, okMsg);

                if (res == PluginEventSifterResult::Accept) {
                    newEvents.emplace_back(std::move(msg->packedStr), std::move(msg->jsonStr), msg);
                } else {
                    PackedEventView packed(msg->packedStr);
                    auto eventIdHex = to_hex(packed.id());

                    if (okMsg.size()) LI << "[" << msg->connId << "] write policy blocked event " << eventIdHex << ": " << okMsg;

                    sendOKResponse(msg->connId, eventIdHex, res == PluginEventSifterResult::ShadowReject, okMsg);
                }
            }
        }

        if (!newEvents.size()) continue;

        // Do write

        try {
            auto t0 = std::chrono::steady_clock::now();
            auto txn = env.txn_rw();
            writeEvents(txn, neFilterCache, newEvents);
            txn.commit();
            auto t1 = std::chrono::steady_clock::now();
            auto us = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
            PrometheusMetrics::getInstance().writeTimeUs.inc(us);
            PrometheusMetrics::getInstance().lastWriteBatchSize.set(newEvents.size());
        } catch (std::exception &e) {
            LE << "Error writing " << newEvents.size() << " events: " << e.what();

            for (auto &newEvent : newEvents) {
                PackedEventView packed(newEvent.packedStr);
                auto eventIdHex = to_hex(packed.id());
                MsgWriter::AddEvent *addEventMsg = static_cast<MsgWriter::AddEvent*>(newEvent.userData);

                std::string message = "Write error: ";
                message += e.what();

                sendOKResponse(addEventMsg->connId, eventIdHex, false, message);
            }

            continue;
        }

        // Log

        for (auto &newEvent : newEvents) {
            PackedEventView packed(newEvent.packedStr);
            auto eventIdHex = to_hex(packed.id());
            std::string message;
            bool written = false;

            if (newEvent.status == EventWriteStatus::Written) {
                LI << "Inserted event. id=" << eventIdHex << " levId=" << newEvent.levId;
                written = true;
                PrometheusMetrics::getInstance().writtenEventsTotal.inc();
                PROM_INC_EVENT_KIND(std::to_string(packed.kind()));
            } else if (newEvent.status == EventWriteStatus::Duplicate) {
                message = "duplicate: have this event";
                written = true;
                PrometheusMetrics::getInstance().dupEventsTotal.inc();
            } else if (newEvent.status == EventWriteStatus::Replaced) {
                message = "replaced: have newer event";
                PrometheusMetrics::getInstance().rejectedEventsTotal.inc();
            } else if (newEvent.status == EventWriteStatus::Deleted) {
                message = "deleted: user requested deletion";
                PrometheusMetrics::getInstance().rejectedEventsTotal.inc();
            }

            if (newEvent.status != EventWriteStatus::Written) {
                LI << "Rejected event. " << message << ", id=" << eventIdHex;
            }

            MsgWriter::AddEvent *addEventMsg = static_cast<MsgWriter::AddEvent*>(newEvent.userData);

            sendOKResponse(addEventMsg->connId, eventIdHex, written, message);
        }
    }
}
