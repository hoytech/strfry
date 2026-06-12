#!/usr/bin/env bash

set -euo pipefail

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; NC='\033[0m'

pass() { echo -e "${GREEN}[PASS]${NC} $*"; }
fail() { echo -e "${RED}[FAIL]${NC} $*"; exit 1; }
info() { echo -e "${YELLOW}[INFO]${NC} $*"; }


# write tests

info "running write tests..."

node "./test/writeTest.js" \
  && pass "./test/writeTest.js" \
  || fail "./test/writeTest.js failed"


info "Seeding events..."

perl "./test/generate-seed-data.pl" -o - | ./strfry --config ./test/cfgs/test.conf import --no-verify

info "running filterFuzzTest..."

perl "./test/filterFuzzTest.pl" scan \
&& pass "./test/filterFuzzTest.pl scan" \
|| fail "./test/filterFuzzTest.pl scan failed"

perl "./test/filterFuzzTest.pl" scan-limit \
&& pass "./test/filterFuzzTest.pl scan-limit" \
|| fail "./test/filterFuzzTest.pl scan-limit failed"

perl "./test/filterFuzzTest.pl" monitor \
&& pass "./test/filterFuzzTest.pl monitor" \
|| fail "./test/filterFuzzTest.pl monitor failed"


# sync tests

info "running sync tests..."

perl "./test/runSyncTests.pl" \
  && pass "./test/syncTests.pl" \
  || fail "./test/runSyncTests.pl failed"

pass "All tests passed."
