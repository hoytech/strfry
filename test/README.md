# strfry Testing Guide

Tests should be run from the **root** of the project after building strfry:

```bash
make -j4
```

---

## Write Tests

Tests event writing, including replacements (NIP-09 deletions, replaceable events, parameterised replaceable events) and edge cases:

```bash
perl test/writeTest.pl
```

> **Note:** This script requires [`nostril`](https://github.com/jb55/nostril) to be installed in your `$PATH`. Install it with:
> ```bash
> git clone https://github.com/jb55/nostril && cd nostril && make && sudo make install
> ```

---

## Fuzz Tests

These tests stress-test strfry's query engine and monitor engine by generating random filters and verifying that results are consistent and correct. They run indefinitely — stop them with `Ctrl+C` after a suitable period (overnight is recommended for thorough coverage).

### Dataset

For best coverage, use a well-populated DB. The [Wellordered 500k dataset](https://wiki.wellorder.net/wiki/nostr-datasets/) is recommended:

```bash
zstdcat ../nostr-dumps/nostr-wellorder-early-500k-v1.jsonl.zst | ./strfry import
```

If you do not have the dataset, you can populate the DB with any JSONL dump of nostr events:

```bash
cat your-events.jsonl | ./strfry import
```

### Determinism and Seeds

All fuzz tests are deterministic by default. To reproduce a specific failing seed, set the `SEED` environment variable:

```bash
SEED=12345 perl test/filterFuzzTest.pl scan
```

### Query Engine Tests

These commands test the DBScan query engine with and without the `limit` field:

```bash
# With limit (tests pagination and time-budget slicing)
perl test/filterFuzzTest.pl scan-limit

# Without limit (tests full scan correctness)
perl test/filterFuzzTest.pl scan
```

**What these test:** Random filters (various combinations of `kinds`, `authors`, `ids`, `since`, `until`, `limit`, and tag filters) are generated and executed against the DB. Results are compared against a naive reference implementation to verify correctness.

### Monitor Engine Tests

This command tests the `ReqMonitor` engine — the component that delivers newly written events to active subscribers in real time:

```bash
perl test/filterFuzzTest.pl monitor
```

**What this tests:** Random filters are registered as active subscriptions. Random events are then written to the DB, and the test verifies that the monitor correctly identifies which subscriptions each event should be delivered to, compared against a reference matcher.

---

## Sync Tests

These test the [negentropy](https://github.com/hoytech/negentropy)-based set reconciliation between two strfry instances:

```bash
perl test/runSyncTests.pl
```

> Requires two strfry instances to be running locally. See the test script for configuration details.

---

## Subscription ID Tests

Tests for subscription ID handling edge cases (length limits, special characters, duplicate handling):

Compiled from `test/SubIdTests.cpp`. Run after building:

```bash
./test/SubIdTests
```

---

## Filter Plugin Tests

Sample write policy plugins (Perl) are available in `test/plugins/` for testing the plugin interface. See [`docs/plugins.md`](../docs/plugins.md) for how to configure and use write policy plugins.

---

## Running All Tests (Quick Sanity Check)

For a quick sanity check before submitting a PR:

```bash
# 1. Build
make -j4

# 2. Write tests (requires nostril)
perl test/writeTest.pl

# 3. Short fuzz run (30 seconds each)
timeout 30 perl test/filterFuzzTest.pl scan-limit || true
timeout 30 perl test/filterFuzzTest.pl scan || true
timeout 30 perl test/filterFuzzTest.pl monitor || true
```

The fuzz tests do not "pass" in the traditional sense — they run until they find an error or are stopped. If they exit cleanly within the timeout, no errors were found.
