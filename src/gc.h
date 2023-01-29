#pragma once

#include <parallel_hashmap/phmap.h>

#include "golpe.h"

#include "render.h"


inline void quadrableGarbageCollect(quadrable::Quadrable &qdb, int logLevel = 0) {
    quadrable::Quadrable::GarbageCollector<phmap::flat_hash_set<uint64_t>> gc(qdb);
    quadrable::Quadrable::GCStats stats;

    if (logLevel >= 2) LI << "Running garbage collection";

    {
        auto txn = env.txn_ro();

        if (logLevel >= 2) LI << "GC: mark phase";
        gc.markAllHeads(txn);
        if (logLevel >= 2) LI << "GC: sweep phase";
        stats = gc.sweep(txn);
    }

    if (logLevel >= 2) {
        LI << "GC: Total nodes:   " << stats.total;
        LI << "GC: Garbage nodes: " << stats.garbage << " (" << renderPercent((double)stats.garbage / stats.total) << ")";
    }

    if (stats.garbage) {
        auto txn = env.txn_rw();
        if (logLevel >= 1) LI << "GC: deleting " << stats.garbage << " garbage nodes";
        gc.deleteNodes(txn);
        txn.commit();
    }

}
