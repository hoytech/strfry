Benchmark Scenarios

This doc describes the standard scenarios and how to create new ones.

Standard Scenarios
- small.yml
  - ~100k events
  - Mixed kinds: "1, 30023"
  - NIP-50 on and off runs
  - Connections: 100, subs/conn: 3, writers/s: 200
- medium.yml
  - ~1M events
  - Same workload profile as small
- large.yml (example)
  - ~10M events (requires ample disk/RAM)
  - Lower writers/s initially to avoid IO bottlenecks

Creating a Scenario
- Copy an existing YAML file under bench/scenarios/ and edit:
  - db.events, db.kinds, db.avg_event_size
  - server.search_enabled and server.search_backend
  - workload parameters (duration, warmup, connections, writers)

Tips
- For NIP-50 enabled runs, ensure catch-up indexer is caught up before measuring
- For very large DBs, consider increasing warmup and run duration
- Keep maxCandidateDocs and overfetchFactor balanced to avoid excessive scoring costs
- Choose search terms strategically:
  - Common terms (e.g., "nostr", "bitcoin") to stress high-DF scoring paths
  - Rare terms (e.g., project-specific tokens) to test low-DF paths and index lookups
  - Multi-term phrases (e.g., "best nostr apps") to test multi-token scoring
  - Inject keywords into generated content via db.keywords and db.keyword_inject_rate so searches return realistic results
