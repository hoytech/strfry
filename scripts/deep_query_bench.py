#!/usr/bin/env python3
"""
Reproduce deep historical query degradation (issue #157).

Seeds a relay with N events from a single author+kind, then paginates
backward using `until` filters and measures per-page latency.

Expected result on a large dataset: latency increases as pagination
goes deeper into history, due to LMDB page cache behavior.

Seeding:
  Events are loaded via `strfry import --no-verify` to bypass signature
  verification. The relay does NOT need to be running during seeding.
  Start the relay after seeding.

Usage:
  # 1. Stop relay, seed 200k events, start relay:
  python3 scripts/deep_query_bench.py --seed-only --seed-events 200000
  ./strfry relay &

  # 2. Paginate and measure:
  python3 scripts/deep_query_bench.py --skip-seed --relay ws://localhost:7777

  # Smaller test (fewer events, still shows the pattern):
  python3 scripts/deep_query_bench.py --seed-events 50000
"""

import argparse
import asyncio
import json
import time
import random
import hashlib
import subprocess
import sys

try:
    import websockets
except ImportError:
    print("pip install websockets", file=sys.stderr)
    sys.exit(1)


# Fixed author so all events share the same index prefix
AUTHOR = "48ec018359cac3c933f0f7a14550e36a4f683dcf55520c916dd8c61e7724f5de"
KIND = 30382


def make_event(created_at):
    content = f"evt-{random.randint(0, 999999)}"
    eid = hashlib.sha256(f"{AUTHOR}{created_at}{KIND}{content}{random.random()}".encode()).hexdigest()
    return {
        "id": eid,
        "pubkey": AUTHOR,
        "created_at": created_at,
        "kind": KIND,
        "tags": [["d", f"item-{random.randint(0, 999999)}"]],
        "content": content,
        "sig": "0" * 128,
    }


def seed(strfry_bin, count, time_span_hours):
    """Seed events spread across `time_span_hours` of history."""
    now = int(time.time())
    span = time_span_hours * 3600

    cmd = [strfry_bin, "import", "--no-verify"]
    print(f"Seeding {count} events (kind={KIND}, author={AUTHOR[:16]}...) "
          f"spanning {time_span_hours}h...")
    print(f"  cmd: {' '.join(cmd)}")

    proc = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.DEVNULL,
                            stderr=subprocess.PIPE, text=True)

    for i in range(count):
        ts = now - int(span * i / count)
        proc.stdin.write(json.dumps(make_event(ts)) + "\n")
        if (i + 1) % 10000 == 0:
            print(f"  {i+1}/{count}", flush=True)

    proc.stdin.close()
    _, stderr = proc.communicate()
    for line in (stderr or "").strip().split('\n'):
        if line:
            print(f"  {line}")
    print(f"Seed done (exit {proc.returncode})")
    return proc.returncode == 0


async def paginate(relay_url, page_limit, max_pages, timeout):
    """Paginate backward through history using `until`, measure per-page time."""
    until = int(time.time()) + 1
    page = 0
    total_events = 0
    results = []

    print(f"\nPaginating (limit={page_limit}, timeout={timeout}s):")
    print(f"{'Page':>5} {'Events':>7} {'Time':>8} {'Until':>12} {'Oldest':>12}")
    print("-" * 55)

    async with websockets.connect(relay_url, close_timeout=5) as ws:
        while page < max_pages:
            filt = {
                "kinds": [KIND],
                "authors": [AUTHOR],
                "until": until,
                "limit": page_limit,
            }
            sub_id = f"page-{page}"

            start = time.monotonic()
            await ws.send(json.dumps(["REQ", sub_id, filt]))

            events = []
            eose = False
            while not eose:
                remaining = timeout - (time.monotonic() - start)
                if remaining <= 0:
                    print(f"  TIMEOUT at page {page} ({timeout}s)")
                    break
                try:
                    msg = await asyncio.wait_for(ws.recv(), timeout=remaining)
                    data = json.loads(msg)
                    if data[0] == "EVENT" and len(data) >= 3:
                        events.append(data[2])
                    elif data[0] == "EOSE":
                        eose = True
                except asyncio.TimeoutError:
                    print(f"  TIMEOUT at page {page} ({timeout}s)")
                    break

            elapsed = time.monotonic() - start
            await ws.send(json.dumps(["CLOSE", sub_id]))

            if not events:
                print(f"  No events returned at page {page}, stopping.")
                break

            oldest_ts = min(e["created_at"] for e in events)
            newest_ts = max(e["created_at"] for e in events)
            total_events += len(events)

            print(f"{page:>5} {len(events):>7} {elapsed:>7.2f}s "
                  f"{until:>12} {oldest_ts:>12}")

            results.append({
                "page": page,
                "events": len(events),
                "time_s": round(elapsed, 3),
                "until": until,
                "oldest_ts": oldest_ts,
                "newest_ts": newest_ts,
                "total_so_far": total_events,
                "timed_out": not eose,
            })

            if not eose:
                break

            # Next page starts just before the oldest event we got
            until = oldest_ts - 1
            page += 1

    print(f"\nTotal: {total_events} events across {page + 1} pages")
    return results


async def run(args):
    results = await paginate(args.relay, args.limit, args.max_pages, args.timeout)
    with open(args.output, "w") as f:
        json.dump({
            "author": AUTHOR,
            "kind": KIND,
            "limit": args.limit,
            "pages": results,
        }, f, indent=2)
    print(f"Results saved to {args.output}")


def main():
    p = argparse.ArgumentParser(description="Reproduce deep query degradation (#157)")
    p.add_argument("--relay", default="ws://localhost:7777")
    p.add_argument("--strfry-bin", default="./strfry")
    p.add_argument("--seed-events", type=int, default=50000)
    p.add_argument("--time-span-hours", type=int, default=12,
                    help="Spread events across this many hours of history")
    p.add_argument("--seed-only", action="store_true")
    p.add_argument("--skip-seed", action="store_true")
    p.add_argument("--limit", type=int, default=500,
                    help="Events per page (matches issue #157 setup)")
    p.add_argument("--max-pages", type=int, default=500)
    p.add_argument("--timeout", type=float, default=30,
                    help="Per-page timeout in seconds")
    p.add_argument("--output", default="deep_query_results.json")

    args = p.parse_args()

    if not args.skip_seed:
        seed(args.strfry_bin, args.seed_events, args.time_span_hours)

    if args.seed_only:
        return

    asyncio.run(run(args))


if __name__ == "__main__":
    main()
