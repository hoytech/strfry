#!/usr/bin/env python3

import argparse
import asyncio
import json
import time
import websockets


async def issue_req(relay_url: str, filter_dict: dict, timeout: float = 30.0) -> dict:
    try:
        async with websockets.connect(relay_url) as ws:
            sub_id = f"test-{int(time.time()*1000)}"
            req_msg = json.dumps(["REQ", sub_id, filter_dict])
            await ws.send(req_msg)
            events = []
            eose_received = False
            start_time = time.time()
            while not eose_received:
                remaining = timeout - (time.time() - start_time)
                if remaining <= 0:
                    break
                
                try:
                    msg = await asyncio.wait_for(ws.recv(), timeout=remaining)
                    data = json.loads(msg)
                    if len(data) >= 2:
                        msg_type = data[0]
                        if msg_type == "EVENT" and len(data) >= 3:
                            events.append(data[2])
                        elif msg_type == "EOSE":
                            eose_received = True
                except asyncio.TimeoutError:
                    break
            
            return {
                "filter": filter_dict,
                "event_count": len(events),
                "event_ids": [e.get("id") for e in events if "id" in e],
                "events": events,
                "eose_received": eose_received,
            }
    except Exception as e:
        return {
            "filter": filter_dict,
            "error": str(e),
            "event_count": 0,
            "event_ids": [],
            "events": [],
            "eose_received": False,
        }


async def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--relay", default="ws://localhost:7777")
    parser.add_argument("--filter", action="append")
    parser.add_argument("--output", default="req_results.json")
    
    args = parser.parse_args()
    if not args.filter:
        args.filter = ['{"kinds":[1],"limit":500}']
    
    results = []
    for i, filter_str in enumerate(args.filter):
        try:
            filter_dict = json.loads(filter_str)
        except json.JSONDecodeError:
            continue
        result = await issue_req(args.relay, filter_dict)
        results.append(result)
    
    with open(args.output, "w") as f:
        json.dump(results, f, indent=2)


if __name__ == "__main__":
    asyncio.run(main())
