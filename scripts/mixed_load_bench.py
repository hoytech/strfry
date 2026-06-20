#!/usr/bin/env python3

import argparse
import asyncio
import json
import time
import random
import string
from dataclasses import dataclass, field
import websockets


@dataclass
class BenchmarkResults:
    write_rate_target: int
    write_rate_actual: float = 0.0
    read_rate_actual: float = 0.0
    req_p50_ms: float = 0.0
    req_p95_ms: float = 0.0
    req_p99_ms: float = 0.0
    req_latencies_ms: list = field(default_factory=list)
    total_events_sent: int = 0
    total_requests: int = 0
    errors: int = 0


def generate_random_key() -> str:
    return ''.join(random.choices(string.hexdigits[:16], k=64))


def generate_event(kind: int = 1, author: str = "") -> dict:
    if not author:
        author = generate_random_key()
    created_at = int(time.time())
    
    event = {
        "kind": kind,
        "pubkey": author,
        "created_at": created_at,
        "tags": [],
        "content": f"Benchmark event {random.randint(1000, 9999)}",
        "sig": generate_random_key()[:128],
        "id": generate_random_key(),
    }
    return event


async def writer_task(relay_url: str, write_interval: float, duration: float, results: BenchmarkResults):
    start_time = time.time()
    try:
        async with websockets.connect(relay_url) as ws:
            while time.time() - start_time < duration:
                try:
                    event = generate_event()
                    msg = json.dumps(["EVENT", event])
                    await ws.send(msg)
                    try:
                        await asyncio.wait_for(ws.recv(), timeout=0.5)
                    except asyncio.TimeoutError:
                        pass
                    
                    results.total_events_sent += 1
                    await asyncio.sleep(write_interval)
                except Exception as e:
                    results.errors += 1
                    await asyncio.sleep(0.1)
    except Exception as e:
        results.errors += 1


async def reader_task(relay_url: str, req_interval: float, duration: float, results: BenchmarkResults):
    start_time = time.time()
    try:
        async with websockets.connect(relay_url) as ws:
            reader_id = random.randint(0, 9999)
            while time.time() - start_time < duration:
                try:
                    sub_id = f"reader-{reader_id}-{int(time.time()*1000)}"
                    filter_dict = {"kinds": [1], "limit": 500}
                    req_msg = json.dumps(["REQ", sub_id, filter_dict])
                    
                    req_time = time.time()
                    await ws.send(req_msg)
                    eose_received = False
                    while not eose_received:
                        try:
                            msg = await asyncio.wait_for(ws.recv(), timeout=30.0)
                            data = json.loads(msg)
                            if len(data) >= 2 and data[0] == "EOSE":
                                eose_received = True
                        except asyncio.TimeoutError:
                            break
                    
                    latency_ms = (time.time() - req_time) * 1000
                    results.req_latencies_ms.append(latency_ms)
                    results.total_requests += 1
                    
                    await asyncio.sleep(req_interval)
                except Exception as e:
                    results.errors += 1
                    await asyncio.sleep(0.1)
    except Exception as e:
        results.errors += 1


def percentile(data: list, p: int) -> float:
    if not data:
        return 0.0
    sorted_data = sorted(data)
    index = int(len(sorted_data) * p / 100)
    return sorted_data[min(index, len(sorted_data) - 1)]


async def run_benchmark(relay_url: str, write_rate: int, read_rate: int,
                       writers: int, readers: int, duration: float) -> BenchmarkResults:
    results = BenchmarkResults(write_rate_target=write_rate)
    write_interval = writers / max(write_rate, 1)
    read_interval = readers / max(read_rate, 1)
    tasks = []
    for _ in range(writers):
        tasks.append(writer_task(relay_url, write_interval, duration, results))
    for _ in range(readers):
        tasks.append(reader_task(relay_url, read_interval, duration, results))
    
    await asyncio.gather(*tasks)
    
    elapsed = duration
    results.write_rate_actual = results.total_events_sent / elapsed
    results.read_rate_actual = results.total_requests / elapsed
    
    if results.req_latencies_ms:
        results.req_p50_ms = percentile(results.req_latencies_ms, 50)
        results.req_p95_ms = percentile(results.req_latencies_ms, 95)
        results.req_p99_ms = percentile(results.req_latencies_ms, 99)
    
    return results


async def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--relay", default="ws://localhost:7777")
    parser.add_argument("--write-rate", type=int, default=1000)
    parser.add_argument("--read-rate", type=int, default=10)
    parser.add_argument("--writers", type=int, default=10)
    parser.add_argument("--readers", type=int, default=50)
    parser.add_argument("--duration", type=float, default=60)
    parser.add_argument("--output", default="bench_results.json")
    
    args = parser.parse_args()
    try:
        results = await run_benchmark(
            args.relay,
            args.write_rate,
            args.read_rate,
            args.writers,
            args.readers,
            args.duration
        )
        
        print(f"Write: {results.write_rate_actual:.1f} ev/s")
        print(f"Read: {results.read_rate_actual:.1f} REQ/s")
        print(f"p50={results.req_p50_ms:.1f}ms p95={results.req_p95_ms:.1f}ms p99={results.req_p99_ms:.1f}ms")
        
        output_dict = {
            "write_rate_target": results.write_rate_target,
            "write_rate_actual": results.write_rate_actual,
            "read_rate_actual": results.read_rate_actual,
            "req_p50_ms": results.req_p50_ms,
            "req_p95_ms": results.req_p95_ms,
            "req_p99_ms": results.req_p99_ms,
            "total_events_sent": results.total_events_sent,
            "total_requests": results.total_requests,
            "errors": results.errors,
        }
        
        with open(args.output, "w") as f:
            json.dump(output_dict, f, indent=2)
        print(f"Saved to {args.output}")
    except Exception as e:
        print(f"Error: {e}")


if __name__ == "__main__":
    asyncio.run(main())
