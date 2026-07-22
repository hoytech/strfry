#include "RelayServer.h"

#include "ActiveMonitors.h"
#include "DMFilter.h"



void RelayServer::runReqMonitor(ThreadPool<MsgReqMonitor>::Thread &thr) {
    auto dbChangeWatcher = hoytech::file_change_monitor(dbDir + "/data.mdb");

    dbChangeWatcher.setDebounce(100);

    dbChangeWatcher.run([&](){
        tpReqMonitor.dispatchToAll([]{ return MsgReqMonitor{MsgReqMonitor::DBChange{}}; });
    });


    Decompressor decomp;
    ActiveMonitors monitors;
    flat_hash_map<uint64_t, Bytes32> connIdToAuthedPubkey;
    uint64_t currEventId = MAX_U64;

    while (1) {
        auto newMsgs = thr.inbox.pop_all();

        auto txn = env.txn_ro();

        uint64_t latestEventId = getMostRecentLevId(txn);
        if (currEventId > latestEventId) currEventId = latestEventId;

        for (auto &newMsg : newMsgs) {
            if (auto msg = std::get_if<MsgReqMonitor::NewSub>(&newMsg.msg)) {
                auto connId = msg->sub.connId;
                auto it = connIdToAuthedPubkey.find(connId);
                Bytes32 connAuthedPubkey = it == connIdToAuthedPubkey.end() ? Bytes32() : it->second;

                env.foreach_Event(txn, [&](auto &ev){
                    PackedEventView packed(ev.buf);
                    if (msg->sub.filterGroup.doesMatch(packed)) {
                        if (DMFilter::shouldSendToSubscriber(packed, connAuthedPubkey)) {
                            sendEvent(connId, msg->sub.subId, getEventJson(txn, decomp, ev.primaryKeyId));
                        }
                    }

                    return true;
                }, false, msg->sub.latestEventId + 1);

                msg->sub.latestEventId = latestEventId;

                if (!monitors.addSub(txn, std::move(msg->sub), latestEventId)) {
                    sendNoticeError(connId, std::string("too many concurrent REQs"));
                }
            } else if (auto msg = std::get_if<MsgReqMonitor::SetAuth>(&newMsg.msg)) {
                connIdToAuthedPubkey[msg->connId] = msg->authed;
            } else if (auto msg = std::get_if<MsgReqMonitor::RemoveSub>(&newMsg.msg)) {
                monitors.removeSub(msg->connId, msg->subId);
            } else if (auto msg = std::get_if<MsgReqMonitor::CloseConn>(&newMsg.msg)) {
                connIdToAuthedPubkey.erase(msg->connId);
                monitors.closeConn(msg->connId);
            } else if (std::get_if<MsgReqMonitor::DBChange>(&newMsg.msg)) {
                env.foreach_Event(txn, [&](auto &ev){
                    monitors.process(txn, ev, [&](RecipientList &&recipients, uint64_t levId){
                        PackedEventView packed(ev.buf);
                        if (packed.kind() == 4 || packed.kind() == 1059) {
                            RecipientList filteredRecipients;
                            for (const auto &recipient : recipients) {
                                auto it = connIdToAuthedPubkey.find(recipient.connId);
                                Bytes32 authedPubkey = it == connIdToAuthedPubkey.end() ? Bytes32() : it->second;
                                if (DMFilter::shouldSendToSubscriber(packed, authedPubkey)) {
                                    filteredRecipients.emplace_back(recipient);
                                }
                            }
                            if (!filteredRecipients.empty()) {
                                sendEventToBatch(std::move(filteredRecipients), std::string(getEventJson(txn, decomp, levId)));
                            }
                        } else {
                            sendEventToBatch(std::move(recipients), std::string(getEventJson(txn, decomp, levId)));
                        }
                    });
                    return true;
                }, false, currEventId + 1);

                currEventId = latestEventId;
            }
        }
    }
}
