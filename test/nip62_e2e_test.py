#!/usr/bin/env python3
"""
NIP-62 (Request to Vanish) End-to-End Tests for strfry relay.

Requires: pip install secp256k1 websockets

Tests the full websocket lifecycle against a live strfry relay:
  - Submit events via EVENT, query via REQ
  - Submit kind 62 vanish requests (ALL_RELAYS + specific relay URL)
  - Verify event deletion by cron sweep (polled, not fixed sleep)
  - Verify re-broadcast prevention (vanish marker persistence)
  - Verify relay tag validation (matching, non-matching, missing)
  - Verify NIP-11 advertisement of NIP-62
  - Verify kind 5 cannot delete kind 62 (both orderings)
  - Verify gift wrap (kind 1059) cleanup and blocking
  - Verify NIP-09 deletion events are also swept
  - Verify non-matching relay URL does NOT set vanish marker
  - Verify cross-pubkey isolation
  - Verify persistence across relay restart
  - Verify feature flag (nip62.enabled = false)
"""

import asyncio
import hashlib
import json
import os
import signal
import shutil
import subprocess
import sys
import time
import urllib.request

import secp256k1
import websockets


# ── Keys ────────────────────────────────────────────────────────────────────

KEY_A_SEC = bytes.fromhex("c1eee22f68dc218d98263cfecb350db6fc6b3e836b47423b66c62af7ae3e32bb")
KEY_A_PUB = "003ba9b2c5bd8afeed41a4ce362a8b7fc3ab59c25b6a1359cae9093f296dac01"

KEY_B_SEC = bytes.fromhex("a0b459d9ff90e30dc9d1749b34c4401dfe80ac2617c7732925ff994e8d5203ff")
KEY_B_PUB = "cc49e2a58373abc226eee84bee9ba954615aa2ef1563c4f955a74c4606a3b1fa"

# Third key for gift wrap author (simulates one-time-use pubkey per NIP-59)
KEY_C_SEC = bytes.fromhex("3fa463bf3a6b0e4c8d9a1e7b2f5c6d8e0a1b3c4d5e6f7a8b9c0d1e2f3a4b5c6d")
KEY_C_PUB = None  # computed at startup


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
    """Send an EVENT and return its OK response, skipping any interleaved frames."""
    await ws.send(json.dumps(["EVENT", event]))
    deadline = time.monotonic() + 10
    while time.monotonic() < deadline:
        resp = json.loads(await asyncio.wait_for(ws.recv(), timeout=5))
        if resp[0] == "OK" and resp[1] == event["id"]:
            return resp
        # Skip interleaved EVENT/EOSE/NOTICE frames from active subscriptions
    raise TimeoutError(f"no OK for event {event['id']} within 10s")


async def query_events(ws, filt, sub_id="test"):
    """Send a REQ, collect all EVENT frames until EOSE, then CLOSE the sub."""
    await ws.send(json.dumps(["REQ", sub_id, filt]))
    events = []
    while True:
        resp = json.loads(await asyncio.wait_for(ws.recv(), timeout=5))
        if resp[0] == "EOSE" and resp[1] == sub_id:
            break
        if resp[0] == "EVENT" and resp[1] == sub_id:
            events.append(resp[2])
    await ws.send(json.dumps(["CLOSE", sub_id]))
    return events


async def poll_until(ws, filt, condition, timeout=45, interval=1, sub_prefix="poll"):
    """Poll query_events until condition(events) is True or timeout."""
    deadline = time.monotonic() + timeout
    seq = 0
    while time.monotonic() < deadline:
        seq += 1
        evts = await query_events(ws, filt, sub_id=f"{sub_prefix}{seq}")
        if condition(evts):
            return evts
        await asyncio.sleep(interval)
    # Return last result even on timeout so caller can report details
    return evts


async def connect(port=40562):
    return await websockets.connect(f"ws://127.0.0.1:{port}")


# ── Process management ──────────────────────────────────────────────────────

