#!/usr/bin/env bash
set -euo pipefail

# Print sanitized system info as JSON to stdout

to_json_kv() {
  local k="$1"; shift
  local v="$*"
  printf '  "%s": %s' "$k" "$v"
}

json_escape() {
  python3 -c 'import json,sys; print(json.dumps(sys.stdin.read().strip()))'
}

cpu_json=$(lscpu | awk -F: '{gsub(/^ +| +$/,"",$2); gsub(/^ +| +$/,"",$1); if($1!~/^Model name|^CPU\(s\)|^Thread|^Core|^Socket|^CPU MHz|^Architecture|^Vendor ID/){next}; printf("\"%s\": \"%s\",\n", $1, $2)}' | sed 's/,$//') || cpu_json=''
mem_total_kb=$(awk '/MemTotal:/ {print $2}' /proc/meminfo 2>/dev/null || echo 0)
kernel=$(uname -r | sed 's/"/\"/g')
os_pretty=$( (grep PRETTY_NAME= /etc/os-release || echo PRETTY_NAME=unknown) | cut -d= -f2- | tr -d '"')
fs=$(df -h . | awk 'NR==2{print $1" "$2" "$6}')
storage=$(lsblk -o NAME,ROTA,TYPE,MOUNTPOINT | awk 'NR==1 || /\/$/ {print}' | sed '1d')

cat <<JSON
{
  "kernel": $(printf '%s' "$kernel" | json_escape),
  "os": $(printf '%s' "$os_pretty" | json_escape),
  "cpu": {
$(printf '%s' "$cpu_json")
  },
  "memory_kb": $mem_total_kb,
  "filesystem": $(printf '%s' "$fs" | json_escape),
  "storage": $(printf '%s' "$storage" | json_escape)
}
JSON

