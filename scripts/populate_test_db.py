#!/usr/bin/env python3

import argparse
import asyncio
import json
import time
import random
import string
from typing import Optional
import websockets


def generate_random_key() -> str:
    return ''.join(random.choices(string.hexdigits[:16], k=64))


def generate_event(kind: int, author: Optional[str] = None, content: str = "") -> dict:
    if author is None:
        author = generate_random_key()
    created_at = int(time.time()) - random.randint(0, 86400)
    
    event = {
        "kind": kind,
        "pubkey": author,
        "created_at": created_at,
        "tags": [],
        "content": content or f"Test event {random.randint(1000, 9999)}",
    }
    event["sig"] = generate_random_key()[:128]
    event["id"] = generate_random_key()
    
    return event


async def populate_relay(relay_url: str, num_events: int, kinds: list[int]):
    try:
        async with websockets.connect(relay_url) as ws:
            sent = 0
            authors = [generate_random_key() for _ in range(20)]
            
            for i in range(num_events):
                kind = random.choice(kinds)
                author = random.choice(authors)
                event = generate_event(kind, author, f"Content #{i}")
                msg = json.dumps(["EVENT", event])
                try:
                    await ws.send(msg)
                    try:
                        response = await asyncio.wait_for(ws.recv(), timeout=1.0)
                        resp = json.loads(response)
                        if resp[0] == "OK":
                            sent += 1
                    except asyncio.TimeoutError:
                        sent += 1
                except Exception as e:
                    break
            print(f"Sent {sent}/{num_events}")
            return sent
    except Exception as e:
        print(f"Error: {e}")
        return 0


async def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--relay", default="ws://localhost:7777")
    parser.add_argument("--events", type=int, default=1000)
    parser.add_argument("--kinds", default="1,0,3")
    
    args = parser.parse_args()
    kinds = [int(k) for k in args.kinds.split(",")]
    await populate_relay(args.relay, args.events, kinds)


if __name__ == "__main__":
    asyncio.run(main())
