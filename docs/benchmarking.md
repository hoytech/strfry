# Strfry Benchmarking & Performance Profiling

This document describes the strfry benchmarking framework, profiling methodologies, A/B comparison tools, and optimization analysis.

---

## 1. Overview & Architecture

The benchmarking infrastructure lives in the `bench/` directory and consists of:

- **`bench/run_stats.py`**: The primary benchmarking orchestrator. It controls relay lifecycles, seeds databases, executes workload suites using the high-performance Rust benchmark client (`strfry-bench`), monitors system resources (`/proc/pid/status`, `/proc/pid/io`, `/proc/net/tcp`), and generates structured Markdown reports (`benchmark_report.md`).
- **`bench/ab_test.py`**: Relative A/B comparison runner. Stashes current branch changes, checks out the base branch (default: `master`), builds and benchmarks both branches under identical test parameters, collects hardware-agnostic metrics, and produces a delta report (`benchmark_comparison.md`).
- **`bench/alloc_tracker.c`**: Shared library (`LD_PRELOAD`) hooked via `dlsym` to record exact heap allocation counts and total bytes allocated during execution without modifying strfry binaries.
- **`strfry-bench`**: A multi-threaded Rust client designed to stress test Nostr relays via WebSocket connections (`ws://`), simulating events, NIP-45 counts, subscription fan-out, connection storms, high churn, and malicious traffic.

---

## 2. Benchmark Suites

The orchestrator executes 12 distinct benchmark suites:

1. **Storage (In-Core vs Out-of-Core)**: Measures sequential scan throughput and pagination latencies when the database fits in memory (In-Core) vs under strict memory limits (256MB Docker container out-of-core pagination).
2. **Event Ingestion Pipeline**: Benchmarks write throughput (events/sec) for small (50B) payloads, large (10KB) payloads, and single-connection spam.
3. **Concurrency & Thread Pool**: Evaluates REQ query performance while heavy background write pressure is applied.
4. **WebSockets & Connections**: Tests connection establishment storms (100, 500, 5000 connections), memory footprint per connection (VmRSS), high connection churn rate, and OS `TIME_WAIT` socket cleanup.
5. **Query Engine & NIP-45 Indices**: Measures exact point lookup latency, NIP-45 `COUNT` performance, and complex multi-field NIP-01 filter queries.
6. **Active Monitors (Viral Post Fanout)**: Simulates subscription fan-out across dozens/hundreds of connected clients.
7. **Negentropy Sync**: Benchmarks NIP-77 set reconciliation synchronization speed and events/sec throughput between relays.
8. **Write Policy Plugin**: Evaluates IPC overhead when strfry delegates event validation to external policy plugins (`relay.writePolicy.plugin`).
9. **CLI & Dictionary Compression**: Measures `strfry import`, `strfry export`, and `strfry dict train` Zstd compression training performance.
10. **OS-Level Metrics**: Tracks baseline memory RSS, CPU utilization %, physical disk reads/writes, and Write Amplification Factor (WAF).
11. **Stress & Edge Cases**: Exercises adversarial workloads including Slow Loris connection holding and Signature Flood attacks.
12. **Backpressure Performance**: Tests event distribution when fast and slow WebSocket clients subscribe to the same feed.

---

## 3. Running Benchmarks & A/B Comparisons

### Running All Benchmark Suites

```bash
python3 bench/run_stats.py
```

To skip the heavy 1-million-event database generation for faster local test runs:

```bash
python3 bench/run_stats.py --skip-heavy
```

To run a single specific suite (e.g. `websockets`, `ingestion`, `queries`, `negentropy`):

```bash
python3 bench/run_stats.py --suite websockets
```

### Running A/B Relative Comparison

To compare your feature branch against `master`:

```bash
python3 bench/ab_test.py --base master
```

To run A/B comparison with lightweight dataset sizes:

```bash
python3 bench/ab_test.py --base master --skip-heavy
```

### Pre-PR Full Comparison

Before opening a pull request, run the **full** A/B comparison to generate a definitive regression report covering all suites including the 1M-event storage test:

```bash
python3 bench/ab_test.py --base master --full
```

The `--full` flag overrides `--skip-heavy` and ensures every suite runs with production-scale dataset sizes. The output report `benchmark_comparison.md` displays delta percentages (%) and status indicators:
- **Deterministic Metrics**: Retired CPU instructions (via `perf stat`), heap allocation count, and total bytes allocated (via `alloc_tracker.c`).
- **Wall-Clock & Resource Metrics**: Throughput, latencies (P50, P90, P95, P99), RSS memory growth, and disk WAF.

---

## 4. Flamegraphs & Profiling

Profiling strfry under heavy load helps pinpoint CPU bottlenecks and memory allocation hotspots.

### Generating Flamegraphs with `perf` and `inferno`

1. Install `inferno` (if not already installed):
   ```bash
   cargo install inferno
   ```

2. Record profile with `perf`:
   ```bash
   perf record -o perf_import.data -F 99 -g -- ./strfry import --no-verify < bench/test_seed.jsonl
   ```

3. Collapse stacks and render SVG flamegraph:
   ```bash
   perf script -i perf_import.data | inferno-collapse-perf | inferno-flamegraph > flamegraph_import.svg
   ```

---

## 5. Identified Hotspots & Optimization Opportunities

Profile data gathered from `perf stat`, flamegraphs, and allocation tracking highlights key areas:

1. **Negentropy Build Transaction Windowing (Resolved in PR)**:
   - *Problem*: `strfry negentropy build` previously held a single write transaction (`txn_rw`) across the entire DB scan and tree construction phase, blocking concurrent relay writes.
   - *Optimization*: Refactored to scan matching events in read-only batches (`txn_ro`) of 10,000 events, followed by short write transaction windows (`txn_rw`). This reduced lock contention while improving negentropy sync throughput.

2. **JSON AST Allocation Overhead**:
   - *Hotspot*: `tao::json` creates heap-allocated C++ `variant` structures for every parsed JSON key, value, and tag array during event ingestion.
   - *Opportunity*: Streaming or zero-copy JSON parsing directly into `PackedEventBuilder` avoids intermediate `basic_value` DOM allocations during high-rate ingestion.

3. **LMDB Secondary Index Insertion**:
   - *Hotspot*: Writing secondary index entries (`Event__pubkey`, `Event__created_at`, `Event__kind`, `Event__tag`) during bulk import incurs CPU overhead in string comparison (`lmdb_comparator__StringUint64`).
   - *Opportunity*: Pre-sorting events or index keys prior to insertion minimizes LMDB page splitting during initial bulk imports.
