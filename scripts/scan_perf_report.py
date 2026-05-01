#!/usr/bin/env python3
"""
Parse strfry scan performance logs and report cursor resume statistics.

Reads log lines emitted when relay.logging.dbScanPerf = true, and reports:
  - Per-scan-type latency distribution
  - Collect time distribution
  - Refill timing and frequency

Usage:
  # Pipe live logs:
  ./strfry relay 2>&1 | tee relay.log
  python3 scripts/scan_perf_report.py relay.log

  # Or parse existing log:
  python3 scripts/scan_perf_report.py /var/log/strfry.log --output report.json
"""

import argparse
import re
import sys
import json
from collections import defaultdict
from dataclasses import dataclass
from typing import List


# Match the enhanced log format with refill/collect diagnostics
LOG_PATTERN = re.compile(
    r"REQ='(?P<sub_id>[^']+)'\s+"
    r"scan=(?P<scan_type>\w+)\s+"
    r"indexOnly=(?P<index_only>\d+)\s+"
    r"time=(?P<time_us>\d+)us\s+"
    r"saveRestores=(?P<save_restores>\d+)\s+"
    r"recsFound=(?P<recs_found>\d+)\s+"
    r"work=(?P<work>\d+)"
    r"(?:\s+refills=(?P<refills>\d+))?"
    r"(?:\s+avgRefillUs=(?P<avg_refill_us>\d+))?"
    r"(?:\s+maxRefillUs=(?P<max_refill_us>\d+))?"
    r"(?:\s+collects=(?P<collects>\d+))?"
    r"(?:\s+avgCollectUs=(?P<avg_collect_us>\d+))?"
    r"(?:\s+maxCollectUs=(?P<max_collect_us>\d+))?"
)



@dataclass
class ScanRecord:
    sub_id: str
    scan_type: str
    index_only: bool
    time_us: int
    save_restores: int
    recs_found: int
    work: int
    refills: int = 0
    avg_refill_us: int = 0
    max_refill_us: int = 0
    collects: int = 0
    avg_collect_us: int = 0
    max_collect_us: int = 0


def parse_log(path: str) -> List[ScanRecord]:
    records = []
    with open(path) as f:
        for line in f:
            m = LOG_PATTERN.search(line)
            if not m:
                continue
            d = m.groupdict()
            records.append(ScanRecord(
                sub_id=d["sub_id"],
                scan_type=d["scan_type"],
                index_only=bool(int(d["index_only"])),
                time_us=int(d["time_us"]),
                save_restores=int(d["save_restores"]),
                recs_found=int(d["recs_found"]),
                work=int(d["work"]),
                refills=int(d["refills"] or 0),
                avg_refill_us=int(d["avg_refill_us"] or 0),
                max_refill_us=int(d["max_refill_us"] or 0),
                collects=int(d["collects"] or 0),
                avg_collect_us=int(d["avg_collect_us"] or 0),
                max_collect_us=int(d["max_collect_us"] or 0),
            ))
    return records


def percentile(values: List[float], p: int) -> float:
    if not values:
        return 0.0
    s = sorted(values)
    idx = min(int(len(s) * p / 100), len(s) - 1)
    return s[idx]


def report(records: List[ScanRecord]):
    if not records:
        print("No scan metric lines found.")
        print("Ensure relay.logging.dbScanPerf = true in strfry.conf")
        return

    paused = [r for r in records if r.save_restores > 0]
    has_collect_data = any(r.collects > 0 for r in records)

    print(f"\n{'='*60}")
    print(f"  Scan Performance Report  ({len(records)} queries)")
    print(f"{'='*60}\n")

    print(f"  Total scans:   {len(records)}")
    print(f"  Paused scans:  {len(paused)} ({100*len(paused)/len(records):.1f}%)")

    times = [r.time_us for r in records]
    print(f"\n  Latency (µs):")
    print(f"    p50:  {percentile(times, 50):>10.0f}")
    print(f"    p95:  {percentile(times, 95):>10.0f}")
    print(f"    p99:  {percentile(times, 99):>10.0f}")
    print(f"    max:  {max(times):>10}")

    if paused:
        paused_times = [r.time_us for r in paused]
        unpaused_times = [r.time_us for r in records if r.save_restores == 0]

        print(f"\n  Paused vs Unpaused (µs):")
        if unpaused_times:
            print(f"    Unpaused avg: {sum(unpaused_times)/len(unpaused_times):>10.0f}")
        print(f"    Paused avg:   {sum(paused_times)/len(paused_times):>10.0f}")
        print(f"    Max restores: {max(r.save_restores for r in paused):>10}")

    # Per-scan-type breakdown
    by_type = defaultdict(list)
    for r in records:
        by_type[r.scan_type].append(r)

    print(f"\n  {'Type':<12} {'Count':>6} {'Paused':>6} {'p50µs':>9} {'p99µs':>9} {'AvgWork':>9}")
    print(f"  {'-'*12} {'-'*6} {'-'*6} {'-'*9} {'-'*9} {'-'*9}")

    for scan_type in sorted(by_type.keys()):
        recs = by_type[scan_type]
        paused_count = sum(1 for r in recs if r.save_restores > 0)
        times = [r.time_us for r in recs]
        avg_work = sum(r.work for r in recs) / len(recs)
        print(f"  {scan_type:<12} {len(recs):>6} {paused_count:>6} "
              f"{percentile(times, 50):>9.0f} {percentile(times, 99):>9.0f} "
              f"{avg_work:>9.0f}")

    # Collect call analysis (only with enhanced instrumentation)
    if has_collect_data:
        print(f"\n  Collect Analysis:")
        collect_records = [r for r in records if r.collects > 0]
        if collect_records:
            avg_collects = [r.avg_collect_us for r in collect_records]
            max_collects = [r.max_collect_us for r in collect_records]
            print(f"    Queries with collect data: {len(collect_records)}")
            print(f"    Avg collect time p50: {percentile(avg_collects, 50):>8.0f} µs")
            print(f"    Avg collect time p99: {percentile(avg_collects, 99):>8.0f} µs")
            print(f"    Max collect time:     {max(max_collects):>8} µs")

        refill_records = [r for r in records if r.refills > 0]
        if refill_records:
            print(f"\n  Refill Analysis:")
            print(f"    Queries with refills: {len(refill_records)}")
            avg_refills = [r.avg_refill_us for r in refill_records]
            max_refills = [r.max_refill_us for r in refill_records]
            print(f"    Avg refill time p50: {percentile(avg_refills, 50):>8.0f} µs")
            print(f"    Avg refill time p99: {percentile(avg_refills, 99):>8.0f} µs")
            print(f"    Max refill time:     {max(max_refills):>8} µs")

    print()


def main():
    parser = argparse.ArgumentParser(description="Parse strfry scan performance logs")
    parser.add_argument("logfile", nargs="?", default="relay.log",
                        help="Path to strfry log file (default: relay.log)")
    parser.add_argument("--output", "-o", default=None,
                        help="Write structured data to JSON file")

    args = parser.parse_args()

    try:
        records = parse_log(args.logfile)
    except FileNotFoundError:
        print(f"Error: log file not found: {args.logfile}", file=sys.stderr)
        sys.exit(1)

    report(records)

    if args.output:
        data = [vars(r) for r in records]
        with open(args.output, "w") as f:
            json.dump(data, f, indent=2)
        print(f"Structured data saved to {args.output}")


if __name__ == "__main__":
    main()
