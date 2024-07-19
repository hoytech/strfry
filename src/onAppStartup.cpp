#include <sys/time.h>
#include <sys/resource.h>
#include <string.h>
#include <errno.h>

#include "golpe.h"


static void dbCheck(lmdb::txn &txn, const std::string &cmd) {
    auto dbTooOld = [&](uint64_t ver) {
        LE << "Database version too old: " << ver << ". Expected version " << CURR_DB_VERSION;
        LE << "You should 'strfry export' your events, delete (or move) the DB files, and 'strfry import' them";
        throw herr("aborting: DB too old");
    };

    auto dbTooNew = [&](uint64_t ver) {
        LE << "Database version too new: " << ver << ". Expected version " << CURR_DB_VERSION;
        LE << "You should upgrade your version of 'strfry'";
        throw herr("aborting: DB too new");
    };

    auto s = env.lookup_Meta(txn, 1);

    if (!s) {
        env.insert_Meta(txn, CURR_DB_VERSION, 1);
        return;
    }

    if (s->endianness() != 1) throw herr("DB was created on a machine with different endianness");

    if (s->dbVersion() < CURR_DB_VERSION) {
        if (cmd == "export" || cmd == "info") return;
        dbTooOld(s->dbVersion());
    }

    if (s->dbVersion() > CURR_DB_VERSION) {
        dbTooNew(s->dbVersion());
    }
}

static void setRLimits() {
    if (!cfg().relay__nofiles) return;
    struct rlimit curr;

    if (getrlimit(RLIMIT_NOFILE, &curr)) throw herr("couldn't call getrlimit: ", strerror(errno));

#ifdef __FreeBSD__
    LI << "getrlimit NOFILES limit current " <<  curr.rlim_cur << " with max of " <<  curr.rlim_max;
    if (cfg().relay__nofiles > curr.rlim_max) {
        LI << "Unable to set NOFILES limit to " << cfg().relay__nofiles << ", exceeds max of " << curr.rlim_max;
        if (curr.rlim_cur < curr.rlim_max) {
            LI << "Setting NOFILES limit to max of " << curr.rlim_max;
            curr.rlim_cur = curr.rlim_max;
        }
    }
    else curr.rlim_cur = cfg().relay__nofiles;
    LI << "setrlimit NOFILES limit to " <<  curr.rlim_cur;
#else
    if (cfg().relay__nofiles > curr.rlim_max) throw herr("Unable to set NOFILES limit to ", cfg().relay__nofiles, ", exceeds max of ", curr.rlim_max);

    curr.rlim_cur = cfg().relay__nofiles;
#endif

    if (setrlimit(RLIMIT_NOFILE, &curr)) throw herr("Failed setting NOFILES limit to ", cfg().relay__nofiles, ": ", strerror(errno));
}


void onAppStartup(lmdb::txn &txn, const std::string &cmd) {
    dbCheck(txn, cmd);

    setRLimits();
}
