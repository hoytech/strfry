#!/usr/bin/env python3
"""
NIP-45 HyperLogLog COUNT End-to-End Tests for strfry relay.

Requires: pip install secp256k1 websockets

Tests HLL data in COUNT responses against a live strfry relay:
  - Publish events from known pubkeys, COUNT with tag filters
  - Verify response includes count and hll (512-char hex)
  - Verify hll matches Python oracle computation
  - Verify non-eligible filters omit hll
  - Verify 0-result queries return all-zero hll
"""

import asyncio
import hashlib
import json
import os
import signal
import shutil
import socket
import struct
import subprocess
import sys
import time

import secp256k1
import websockets


# ── HLL reference (inline from hll_reference.py) ────────────────────────────

def clz56(x: int) -> int:
    c = 0
    m = 1 << 55
    while (m & x) == 0 and m != 0:
        c += 1
        m >>= 1
    return c

def hll_add(registers: bytearray, pubkey_bytes: bytes, offset: int):
    x = pubkey_bytes[offset:offset + 8]
    j = x[0]
    w = struct.unpack('>Q', x)[0]
    zero_bits = clz56(w) + 1
    if zero_bits > registers[j]:
        registers[j] = zero_bits

def hll_encode(registers: bytearray) -> str:
    return registers.hex()

def compute_offset_from_hex(tag_value_hex: str) -> int:
    return int(tag_value_hex[32], 16) + 8


# ── Keys ────────────────────────────────────────────────────────────────────

KEY_A_SEC = bytes.fromhex("c1eee22f68dc218d98263cfecb350db6fc6b3e836b47423b66c62af7ae3e32bb")
KEY_A_PUB = None  # computed at startup

KEY_B_SEC = bytes.fromhex("a0b459d9ff90e30dc9d1749b34c4401dfe80ac2617c7732925ff994e8d5203ff")
KEY_B_PUB = None

KEY_C_SEC = bytes.fromhex("3fa463bf3a6b0e4c8d9a1e7b2f5c6d8e0a1b3c4d5e6f7a8b9c0d1e2f3a4b5c6d")
KEY_C_PUB = None

KEY_D_SEC = bytes.fromhex("5fa463bf3a6b0e4c8d9a1e7b2f5c6d8e0a1b3c4d5e6f7a8b9c0d1e2f3a4b5c6e")
KEY_D_PUB = None


# ── Nostr helpers ───────────────────────────────────────────────────────────

def compute_pubkey(sec_bytes: bytes) -> str:
    pk = secp256k1.PrivateKey(sec_bytes)
    return pk.pubkey.serialize()[1:].hex()


def make_event(sec_key: bytes, pubkey: str, kind: int, content: str,
               tags: list = None, created_at: int = None) -> dict:
    if tags is None:
        tags = []
    if created_at is None:
        created_at = int(time.time())
    event = {
        "pubkey": pubkey,
        "created_at": created_at,
        "kind": kind,
        "tags": tags,
        "content": content,
    }
    serialized = json.dumps(
        [0, event["pubkey"], event["created_at"], event["kind"],
         event["tags"], event["content"]],
        separators=(",", ":"), ensure_ascii=False,
    )
    event["id"] = hashlib.sha256(serialized.encode()).hexdigest()
    pk = secp256k1.PrivateKey(sec_key)
    sig = pk.schnorr_sign(bytes.fromhex(event["id"]), bip340tag=None, raw=True)
    event["sig"] = sig.hex()
    return event


async def send_event(ws, event):
    await ws.send(json.dumps(["EVENT", event]))
    deadline = time.monotonic() + 10
    while time.monotonic() < deadline:
        resp = json.loads(await asyncio.wait_for(ws.recv(), timeout=5))
        if resp[0] == "OK" and resp[1] == event["id"]:
            return resp
    raise TimeoutError(f"no OK for event {event['id']} within 10s")


async def count_events(ws, filt, sub_id="cnt"):
    """Send a COUNT request, return the response body dict."""
    await ws.send(json.dumps(["COUNT", sub_id, filt]))
    deadline = time.monotonic() + 10
    while time.monotonic() < deadline:
        resp = json.loads(await asyncio.wait_for(ws.recv(), timeout=5))
        if resp[0] == "COUNT" and resp[1] == sub_id:
            return resp[2]
    raise TimeoutError(f"no COUNT response for {sub_id} within 10s")


