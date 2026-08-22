#!/usr/bin/env python3
import sys
import json

for line in sys.stdin:
    try:
        req = json.loads(line)
        if req.get("type") == "new":
            res = {
                "id": req["event"]["id"],
                "action": "accept"
            }
            print(json.dumps(res), flush=True)
    except Exception as e:
        sys.stderr.write(f"Error: {e}\n")
        sys.stderr.flush()
