#!/usr/bin/env bash
set -euo pipefail

# Run a benchmark for a scenario
# - Starts strfry with scenario config
# - Collects sysinfo and server logs
# - Invokes client load generator (placeholder)

usage() {
  echo "Usage: $0 -s <scenario.yml> --out <outdir> [--port <7777>]" >&2
  exit 1
}

SCENARIO=""
OUTDIR=""
PORT=7777

while [[ $# -gt 0 ]]; do
  case "$1" in
    -s|--scenario) SCENARIO="$2"; shift 2;;
    --out) OUTDIR="$2"; shift 2;;
    --port) PORT="$2"; shift 2;;
    *) usage;;
  esac
done

[[ -z "$SCENARIO" || -z "$OUTDIR" ]] && usage

root_dir="$(cd "$(dirname "$0")/../.." && pwd)"
bench_dir="$root_dir/bench"
work_dir="$bench_dir/work"
out_dir="$OUTDIR"
mkdir -p "$out_dir"

command -v jq >/dev/null || { echo "jq required" >&2; exit 2; }

y2j() { python3 - <<'PY'
import sys, yaml, json
print(json.dumps(yaml.safe_load(sys.stdin.read())))
PY
}

SCJ=$(y2j <"$bench_dir/$SCENARIO")
NAME=$(echo "$SCJ" | jq -r .name)
SEARCH_ENABLED=$(echo "$SCJ" | jq -r .server.search_enabled)
SEARCH_BACKEND=$(echo "$SCJ" | jq -r .server.search_backend)
DURATION=$(echo "$SCJ" | jq -r .workload.duration_s)
WARMUP=$(echo "$SCJ" | jq -r .workload.warmup_s)

db_dir="$work_dir/$NAME/db"
log_file="$out_dir/server.log"
sysinfo_json="$out_dir/sysinfo.json"
client_json="$out_dir/client.json"

echo "[run] scenario=$NAME search=$SEARCH_ENABLED backend=$SEARCH_BACKEND duration=${DURATION}s warmup=${WARMUP}s"

# Collect sanitized system info
"$bench_dir/scripts/sysinfo.sh" > "$sysinfo_json"

# Render scenario-specific strfry.conf (override search settings and db path)
conf_tmp="$out_dir/strfry-bench.conf"
cp "$root_dir/strfry.conf" "$conf_tmp"
sed -i "s#^db\s*=.*#db = $db_dir#" "$conf_tmp"
sed -i "s#^relay.search.enabled\s*=.*#relay.search.enabled = $SEARCH_ENABLED#" "$conf_tmp" || true
sed -i "s#^relay.search.backend\s*=.*#relay.search.backend = $SEARCH_BACKEND#" "$conf_tmp" || true
sed -i "s#^relay.port\s*=.*#relay.port = $PORT#" "$conf_tmp" || true

# Start strfry
echo "[run] starting strfry on port $PORT" | tee -a "$log_file"
"$root_dir/build/strfry" relay --config "$conf_tmp" >> "$log_file" 2>&1 &
SRV_PID=$!
sleep 2

cleanup() {
  if kill -0 "$SRV_PID" 2>/dev/null; then
    kill "$SRV_PID" || true
    wait "$SRV_PID" || true
  fi
}
trap cleanup EXIT

# Placeholder: invoke load generator to populate $client_json
echo '{"note":"client metrics placeholder"}' > "$client_json"

echo "[run] sleeping for duration=${DURATION}s (including warmup=${WARMUP}s)"
sleep "$DURATION"

echo "[run] complete; shutting down server" | tee -a "$log_file"

