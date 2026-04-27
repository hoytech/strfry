Strfry Benchmark Suite — Plan and Structure

Purpose
- Measure strfry performance across DB sizes and workloads with NIP-50 disabled and enabled.
- Produce comparable Markdown reports that include sanitized system info (no PII).

Outcomes
- Repeatable, automated runs that generate:
  - Per-scenario Markdown report with metrics and system profile
  - Aggregated summary Markdown table across scenarios
- Clear separation of preparation (DB build) vs execution (load + measure)

High-Level Flow
1) Prepare: build test DB for each scenario
   - Generate cryptographically valid nostr events via nak
   - Ingest into a fresh strfry DB (separate directory per scenario)
   - Optionally pre-compress dictionaries and warm caches
2) Run: execute workload against the prepared DB
   - Start strfry with scenario config (NIP-50 on/off)
   - Run benchmark client to drive REQ/EVENT traffic
   - Capture server logs, resource stats, and client timings
3) Report: aggregate results
   - Parse logs and client output
   - Emit per-scenario report and a combined summary table

Repository Layout (bench/)
- bench/
  - README.md                 — this plan and usage
  - SCENARIOS.md              — curated scenarios list and guidance
  - scenarios/
    - small.yml               — ~100k events
    - medium.yml              — ~1M events
    - large.yml               — ~10M events (example)
- scripts/
    - prepare.sh              — build DB(s) for scenarios
    - run.sh                  — run benchmark(s) and gather metrics
    - sysinfo.sh              — collect sanitized system profile
    - report.py               — generate per-scenario + summary Markdown
  - client/                   — load generator (future; optional)
  - results/
    - raw/                    — JSON and logs per run
    - summary.md              — aggregated Markdown table
  - work/                     — ephemeral DBs and run artifacts

External Dependencies
- nak: Event generator that produces valid signed nostr events
  - Provide binary path via env `NAK_BIN` or place on PATH
- Tools: `bash`, `jq`, `awk`, `sed`, `python3` (for report.py)
- Optional: `mpstat`/`pidstat` (sysstat), `lsblk`, `lscpu` for system profiling

Scenario Format (YAML)
- name: human-readable name
- db:
  - events: integer (total events to generate)
  - kinds: pattern (e.g., "1,30023")
  - avg_event_size: bytes (approx)
  - keyword_inject_rate: float 0..1 (probability to inject keywords into generated event content)
  - keywords: list of { term, weight } to control frequency distribution and enable realistic search results
  - distribution: optional mix (e.g., replies/hashtags)
- server:
  - search_enabled: true|false
  - search_backend: lmdb|noop
  - config_overrides: map of strfry config keys/values
- workload:
  - duration_s: 120
  - warmup_s: 15
  - connections: 100
  - subscriptions_per_conn: 3
  - writers_per_sec: 200  # events/sec sent to relay
  - req_mix:
      - type: read
        filter: { kinds: [1], limit: 200 }
        weight: 3
      - type: search
        # if the client runner supports it, the search term will be sampled from db.keywords biased by weight
        filter: { kinds: [1], search: "best nostr apps", limit: 100 }
        weight: 1

Metrics
- Throughput: events/s sent; events/s delivered
- Latency: p50/p95/p99 for
  - Initial REQ scan to EOSE
  - EVENT -> OK roundtrip
  - EVENT observe-to-deliver (ingest to delivery to live subscribers)
- Resource:
  - strfry RSS/CPU (sampled), total system CPU/mem
  - DB size on disk
- Search (when enabled):
  - Search query latency p50/p95/p99
  - Index catch-up state at run start/end
  - Results cardinality across term classes (common vs rare), using weighted keywords

System Profile (sanitized)
- OS: kernel version, distro
- CPU: model, sockets, cores, MHz
- Memory: total
- Storage: device type (NVMe/SATA), rotational flag, filesystem
- Notes:
  - Do not record hostname, users, IP addresses, or MACs

Execution
- Prepare one or more scenarios:
  - `bench/scripts/prepare.sh -s scenarios/small.yml [--workers N] [--nak /path/to/nak]`
    - `--workers N` controls parallel generators (defaults to min(4, nproc) or env GEN_PAR)
  - Output DB at `bench/work/small/db/`
- Run benchmark(s):
  - `bench/scripts/run.sh -s scenarios/small.yml --out bench/results/raw/small-$(date +%s)`
  - Produces: client.json, server.log, sysinfo.json
- Report:
  - `bench/scripts/report.py bench/results/raw/* > bench/results/summary.md`

Output: Markdown Table (example)

| Scenario | DB events | NIP-50 | Conns | Subs/Conn | Writers/s | EOSE p50/p95/p99 (ms) | Search p50/p95/p99 (ms) | OK p50/p95/p99 (ms) | Delivered/s | RSS max (MB) | CPU avg (%) |
|---------:|----------:|:------:|------:|----------:|----------:|-----------------------:|-------------------------:|--------------------:|-----------:|-------------:|------------:|
| small    | 100k      | off    | 100   | 3         | 200       | 8 / 19 / 42            | —                       | 5 / 12 / 29         | 12,300     | 620          | 240         |
| small    | 100k      | on     | 100   | 3         | 200       | 9 / 21 / 47            | 14 / 31 / 66            | 5 / 12 / 30         | 12,100     | 640          | 252         |

Methodology Notes
- Warm-up period excluded from measurements
- Each scenario run twice and best-of-two reported (helps mitigate jitter)
- REQ scan latency measured from REQ send to EOSE receive per sub
- Search latency measured from REQ send to first EVENT, and to EOSE
- OK roundtrip measured per EVENT
- Logs parsed for dbScan perf lines if `relay__logging__dbScanPerf = true`

PII and Safety
- Scripts must not include hostname, users, IP/MAC addresses
- Sanitize all system data before writing to artifacts

Future Extensions
- k6-like scenario runner for WebSocket; distributed load generation
- Flame graphs and CPU profiling (perf/pprof) under opt-in
- Additional NIP scenarios (negentropy sync under load)
