#!/usr/bin/env bash
set -euo pipefail

# Prepare benchmark databases for a scenario
# - Generates events with nak
# - Ingests into a fresh strfry DB under bench/work/<name>/db

usage() {
  echo "Usage: $0 -s <scenario.yml> [--nak <path>] [--workers <n>]" >&2
  exit 1
}

SCENARIO=""
NAK_BIN="${NAK_BIN:-nak}"
CLI_WORKERS=""

while [[ $# -gt 0 ]]; do
  case "$1" in
    -s|--scenario) SCENARIO="$2"; shift 2;;
    --nak) NAK_BIN="$2"; shift 2;;
    -w|--workers) CLI_WORKERS="$2"; shift 2;;
    *) usage;;
  esac
done

[[ -z "$SCENARIO" ]] && usage

root_dir="$(cd "$(dirname "$0")/../.." && pwd)"
bench_dir="$root_dir/bench"
work_dir="$bench_dir/work"

command -v jq >/dev/null || { echo "jq required" >&2; exit 2; }
command -v "${NAK_BIN}" >/dev/null || { echo "nak required on PATH or set NAK_BIN" >&2; exit 2; }

# Render scenario JSON for easier parsing
y2j() { python3 - <<'PY'
import sys, yaml, json
print(json.dumps(yaml.safe_load(sys.stdin.read())))
PY
}

SC_FILE="$bench_dir/$SCENARIO"
SCJ=$(y2j <"$SC_FILE" 2>/dev/null || true)
NAME=$(echo "$SCJ" | jq -r '.name // empty')
EVENTS=$(echo "$SCJ" | jq -r '.db.events // empty')
KINDS=$(echo "$SCJ" | jq -r '.db.kinds // empty')
INJECT_RATE=$(echo "$SCJ" | jq -r '.db.keyword_inject_rate // empty')
KEYWORDS_JSON=$(echo "$SCJ" | jq -c '.db.keywords // empty')

# Fallback parsing if PyYAML isn't available
if [[ -z "$NAME" || -z "$EVENTS" || -z "$KINDS" ]]; then
  NAME=$(grep -E '^name:' "$SC_FILE" | head -1 | sed 's/^name:[[:space:]]*//')
  EVENTS=$(grep -E '^[[:space:]]*events:' "$SC_FILE" | head -1 | sed 's/.*events:[[:space:]]*//')
  KINDS=$(grep -E '^[[:space:]]*kinds:' "$SC_FILE" | head -1 | sed 's/.*kinds:[[:space:]]*//' | sed 's/^"//; s/"$//')
  INJECT_RATE=$(grep -E '^[[:space:]]*keyword_inject_rate:' "$SC_FILE" | head -1 | sed 's/.*keyword_inject_rate:[[:space:]]*//')
  KEYWORDS_JSON=""
fi

# Defaults if still unset
[[ -z "$INJECT_RATE" ]] && INJECT_RATE="0.6"
if [[ -z "$KEYWORDS_JSON" ]]; then KEYWORDS_JSON="[]"; fi

# Prompt for target DB installation directory
echo "[prepare] WARNING: This will populate a strfry LMDB database at the chosen path."
echo "[prepare] Existing contents may be overwritten."
read -r -p "[prepare] Enter database directory to install (blank = current dir: $(pwd)): " DB_PROMPT
if [[ -z "${DB_PROMPT}" ]]; then
  db_dir="$(pwd)"
  echo "[prepare] Using current directory: $db_dir"
else
  db_dir="${DB_PROMPT}"
  echo "[prepare] Using provided directory: $db_dir"
fi

# Create directory if needed and warn if non-empty
mkdir -p "$db_dir"
if [[ -n "$(ls -A "$db_dir" 2>/dev/null)" ]]; then
  read -r -p "[prepare] Directory '$db_dir' is not empty. Continue? [y/N]: " CONFIRM
  if [[ ! "$CONFIRM" =~ ^[Yy]$ ]]; then
    echo "[prepare] Aborting at user request." >&2
    exit 4
  fi
fi

mkdir -p "$work_dir/$NAME"
conf_tmp="$work_dir/$NAME/strfry-import.conf"

echo "[prepare] scenario=$NAME events=$EVENTS kinds=$KINDS"

# Prepare scenario-specific config: point DB to bench/work and disable search for faster import
cp "$root_dir/strfry.conf" "$conf_tmp"
sed -i "s#^db\s*=.*#db = $db_dir#" "$conf_tmp"
sed -i "s#^relay.search.enabled\s*=.*#relay.search.enabled = false#" "$conf_tmp" || true

# Simple parser for kinds pattern: supports comma-separated ints and A-B ranges; '*' => kind 1
parse_kinds() {
  local s="$1"; s="${s// /}"; IFS=',' read -r -a toks <<< "$s"
  local out=()
  for tok in "${toks[@]}"; do
    [[ -z "$tok" ]] && continue
    if [[ "$tok" == "*" ]]; then
      out+=("*")
    elif [[ "$tok" =~ ^[0-9]+-[0-9]+$ ]]; then
      out+=("$tok")
    elif [[ "$tok" =~ ^[0-9]+$ ]]; then
      out+=("$tok")
    fi
  done
  printf '%s\n' "${out[@]}"
}

