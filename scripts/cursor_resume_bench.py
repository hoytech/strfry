#!/usr/bin/env python3
"""
Benchmark: REQ latency under concurrent ingester load.

Measures how cursor pause/resume in the query scheduler affects
REQ response times when the ingester threads are busy.

Seeding is done via `strfry import --no-verify` (stdin JSONL) since
the relay's websocket ingester verifies signatures.

Usage:
  # 1. Seed test data (only needed once):
  python3 scripts/cursor_resume_bench.py --seed-only --seed-events 10000

  # 2. Run the benchmark (relay must be running):
  python3 scripts/cursor_resume_bench.py --relay ws://localhost:7777

  # 3. Parse the scan perf logs:
  python3 scripts/scan_perf_report.py relay.log
"""

import argparse
import asyncio
import json
import time
import random
import hashlib
import subprocess
import sys
from dataclasses import dataclass, field

try:
    import websockets
except ImportError:
    print("pip install websockets", file=sys.stderr)
    sys.exit(1)


def make_hex(n=64):
    return ''.join(random.choices('0123456789abcdef', k=n))


def make_event(kind=1, pubkey=None, created_at=None):
    """Generate a fake nostr event dict."""
    pk = pubkey or make_hex(64)
    ts = created_at or (int(time.time()) - random.randint(0, 86400))
    content = f"bench-{random.randint(0, 999999)}"
    eid = hashlib.sha256(f"{pk}{ts}{kind}{content}{random.random()}".encode()).hexdigest()
    return {
        "id": eid,
        "pubkey": pk,
        "created_at": ts,
        "kind": kind,
        "tags": [],
        "content": content,
        "sig": make_hex(128),
    }


def seed_via_import(strfry_bin, count, kinds):
    """Seed the DB using `strfry import --no-verify` (bypasses sig checks)."""
    authors = [make_hex(64) for _ in range(20)]
    cmd = [strfry_bin, "import", "--no-verify"]

    print(f"  Seeding {count} events via `{' '.join(cmd)}`...")
    proc = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.DEVNULL,
                            stderr=subprocess.PIPE, text=True)

    for i in range(count):
        ev = make_event(
            kind=random.choice(kinds),
            pubkey=random.choice(authors),
        )
        proc.stdin.write(json.dumps(ev) + "\n")

    proc.stdin.close()
    _, stderr = proc.communicate()
    if stderr:
        # strfry import logs to stderr
        for line in stderr.strip().split('\n'):
            if line:
                print(f"    {line}")
    print(f"  Seed complete (exit {proc.returncode})")
    return proc.returncode == 0


@dataclass
class SweepResult:
    write_rate: int = 0
    reqs: int = 0
    latencies_ms: list = field(default_factory=list)
    errors: int = 0

    def p(self, pct):
        if not self.latencies_ms:
            return 0.0
        s = sorted(self.latencies_ms)
        return s[min(int(len(s) * pct / 100), len(s) - 1)]


async def reader_loop(url, duration, filters, result):
    """Issue REQ queries and record EOSE latency."""
    t0 = time.monotonic()
    seq = 0
    try:
        async with websockets.connect(url) as ws:
            while time.monotonic() - t0 < duration:
                filt = random.choice(filters)
                sub_id = f"b-{id(result) % 9999}-{seq}"
                seq += 1

                start = time.monotonic()
                await ws.send(json.dumps(["REQ", sub_id, filt]))

                eose = False
                while not eose:
                    left = 30.0 - (time.monotonic() - start)
                    if left <= 0:
                        break
                    try:
                        msg = await asyncio.wait_for(ws.recv(), timeout=left)
                        data = json.loads(msg)
                        if data[0] == "EOSE":
                            eose = True
                    except asyncio.TimeoutError:
                        break

                result.latencies_ms.append((time.monotonic() - start) * 1000)
                result.reqs += 1
                await ws.send(json.dumps(["CLOSE", sub_id]))
                await asyncio.sleep(0.3)
    except Exception as e:
        result.errors += 1