async def connect(port=40563):
    return await websockets.connect(f"ws://127.0.0.1:{port}")


# ── Process management ──────────────────────────────────────────────────────

STRFRY = "./strfry"
DB_DIR = "./strfry-db-nip45-hll-test"
CONF = "test/cfgs/nip45HllTest.conf"
PORT = 40563


def clean_db():
    if os.path.exists(DB_DIR):
        shutil.rmtree(DB_DIR)
    os.makedirs(DB_DIR, exist_ok=True)


def start_relay(conf=CONF):
    try:
        s = socket.create_connection(("127.0.0.1", PORT), timeout=0.1)
        s.close()
        raise RuntimeError(f"port {PORT} already in use before starting relay")
    except (ConnectionRefusedError, OSError):
        pass

    proc = subprocess.Popen(
        [STRFRY, "--config", conf, "relay"],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    for _ in range(50):
        if proc.poll() is not None:
            raise RuntimeError(f"strfry exited early with code {proc.returncode}")
        try:
            s = socket.create_connection(("127.0.0.1", PORT), timeout=0.1)
            s.close()
            return proc
        except (ConnectionRefusedError, OSError):
            time.sleep(0.1)
    proc.kill()
    proc.wait()
    raise RuntimeError("strfry relay did not start within 5s")


def stop_relay(proc):
    proc.send_signal(signal.SIGTERM)
    try:
        proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait()


# ── Test infrastructure ─────────────────────────────────────────────────────

passed = 0
failed = 0
errors = []


def report(name, ok, detail=""):
    global passed, failed, errors
    if ok:
        passed += 1
        print(f"  PASS  {name}")
    else:
        failed += 1
        errors.append((name, detail))
        print(f"  FAIL  {name}  -- {detail}")


async def run_with_fresh_relay(test_fn, conf=CONF):
    clean_db()
    relay = start_relay(conf)
    try:
        await test_fn()
    finally:
        stop_relay(relay)


# ── Tests ───────────────────────────────────────────────────────────────────

async def test_hll_e_tag_reactions():
    """Publish kind-7 reactions from N pubkeys tagging a target event, verify HLL."""
    ws = await connect()
    try:
        # Create a target event
        target = make_event(KEY_A_SEC, KEY_A_PUB, 1, "target post", created_at=1000)
        await send_event(ws, target)
        target_id = target["id"]

        # Post reactions from multiple pubkeys
        keys = [
            (KEY_A_SEC, KEY_A_PUB),
            (KEY_B_SEC, KEY_B_PUB),
            (KEY_C_SEC, KEY_C_PUB),
            (KEY_D_SEC, KEY_D_PUB),
        ]

        for i, (sec, pub) in enumerate(keys):
            ev = make_event(sec, pub, 7, "+",
                            tags=[["e", target_id]], created_at=2000 + i)
            r = await send_event(ws, ev)
            assert r[2] is True, f"event rejected: {r}"

        # COUNT with #e filter
        result = await count_events(ws, {"#e": [target_id], "kinds": [7]})

        report("HLL #e: count is correct",
               result.get("count") == len(keys),
               f"count={result.get('count')}, expected={len(keys)}")

        hll_hex = result.get("hll", "")
        report("HLL #e: hll field present and 512 chars",
               len(hll_hex) == 512,
               f"hll length={len(hll_hex)}")

        # Compute expected HLL
        offset = compute_offset_from_hex(target_id)
        expected_regs = bytearray(256)
        for _, pub in keys:
            hll_add(expected_regs, bytes.fromhex(pub), offset)
        expected_hex = hll_encode(expected_regs)

        report("HLL #e: hll matches oracle",
               hll_hex == expected_hex,
               f"got={hll_hex[:40]}... expected={expected_hex[:40]}...")
    finally:
        await ws.close()


async def test_hll_p_tag_follows():
    """Publish kind-3 follow events, verify HLL for #p filter."""
    ws = await connect()
    try:
        # Post follow events from multiple pubkeys all tagging KEY_A_PUB
        keys = [
            (KEY_B_SEC, KEY_B_PUB),
            (KEY_C_SEC, KEY_C_PUB),
            (KEY_D_SEC, KEY_D_PUB),
        ]

        for i, (sec, pub) in enumerate(keys):
            ev = make_event(sec, pub, 3, "",
                            tags=[["p", KEY_A_PUB]], created_at=3000 + i)
            r = await send_event(ws, ev)
            assert r[2] is True, f"event rejected: {r}"

        # COUNT with #p filter
        result = await count_events(ws, {"#p": [KEY_A_PUB], "kinds": [3]})

        report("HLL #p: count is correct",
               result.get("count") == len(keys),
               f"count={result.get('count')}, expected={len(keys)}")

        hll_hex = result.get("hll", "")
        report("HLL #p: hll field present and 512 chars",
               len(hll_hex) == 512,
               f"hll length={len(hll_hex)}")

        # Compute expected HLL
        offset = compute_offset_from_hex(KEY_A_PUB)
        expected_regs = bytearray(256)
        for _, pub in keys:
            hll_add(expected_regs, bytes.fromhex(pub), offset)
        expected_hex = hll_encode(expected_regs)

        report("HLL #p: hll matches oracle",
               hll_hex == expected_hex,
               f"got={hll_hex[:40]}... expected={expected_hex[:40]}...")
    finally:
        await ws.close()


async def test_no_hll_without_tags():
    """Non-eligible filter (no tags) should NOT include hll in response."""
    ws = await connect()
    try:
        ev = make_event(KEY_A_SEC, KEY_A_PUB, 1, "hello", created_at=4000)
        await send_event(ws, ev)

        result = await count_events(ws, {"authors": [KEY_A_PUB]})

        report("No HLL: count present",
               result.get("count") is not None,
               f"result={result}")

        report("No HLL: hll absent for non-tag filter",
               "hll" not in result,
               f"result keys={list(result.keys())}")
    finally:
        await ws.close()


async def test_hll_zero_results():
    """COUNT with 0 matching events should return all-zero hll."""
    ws = await connect()
    try:
        # Use a target that has no events
        fake_id = "0000000000000000000000000000000000000000000000000000000000000000"
        result = await count_events(ws, {"#e": [fake_id], "kinds": [7]})

        report("Zero results: count is 0",
               result.get("count") == 0,
               f"count={result.get('count')}")

        hll_hex = result.get("hll", "")
        expected_zero = "0" * 512

        report("Zero results: hll is all zeros",
               hll_hex == expected_zero,
               f"hll={hll_hex[:40]}...")
    finally:
        await ws.close()


async def test_no_hll_multiple_tags():
    """Filter with multiple tag types should NOT include hll."""
    ws = await connect()
    try:
        target = make_event(KEY_A_SEC, KEY_A_PUB, 1, "target", created_at=5000)
        await send_event(ws, target)

        ev = make_event(KEY_B_SEC, KEY_B_PUB, 7, "+",
                        tags=[["e", target["id"]], ["p", KEY_A_PUB]],
                        created_at=5001)
        await send_event(ws, ev)

        # COUNT with both #e and #p — not eligible for HLL
        result = await count_events(ws, {"#e": [target["id"]], "#p": [KEY_A_PUB]})

        report("Multi-tag: hll absent",
               "hll" not in result,
               f"result keys={list(result.keys())}")
    finally:
        await ws.close()


# ── Runner ──────────────────────────────────────────────────────────────────

async def run_tests():
    global passed, failed

    print("\n=== NIP-45 HLL COUNT Tests ===\n")

    await run_with_fresh_relay(test_hll_e_tag_reactions)
    await run_with_fresh_relay(test_hll_p_tag_follows)
    await run_with_fresh_relay(test_no_hll_without_tags)
    await run_with_fresh_relay(test_hll_zero_results)
    await run_with_fresh_relay(test_no_hll_multiple_tags)

    # Cleanup
    if os.path.exists(DB_DIR):
        shutil.rmtree(DB_DIR)

    print(f"\n{'='*50}")
    print(f"  {passed} passed, {failed} failed")
    if errors:
        print(f"\n  Failures:")
        for name, detail in errors:
            print(f"    - {name}: {detail}")
    print(f"{'='*50}\n")
    return failed == 0


if __name__ == "__main__":
    KEY_A_PUB = compute_pubkey(KEY_A_SEC)
    KEY_B_PUB = compute_pubkey(KEY_B_SEC)
    KEY_C_PUB = compute_pubkey(KEY_C_SEC)
    KEY_D_PUB = compute_pubkey(KEY_D_SEC)
    ok = asyncio.run(run_tests())
    sys.exit(0 if ok else 1)