STRFRY = "./strfry"
DB_DIR = "./strfry-db-nip62-test"
CONF = "test/cfgs/nip62Test.conf"
CONF_DISABLED = "test/cfgs/nip62TestDisabled.conf"
PORT = 40562


def clean_db():
    if os.path.exists(DB_DIR):
        shutil.rmtree(DB_DIR)
    os.makedirs(DB_DIR, exist_ok=True)


def start_relay(conf=CONF):
    import socket
    # Verify port is free before starting — detect stale processes or conflicts
    try:
        s = socket.create_connection(("127.0.0.1", PORT), timeout=0.1)
        s.close()
        raise RuntimeError(f"port {PORT} already in use before starting relay")
    except (ConnectionRefusedError, OSError):
        pass  # good — port is free

    proc = subprocess.Popen(
        [STRFRY, "--config", conf, "relay"],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    # Poll until our relay is accepting connections (up to 5s)
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
    """Run a single test with a fresh DB and relay instance."""
    clean_db()
    relay = start_relay(conf)
    try:
        await test_fn()
    finally:
        stop_relay(relay)


# ── Tests ───────────────────────────────────────────────────────────────────

async def test_nip11_advertises_62():
    """NIP-11 response should include 62 in supported_nips when enabled."""
    req = urllib.request.Request(
        f"http://127.0.0.1:{PORT}",
        headers={"Accept": "application/nostr+json"},
    )
    with urllib.request.urlopen(req, timeout=5) as resp:
        info = json.loads(resp.read())
    ok = 62 in info.get("supported_nips", [])
    report("NIP-11 advertises NIP-62", ok,
           f"supported_nips={info.get('supported_nips')}")


async def test_vanish_all_relays():
    """Kind 62 with ALL_RELAYS deletes old events, allows new ones."""
    ws = await connect()
    try:
        ev1 = make_event(KEY_A_SEC, KEY_A_PUB, 1, "hello", created_at=5000)
        ev2 = make_event(KEY_A_SEC, KEY_A_PUB, 1, "world", created_at=5500)
        r1 = await send_event(ws, ev1)
        r2 = await send_event(ws, ev2)
        report("Pre-vanish: events accepted",
               r1[2] is True and r2[2] is True,
               f"r1={r1} r2={r2}")

        evts = await query_events(ws, {"authors": [KEY_A_PUB]})
        report("Pre-vanish: events queryable",
               len(evts) == 2,
               f"got {len(evts)} events, expected 2")

        vanish = make_event(KEY_A_SEC, KEY_A_PUB, 62, "goodbye",
                            tags=[["relay", "ALL_RELAYS"]], created_at=6000)
        rv = await send_event(ws, vanish)
        report("Vanish event accepted", rv[2] is True, f"rv={rv}")

        # Poll until cron deletes old events
        print("    ... polling for cron sweep")
        evts = await poll_until(
            ws, {"authors": [KEY_A_PUB]},
            lambda e: ev1["id"] not in {x["id"] for x in e},
        )
        ids = {e["id"] for e in evts}
        report("Post-cron: old events deleted",
               ev1["id"] not in ids and ev2["id"] not in ids,
               f"ev1 present={ev1['id'] in ids}, ev2 present={ev2['id'] in ids}")
        report("Post-cron: kind 62 preserved",
               vanish["id"] in ids,
               f"vanish present={vanish['id'] in ids}")

        ev_new = make_event(KEY_A_SEC, KEY_A_PUB, 1, "I'm back", created_at=7000)
        rn = await send_event(ws, ev_new)
        report("Post-vanish: new event (ts>vanish) accepted",
               rn[2] is True, f"rn={rn}")

        ev_old = make_event(KEY_A_SEC, KEY_A_PUB, 1, "old stuff", created_at=5800)
        ro = await send_event(ws, ev_old)
        report("Post-vanish: old event (ts<=vanish) blocked",
               ro[2] is False, f"ro={ro}")
    finally:
        await ws.close()


async def test_vanish_specific_relay():
    """Kind 62 targeting this relay's serviceUrl is accepted."""
    ws = await connect()
    try:
        ev1 = make_event(KEY_A_SEC, KEY_A_PUB, 1, "msg", created_at=5000)
        await send_event(ws, ev1)

        vanish = make_event(KEY_A_SEC, KEY_A_PUB, 62, "vanish me",
                            tags=[["relay", f"ws://127.0.0.1:{PORT}"]], created_at=6000)
        rv = await send_event(ws, vanish)
        report("Vanish with matching serviceUrl accepted",
               rv[2] is True, f"rv={rv}")
    finally:
        await ws.close()


async def test_vanish_wrong_relay_rejected():
    """Kind 62 targeting a different relay URL is rejected."""
    ws = await connect()
    try:
        vanish = make_event(KEY_A_SEC, KEY_A_PUB, 62, "wrong relay",
                            tags=[["relay", "wss://other-relay.example.com"]], created_at=6000)
        rv = await send_event(ws, vanish)
        report("Vanish targeting wrong relay rejected",
               rv[2] is False and "not targeting" in str(rv[3]),
               f"rv={rv}")
    finally:
        await ws.close()


async def test_vanish_wrong_relay_no_marker():
    """Non-matching relay tag must NOT set a vanish marker; events remain accepted."""
    ws = await connect()
    try:
        # Send vanish targeting a *different* relay
        vanish = make_event(KEY_A_SEC, KEY_A_PUB, 62, "not for us",
                            tags=[["relay", "wss://other-relay.example.com"]], created_at=6000)
        rv = await send_event(ws, vanish)
        report("Wrong-relay vanish rejected (precondition)",
               rv[2] is False, f"rv={rv}")

        # Old event from same pubkey should still be accepted (no marker set)
        ev = make_event(KEY_A_SEC, KEY_A_PUB, 1, "still works", created_at=5000)
        re = await send_event(ws, ev)
        report("No vanish marker: old event still accepted",
               re[2] is True, f"re={re}")
    finally:
        await ws.close()


async def test_vanish_no_relay_tag_rejected():
    """Kind 62 with no relay tag is rejected."""
    ws = await connect()
    try:
        vanish = make_event(KEY_A_SEC, KEY_A_PUB, 62, "no relay tag",
                            tags=[], created_at=6000)
        rv = await send_event(ws, vanish)
        report("Vanish with no relay tag rejected",
               rv[2] is False and "not targeting" in str(rv[3]),
               f"rv={rv}")
    finally:
        await ws.close()


async def test_vanish_cross_pubkey_isolation():
    """Vanish for pubkey A does not affect pubkey B events."""
    ws = await connect()
    try:
        ev_a = make_event(KEY_A_SEC, KEY_A_PUB, 1, "from A", created_at=5000)
        ev_b = make_event(KEY_B_SEC, KEY_B_PUB, 1, "from B", created_at=5000)
        await send_event(ws, ev_a)
        await send_event(ws, ev_b)

        vanish = make_event(KEY_A_SEC, KEY_A_PUB, 62, "A vanishes",
                            tags=[["relay", "ALL_RELAYS"]], created_at=6000)
        await send_event(ws, vanish)

        print("    ... polling for cron sweep")
        await poll_until(
            ws, {"authors": [KEY_A_PUB]},
            lambda e: ev_a["id"] not in {x["id"] for x in e},
        )

        evts_b = await query_events(ws, {"authors": [KEY_B_PUB]})
        report("Cross-pubkey: B events unaffected",
               len(evts_b) == 1 and evts_b[0]["id"] == ev_b["id"],
               f"got {len(evts_b)} B events")

        evts_a = await query_events(ws, {"authors": [KEY_A_PUB]})
        a_ids = {e["id"] for e in evts_a}
        report("Cross-pubkey: A old event deleted",
               ev_a["id"] not in a_ids,
               f"ev_a present={ev_a['id'] in a_ids}")
        report("Cross-pubkey: A vanish preserved",
               vanish["id"] in a_ids,
               f"vanish present={vanish['id'] in a_ids}")

        ev_b2 = make_event(KEY_B_SEC, KEY_B_PUB, 1, "B still here", created_at=7000)
        rb = await send_event(ws, ev_b2)
        report("Cross-pubkey: B can still post",
               rb[2] is True, f"rb={rb}")
    finally:
        await ws.close()


async def test_kind5_cannot_delete_kind62():
    """Kind 5 deletion event cannot delete a kind 62 vanish event (kind 62 first)."""
    ws = await connect()
    try:
        vanish = make_event(KEY_A_SEC, KEY_A_PUB, 62, "protect me",
                            tags=[["relay", "ALL_RELAYS"]], created_at=5000)
        await send_event(ws, vanish)

        deletion = make_event(KEY_A_SEC, KEY_A_PUB, 5, "try delete",
                              tags=[["e", vanish["id"]]], created_at=6000)
        await send_event(ws, deletion)

        await asyncio.sleep(2)

        evts = await query_events(ws, {"ids": [vanish["id"]]})
        report("Kind 5 after kind 62: vanish event survives",
               len(evts) == 1 and evts[0]["id"] == vanish["id"],
               f"got {len(evts)} events")
    finally:
        await ws.close()


async def test_kind5_tombstone_does_not_block_kind62():
    """A deletion tombstone keyed to a kind 62 event's own ID must not block it.

    Regression test for commit 97f2124 (events.cpp:274 guard).
    The deletion index is populated automatically when a kind 5 event is
    stored (golpe.yaml indexPrelude). To exercise the vulnerable code path:
      1. Compute the kind 62 event's ID without sending it yet
      2. Send a kind 5 that e-tags that ID → creates tombstone (id, pubkey)
      3. Send the kind 62 → without the fix, the tombstone would block it

    This is done via strfry import (single batch) to get deterministic
    ordering: both events enter one writeEvents() call, with the kind 5
    (created_at=4000) sorted before the kind 62 (created_at=5000).
    """
    # Step 1: Build the kind 62 event to learn its ID
    vanish = make_event(KEY_A_SEC, KEY_A_PUB, 62, "vanish",
                        tags=[["relay", "ALL_RELAYS"]], created_at=5000)

    # Step 2: Build a kind 5 that references the kind 62 event's ID
    deletion = make_event(KEY_A_SEC, KEY_A_PUB, 5, "tombstone for vanish",
                          tags=[["e", vanish["id"]]], created_at=4000)

    # Step 3: Import both in one batch (kind 5 first by created_at sort)
    # writeEvents sorts by created_at, so deletion (4000) is processed before vanish (5000)
    batch = json.dumps(deletion) + "\n" + json.dumps(vanish) + "\n"
    result = subprocess.run(
        [STRFRY, "--config", CONF, "import"],
        input=batch, capture_output=True, text=True, timeout=10,
    )

    # Step 4: Export and verify both events exist
    export = subprocess.run(
        [STRFRY, "--config", CONF, "export"],
        capture_output=True, text=True, timeout=10,
    )
    exported_ids = set()
    for line in export.stdout.strip().split("\n"):
        if line:
            exported_ids.add(json.loads(line)["id"])

    report("Tombstone-producing kind 5 imported (precondition)",
           deletion["id"] in exported_ids,
           f"kind5 present={deletion['id'] in exported_ids}")
    report("Kind 62 survives its own tombstone (import path)",
           vanish["id"] in exported_ids,
           f"vanish present={vanish['id'] in exported_ids}")


async def test_multiple_vanish_max_timestamp():
    """Multiple vanish requests: the max timestamp is used."""
    ws = await connect()
    try:
        ev_early = make_event(KEY_A_SEC, KEY_A_PUB, 1, "early", created_at=3000)
        ev_mid = make_event(KEY_A_SEC, KEY_A_PUB, 1, "mid", created_at=5000)
        ev_late = make_event(KEY_A_SEC, KEY_A_PUB, 1, "late", created_at=8000)
        await send_event(ws, ev_early)
        await send_event(ws, ev_mid)
        await send_event(ws, ev_late)

        v1 = make_event(KEY_A_SEC, KEY_A_PUB, 62, "vanish1",
                         tags=[["relay", "ALL_RELAYS"]], created_at=4000)
        await send_event(ws, v1)

        v2 = make_event(KEY_A_SEC, KEY_A_PUB, 62, "vanish2",
                         tags=[["relay", "ALL_RELAYS"]], created_at=6000)
        await send_event(ws, v2)

        print("    ... polling for cron sweep")
        evts = await poll_until(
            ws, {"authors": [KEY_A_PUB]},
            lambda e: ev_mid["id"] not in {x["id"] for x in e},
        )
        ids = {e["id"] for e in evts}
        report("Multiple vanish: early event deleted",
               ev_early["id"] not in ids,
               f"early present={ev_early['id'] in ids}")
        report("Multiple vanish: mid event deleted",
               ev_mid["id"] not in ids,
               f"mid present={ev_mid['id'] in ids}")
        report("Multiple vanish: late event (ts>max vanish) survives",
               ev_late["id"] in ids,
               f"late present={ev_late['id'] in ids}")
        report("Multiple vanish: both vanish events preserved",
               v1["id"] in ids and v2["id"] in ids,
               f"v1={v1['id'] in ids} v2={v2['id'] in ids}")
    finally:
        await ws.close()


async def test_vanish_exact_timestamp():
    """Events at exact vanish timestamp are blocked."""
    ws = await connect()
    try:
        vanish = make_event(KEY_A_SEC, KEY_A_PUB, 62, "vanish",
                            tags=[["relay", "ALL_RELAYS"]], created_at=5000)
        await send_event(ws, vanish)

        ev_exact = make_event(KEY_A_SEC, KEY_A_PUB, 1, "exact ts", created_at=5000)
        re = await send_event(ws, ev_exact)
        report("Exact timestamp blocked",
               re[2] is False, f"re={re}")

        ev_after = make_event(KEY_A_SEC, KEY_A_PUB, 1, "just after", created_at=5001)
        ra = await send_event(ws, ev_after)
        report("Timestamp+1 accepted",
               ra[2] is True, f"ra={ra}")
    finally:
        await ws.close()


async def test_nip09_deletion_events_swept():
    """NIP-09 kind 5 deletion events authored by the vanishing pubkey are also deleted.

    Spec: 'delete everything, including NIP-09 Deletion Events, from the .pubkey'
    """
    ws = await connect()
    try:
        ev = make_event(KEY_A_SEC, KEY_A_PUB, 1, "target", created_at=4000)
        await send_event(ws, ev)

        # Kind 5 deletion by pubkey A
        del_ev = make_event(KEY_A_SEC, KEY_A_PUB, 5, "delete target",
                            tags=[["e", ev["id"]]], created_at=4500)
        await send_event(ws, del_ev)

        # Verify the deletion event exists
        evts = await query_events(ws, {"ids": [del_ev["id"]]})
        report("NIP-09 deletion event exists pre-vanish",
               len(evts) == 1, f"got {len(evts)}")

        # Vanish
        vanish = make_event(KEY_A_SEC, KEY_A_PUB, 62, "vanish all",
                            tags=[["relay", "ALL_RELAYS"]], created_at=5000)
        await send_event(ws, vanish)

        print("    ... polling for cron sweep")
        evts = await poll_until(
            ws, {"ids": [del_ev["id"]]},
            lambda e: len(e) == 0,
        )
        report("NIP-09 deletion event swept by vanish",
               len(evts) == 0,
               f"got {len(evts)} deletion events remaining")
    finally:
        await ws.close()


async def test_gift_wrap_deletion():
    """Kind 1059 gift wraps addressed to vanished pubkey should be deleted."""
    ws = await connect()
    try:
        # Use KEY_C as gift wrap author (simulates one-time-use pubkey per NIP-59)
        gw = make_event(KEY_C_SEC, KEY_C_PUB, 1059, "encrypted dm",
                         tags=[["p", KEY_A_PUB]], created_at=5000)
        rg = await send_event(ws, gw)
        report("Gift wrap accepted", rg[2] is True, f"rg={rg}")

        ev_b = make_event(KEY_B_SEC, KEY_B_PUB, 1, "B normal", created_at=5000)
        await send_event(ws, ev_b)

        vanish = make_event(KEY_A_SEC, KEY_A_PUB, 62, "vanish",
                            tags=[["relay", "ALL_RELAYS"]], created_at=6000)
        await send_event(ws, vanish)

        print("    ... polling for cron sweep")
        await poll_until(
            ws, {"ids": [gw["id"]]},
            lambda e: len(e) == 0,
        )

        evts_gw = await query_events(ws, {"ids": [gw["id"]]})
        report("Gift wrap to vanished pubkey deleted",
               len(evts_gw) == 0,
               f"got {len(evts_gw)} gift wraps")

        evts_b = await query_events(ws, {"ids": [ev_b["id"]]})
        report("B regular event survives",
               len(evts_b) == 1,
               f"got {len(evts_b)} B events")
    finally:
        await ws.close()


async def test_new_gift_wrap_blocked():
    """New gift wraps to a vanished pubkey should be rejected."""
    ws = await connect()
    try:
        vanish = make_event(KEY_A_SEC, KEY_A_PUB, 62, "vanish",
                            tags=[["relay", "ALL_RELAYS"]], created_at=6000)
        await send_event(ws, vanish)

        await asyncio.sleep(2)

        gw = make_event(KEY_B_SEC, KEY_B_PUB, 1059, "new gw",
                         tags=[["p", KEY_A_PUB]], created_at=7000)
        rg = await send_event(ws, gw)
        report("New gift wrap to vanished pubkey blocked",
               rg[2] is False, f"rg={rg}")
    finally:
        await ws.close()


async def test_gift_wrap_blocked_specific_relay():
    """New gift wraps blocked when vanish uses specific relay URL (not just ALL_RELAYS)."""
    ws = await connect()
    try:
        vanish = make_event(KEY_A_SEC, KEY_A_PUB, 62, "vanish specific",
                            tags=[["relay", f"ws://127.0.0.1:{PORT}"]], created_at=6000)
        rv = await send_event(ws, vanish)
        report("Specific-relay vanish accepted (precondition)",
               rv[2] is True, f"rv={rv}")

        await asyncio.sleep(2)

        gw = make_event(KEY_C_SEC, KEY_C_PUB, 1059, "gw after specific vanish",
                         tags=[["p", KEY_A_PUB]], created_at=7000)
        rg = await send_event(ws, gw)
        report("Gift wrap blocked after specific-relay vanish",
               rg[2] is False, f"rg={rg}")
    finally:
        await ws.close()


async def test_vanish_persists_across_restart():
    """Vanish marker survives relay restart; old events still blocked."""
    ws = await connect()
    try:
        vanish = make_event(KEY_A_SEC, KEY_A_PUB, 62, "persist",
                            tags=[["relay", "ALL_RELAYS"]], created_at=6000)
        rv = await send_event(ws, vanish)
        report("Persistence: vanish event accepted",
               rv[2] is True, f"rv={rv}")
    finally:
        await ws.close()


async def test_vanish_persists_after_restart():
    """After restart, the vanish marker should still block old events."""
    ws = await connect()
    try:
        ev_old = make_event(KEY_A_SEC, KEY_A_PUB, 1, "old after restart", created_at=5000)
        ro = await send_event(ws, ev_old)
        report("Post-restart: old event still blocked",
               ro[2] is False, f"ro={ro}")

        ev_new = make_event(KEY_A_SEC, KEY_A_PUB, 1, "new after restart", created_at=7000)
        rn = await send_event(ws, ev_new)
        report("Post-restart: new event accepted",
               rn[2] is True, f"rn={rn}")
    finally:
        await ws.close()


# ── Tests for NIP-62 disabled ──────────────────────────────────────────────

async def test_nip62_disabled_rejects():
    """When NIP-62 is disabled, kind 62 events are rejected via websocket."""
    ws = await connect()
    try:
        vanish = make_event(KEY_A_SEC, KEY_A_PUB, 62, "vanish",
                            tags=[["relay", "ALL_RELAYS"]], created_at=6000)
        rv = await send_event(ws, vanish)
        report("NIP-62 disabled: vanish rejected",
               rv[2] is False and "not enabled" in str(rv[3]),
               f"rv={rv}")
    finally:
        await ws.close()


async def test_nip11_no_62_when_disabled():
    """NIP-11 should NOT include 62 when disabled."""
    req = urllib.request.Request(
        f"http://127.0.0.1:{PORT}",
        headers={"Accept": "application/nostr+json"},
    )
    with urllib.request.urlopen(req, timeout=5) as resp:
        info = json.loads(resp.read())
    ok = 62 not in info.get("supported_nips", [])
    report("NIP-11 does NOT advertise 62 when disabled", ok,
           f"supported_nips={info.get('supported_nips')}")


# ── Runner ──────────────────────────────────────────────────────────────────

async def run_tests():
    global passed, failed

    print("\n=== NIP-62 E2E Tests (NIP-62 enabled) ===\n")

    # Phase 1: Quick tests (no cron wait) — each gets fresh relay
    await run_with_fresh_relay(test_nip11_advertises_62)
    await run_with_fresh_relay(test_vanish_specific_relay)
    await run_with_fresh_relay(test_vanish_wrong_relay_rejected)
    await run_with_fresh_relay(test_vanish_wrong_relay_no_marker)
    await run_with_fresh_relay(test_vanish_no_relay_tag_rejected)
    await run_with_fresh_relay(test_kind5_cannot_delete_kind62)
    await run_with_fresh_relay(test_kind5_tombstone_does_not_block_kind62)
    await run_with_fresh_relay(test_vanish_exact_timestamp)
    await run_with_fresh_relay(test_new_gift_wrap_blocked)
    await run_with_fresh_relay(test_gift_wrap_blocked_specific_relay)

    # Phase 2: Cron-dependent tests (poll for completion)
    await run_with_fresh_relay(test_vanish_all_relays)
    await run_with_fresh_relay(test_vanish_cross_pubkey_isolation)
    await run_with_fresh_relay(test_multiple_vanish_max_timestamp)
    await run_with_fresh_relay(test_gift_wrap_deletion)
    await run_with_fresh_relay(test_nip09_deletion_events_swept)

    # Phase 3: Persistence across restart (shares DB between two relay runs)
    clean_db()
    relay = start_relay()
    try:
        await test_vanish_persists_across_restart()
    finally:
        stop_relay(relay)
    relay = start_relay()
    try:
        await test_vanish_persists_after_restart()
    finally:
        stop_relay(relay)

    # Phase 4: NIP-62 disabled
    print("\n=== NIP-62 E2E Tests (NIP-62 disabled) ===\n")
    await run_with_fresh_relay(test_nip62_disabled_rejects, conf=CONF_DISABLED)
    await run_with_fresh_relay(test_nip11_no_62_when_disabled, conf=CONF_DISABLED)

    # Cleanup — remove test DB entirely
    if os.path.exists(DB_DIR):
        shutil.rmtree(DB_DIR)

    # Summary
    print(f"\n{'='*50}")
    print(f"  {passed} passed, {failed} failed")
    if errors:
        print(f"\n  Failures:")
        for name, detail in errors:
            print(f"    - {name}: {detail}")
    print(f"{'='*50}\n")
    return failed == 0


if __name__ == "__main__":
    KEY_C_PUB = compute_pubkey(KEY_C_SEC)
    ok = asyncio.run(run_tests())
    sys.exit(0 if ok else 1)