async def writer_loop(url, interval, duration, result):
    """Send EVENTs over websocket. These get rejected (bad sigs) but the
    ingester threads still parse and verify them, competing for CPU with
    the reqWorker threads that handle REQ queries."""
    if interval <= 0:
        return
    t0 = time.monotonic()
    try:
        async with websockets.connect(url) as ws:
            while time.monotonic() - t0 < duration:
                ev = make_event()
                await ws.send(json.dumps(["EVENT", ev]))
                try:
                    await asyncio.wait_for(ws.recv(), timeout=0.3)
                except asyncio.TimeoutError:
                    pass
                await asyncio.sleep(interval)
    except Exception:
        result.errors += 1


async def sweep_point(url, write_rate, n_writers, n_readers, duration, filters):
    result = SweepResult(write_rate=write_rate)
    tasks = []

    if write_rate > 0:
        wi = max(n_writers / write_rate, 0.001)
        for _ in range(n_writers):
            tasks.append(writer_loop(url, wi, duration, result))

    for _ in range(n_readers):
        tasks.append(reader_loop(url, duration, filters, result))

    await asyncio.gather(*tasks)
    return result


async def run(args):
    rates = [int(r) for r in args.rates.split(",")]
    filters = [
        {"kinds": [1], "limit": 500},
        {"kinds": [1, 0, 3], "limit": 200},
        {"limit": 100},
    ]

    print(f"\nRelay:    {args.relay}")
    print(f"Rates:    {rates}")
    print(f"Readers:  {args.readers}")
    print(f"Duration: {args.duration}s per point\n")

    hdr = f"{'Rate':>6} {'Reqs':>5} {'p50':>7} {'p95':>7} {'p99':>7} {'max':>7} {'err':>4}"
    print(hdr)
    print("-" * len(hdr))

    out = []
    for i, rate in enumerate(rates):
        r = await sweep_point(
            args.relay, rate, args.writers, args.readers, args.duration, filters
        )
        mx = max(r.latencies_ms) if r.latencies_ms else 0
        print(f"{rate:>6} {r.reqs:>5} {r.p(50):>7.1f} {r.p(95):>7.1f} "
              f"{r.p(99):>7.1f} {mx:>7.1f} {r.errors:>4}")
        out.append({
            "write_rate": rate, "reqs": r.reqs,
            "p50_ms": round(r.p(50), 2), "p95_ms": round(r.p(95), 2),
            "p99_ms": round(r.p(99), 2), "max_ms": round(mx, 2),
            "errors": r.errors,
        })
        if i < len(rates) - 1:
            await asyncio.sleep(2)

    with open(args.output, "w") as f:
        json.dump(out, f, indent=2)
    print(f"\nSaved to {args.output}")


def main():
    p = argparse.ArgumentParser(description="Cursor resume latency benchmark")
    p.add_argument("--relay", default="ws://localhost:7777")
    p.add_argument("--strfry-bin", default="./strfry",
                    help="Path to strfry binary (for seeding via import)")
    p.add_argument("--rates", default="0,100,500,1000,2000")
    p.add_argument("--seed-events", type=int, default=5000)
    p.add_argument("--seed-only", action="store_true",
                    help="Only seed data, don't run benchmark")
    p.add_argument("--skip-seed", action="store_true")
    p.add_argument("--writers", type=int, default=5)
    p.add_argument("--readers", type=int, default=20)
    p.add_argument("--duration", type=float, default=30)
    p.add_argument("--output", default="cursor_resume_results.json")

    args = p.parse_args()

    if not args.skip_seed:
        seed_via_import(args.strfry_bin, args.seed_events, [1, 0, 3])

    if args.seed_only:
        return

    asyncio.run(run(args))


if __name__ == "__main__":
    main()