pick_kind() {
  local -a arr=(); while IFS= read -r line; do arr+=("$line"); done < <(parse_kinds "$1")
  local tok="${arr[$RANDOM % ${#arr[@]}]}"
  if [[ "$tok" == "*" || -z "$tok" ]]; then echo 1; return; fi
  if [[ "$tok" =~ ^([0-9]+)-([0-9]+)$ ]]; then
    local a=${BASH_REMATCH[1]} b=${BASH_REMATCH[2]}
    echo $(( a + (RANDOM % (b - a + 1)) ))
  else
    echo "$tok"
  fi
}

# Generate events with nak into NDJSON parts in parallel, then import once
parts_dir="$work_dir/$NAME/gen"
rm -rf "$parts_dir" && mkdir -p "$parts_dir"

# Build weighted keyword pool for random sampling across workers
kw_pool_file="$work_dir/$NAME/kw_pool.txt"
rm -f "$kw_pool_file"
if [[ $(echo "$KEYWORDS_JSON" | jq 'length') -gt 0 ]]; then
  echo "$KEYWORDS_JSON" | jq -r '.[] | .term as $t | ((.weight // 1) | tonumber) as $w | [range($w)] | .[] | $t' > "$kw_pool_file"
else
  # Defaults if not provided by scenario
  printf '%s\n' nostr nostr nostr bitcoin bitcoin lightning federation 'best nostr apps' > "$kw_pool_file"
fi
KW_COUNT=$(wc -l < "$kw_pool_file" | tr -d ' ')
if [[ "$KW_COUNT" -le 0 ]]; then echo "[prepare] keyword pool is empty" >&2; exit 3; fi

CORES=$( (command -v nproc >/dev/null && nproc) || echo 2 )
# Choose workers from CLI if provided, else env GEN_PAR, else min(4, CORES)
if [[ -n "$CLI_WORKERS" ]]; then
  if ! [[ "$CLI_WORKERS" =~ ^[0-9]+$ ]] || [[ "$CLI_WORKERS" -le 0 ]]; then
    echo "[prepare] invalid --workers value: $CLI_WORKERS" >&2; exit 2
  fi
  GEN_PAR="$CLI_WORKERS"
else
  GEN_PAR=${GEN_PAR:-$(( CORES > 4 ? 4 : CORES ))}
fi
PER_PART=$(( (EVENTS + GEN_PAR - 1) / GEN_PAR ))

echo "[prepare] generating $EVENTS events using $GEN_PAR parallel workers"

gen_one_part() {
  local idx="$1"; local count="$2"; local outfile="$3"
  : > "$outfile"
  # Load keyword pool into an array for this worker
  mapfile -t KWPOOL < "$kw_pool_file"
  # Scale inject rate to 0..1000 integer space for $RANDOM comparisons
  local INJ_THOUS
  INJ_THOUS=$(awk -v r="$INJECT_RATE" 'BEGIN{printf("%d", (r<0?0:(r>1?1:r))*1000)}')
  for ((i=1;i<=count;i++)); do
    local k=$(pick_kind "$KINDS")
    local content="bench-$NAME-$idx-$i-$(date +%s%N)"
    # maybe inject one or two keywords into content
    if (( (RANDOM % 1000) < INJ_THOUS )); then
      local k1_index=$(( RANDOM % ${#KWPOOL[@]} ))
      local k1=${KWPOOL[$k1_index]}
      content+=" $k1"
      # 30% chance to add a second distinct keyword
      if (( (RANDOM % 100) < 30 )) && [[ ${#KWPOOL[@]} -gt 1 ]]; then
        local k2_index=$(( RANDOM % ${#KWPOOL[@]} ))
        if [[ $k2_index -eq $k1_index ]]; then k2_index=$(((k2_index+1)%${#KWPOOL[@]})); fi
        local k2=${KWPOOL[$k2_index]}
        content+=" $k2"
      fi
    fi
    if ! ${NAK_BIN} event -k "$k" -c "$content" >> "$outfile"; then
      echo "[prepare] nak failed on part $idx at i=$i" >&2; exit 3
    fi
  done
}

pids=()
for ((w=1; w<=GEN_PAR; w++)); do
  n=$PER_PART
  if [[ $w -eq $GEN_PAR ]]; then n=$(( EVENTS - (PER_PART * (GEN_PAR - 1)) )); fi
  [[ $n -le 0 ]] && break
  gen_one_part "$w" "$n" "$parts_dir/part_$w.ndjson" &
  pids+=("$!")
done

fail=0
for pid in "${pids[@]}"; do wait "$pid" || fail=1; done
[[ $fail -ne 0 ]] && { echo "[prepare] generation failed" >&2; exit 3; }

echo "[prepare] importing events into strfry DB: $db_dir"
cat "$parts_dir"/*.ndjson | "$root_dir/build/strfry" import --config "$conf_tmp" --no-verify

echo "[prepare] import complete. cleaning up generated parts"
rm -rf "$parts_dir"

echo "[prepare] Done."
