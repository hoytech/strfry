# strfry testing docs

Tests should be run from the *root* of the project.

## Tests for event writing, including replacements, deletions, etc:

    perl test/writeTest.pl

Note that this script relies on [`nostril`](https://github.com/jb55/nostril) being installed in your path.

## Fuzz tests

Note that these tests need a well populated DB. For best coverage, use the [wellordered 500k](https://wiki.wellorder.net/wiki/nostr-datasets/) data-set:

    zstdcat ../nostr-dumps/nostr-wellorder-early-500k-v1.jsonl.zst | ./strfry import

If successful, these tests will run forever, so you can run them overnight to see if any errors occur.

The tests are deterministic, but you can change the seed by setting the `SEED` env variable.

These commands test the query engine, with and without `limit`:

    perl test/filterFuzzTest.pl scan-limit
    perl test/filterFuzzTest.pl scan

These commands test the monitor engine:

    perl test/filterFuzzTest.pl monitor

## NIP-62 (Request to Vanish) E2E tests

These tests start a live strfry relay and exercise the full NIP-62 lifecycle
over websocket: vanish requests, cron-based event deletion, re-broadcast
prevention, gift wrap cleanup, relay tag validation, and more.

Requires Python 3.8+ with `secp256k1` and `websockets`:

    pip install secp256k1 websockets
    python3 test/nip62_e2e_test.py

The suite takes ~3 minutes (cron sweep polling at 30s intervals for 5 tests).
