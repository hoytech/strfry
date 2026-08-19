#!/usr/bin/env python3

import re
import sys
import json
from collections import defaultdict
from dataclasses import dataclass
from typing import List

LOG_RE = re.compile(
    r'(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}).*?'
    r"REQ='([^']+)'\s+"
    r'scan=(\w+)\s+'
    r'indexOnly=(\d+)\s+'
    r'time=(\d+)us\s+'
    r'saveRestores=(\d+)\s+'
    r'recsFound=(\d+)\s+'
    r'work=(\d+)'
)

@dataclass
class ScanRecord:
    timestamp: str
    sub_id: str
    scan_type: str
    index_only: bool
    time_us: int
    save_restores: int
    recs_found: int
    work: int

def parse_log(path: str) -> List[ScanRecord]:
    records = []
    with open(path) as f:
        for line in f:
            m = LOG_RE.search(line)
            if m:
                records.append(ScanRecord(
                    timestamp=m.group(1),
                    sub_id=m.group(2),
                    scan_type=m.group(3),
                    index_only=bool(int(m.group(4))),
                    time_us=int(m.group(5)),
                    save_restores=int(m.group(6)),
                    recs_found=int(m.group(7)),
                    work=int(m.group(8)),
                ))
    return records

def report(records: List[ScanRecord]):
    if not records:
        print("No scan metric lines found. Is logScanMetrics enabled?")
        return

    paused = [r for r in records if r.save_restores > 0]
    by_type = defaultdict(list)
    for r in records:
        by_type[r.scan_type].append(r)

    print(f"Total: {len(records)}")
    print(f"Paused: {len(paused)} ({100*len(paused)/len(records):.1f}%)")
    
    if paused:
        print(f"Max restores: {max(r.save_restores for r in records)}")
        print(f"Paused avg: {sum(r.time_us for r in paused)/len(paused):.0f}µs")
    
    unpaused = [r for r in records if r.save_restores == 0]
    if unpaused:
        print(f"Unpaused avg: {sum(r.time_us for r in unpaused)/len(unpaused):.0f}µs")

    print(f"\n{'Type':<10} {'Count':>6} {'Paused':>6} {'AvgTime':>10} {'AvgRest':>8}")
    for scan_type, recs in sorted(by_type.items()):
        paused_recs = [r for r in recs if r.save_restores > 0]
        avg_time = sum(r.time_us for r in recs) / len(recs)
        avg_restores = sum(r.save_restores for r in recs) / len(recs)
        print(f"{scan_type:<10} {len(recs):>6} {len(paused_recs):>6} "
              f"{avg_time:>10.0f} {avg_restores:>8.1f}")

if __name__ == "__main__":
    path = sys.argv[1] if len(sys.argv) > 1 else "relay.log"
    records = parse_log(path)
    report(records)
    with open("scan_metrics.json", "w") as f:
        json.dump([vars(r) for r in records], f, indent=2)
