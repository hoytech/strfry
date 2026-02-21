#include "RelayServer.h"

#include "ActiveMonitors.h"



void RelayServer::runReqMonitor(ThreadPool<MsgReqMonitor>::Thread &thr) {
    auto dbChangeWatcher = hoytech::file_change_monitor(dbDir + "/data.mdb");

    dbChangeWatcher.setDebounce(100);

    dbChangeWatcher.run([&](){
        tpReqMonitor.dispatchToAll([]{ return MsgReqMonitor{MsgReqMonitor::DBChange{}}; });
    });


    Decompressor decomp;
    ActiveMonitors monitors;
    uint64_t currEventId = MAX_U64;
    auto sensitiveKinds = parseSensitiveKinds(cfg().relay__auth__sensitiveKinds);

    while (1) {
        auto newMsgs = thr.inbox.pop_all();

        auto txn = env.txn_ro();

        uint64_t latestEventId = getMostRecentLevId(txn);
        if (currEventId > latestEventId) currEventId = latestEventId;

        for (auto &newMsg : newMsgs) {
            if (auto msg = std::get_if<MsgReqMonitor::NewSub>(&newMsg.msg)) {
                auto connId = msg->sub.connId;
                auto &authedPubkey = msg->sub.authedPubkey;

                env.foreach_Event(txn, [&](auto &ev){
                    if (msg->sub.filterGroup.doesMatch(PackedEventView(ev.buf))) {
                        auto evJson = getEventJson(txn, decomp, ev.primaryKeyId);
                        if (isSensitiveEventAllowed(sensitiveKinds, evJson, authedPubkey)) {
                            sendEvent(connId, msg->sub.subId, evJson);
                        }
                    }

                    return true;
                }, false, msg->sub.latestEventId + 1);

                msg->sub.latestEventId = latestEventId;

                if (!monitors.addSub(txn, std::move(msg->sub), latestEventId)) {
                    sendNoticeError(connId, std::string("too many concurrent REQs"));
                }
            } else if (auto msg = std::get_if<MsgReqMonitor::RemoveSub>(&newMsg.msg)) {
                monitors.removeSub(msg->connId, msg->subId);
            } else if (auto msg = std::get_if<MsgReqMonitor::CloseConn>(&newMsg.msg)) {
                monitors.closeConn(msg->connId);
            } else if (std::get_if<MsgReqMonitor::DBChange>(&newMsg.msg)) {
                env.foreach_Event(txn, [&](auto &ev){
                    monitors.process(txn, ev, [&](RecipientList &&recipients, uint64_t levId){
                        if (sensitiveKinds.empty()) {
                            sendEventToBatch(std::move(recipients), std::string(getEventJson(txn, decomp, levId)));
                        } else {
                            auto evJson = std::string(getEventJson(txn, decomp, levId));
                            // For sensitive events, filter per-recipient using their subscription's authedPubkey
                            RecipientList filtered;
                            for (auto &r : recipients) {
                                auto subPubkey = monitors.getSubAuthedPubkey(r.connId, r.subId);
                                if (isSensitiveEventAllowed(sensitiveKinds, evJson, subPubkey)) {
                                    filtered.push_back(r);
                                }
                            }
                            if (filtered.size()) {
                                sendEventToBatch(std::move(filtered), std::move(evJson));
                            }
                        }
                    });
                    return true;
                }, false, currEventId + 1);

                currEventId = latestEventId;
            }
        }
    }
}
