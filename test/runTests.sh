#!/usr/bin/env bash

set -euo pipefail

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; NC='\033[0m'

pass() { echo -e "${GREEN}[PASS]${NC} $*"; }
fail() { echo -e "${RED}[FAIL]${NC} $*"; exit 1; }
info() { echo -e "${YELLOW}[INFO]${NC} $*"; }


# write tests

info "running write tests..."

node "./test/tests/writeTest.js" \
  && pass "./test/tests/writeTest.js" \
  || fail "./test/tests/writeTest.js failed"

info "running restricted read tests..."

node "./test/tests/readRestrictTest.js" \
  && pass "./test/tests/readRestrictTest.js" \
  || fail "./test/tests/readRestrictTest.js failed"

info "Seeding events..."

perl "./test/utils/generate-seed-data.pl" -o - | ./strfry --config ./test/cfgs/test.conf import --no-verify

info "running filterFuzzTest..."

perl "./test/tests/filterFuzzTest.pl" scan \
&& pass "./test/tests/filterFuzzTest.pl scan" \
|| fail "./test/tests/filterFuzzTest.pl scan failed"

perl "./test/tests/filterFuzzTest.pl" scan-limit \
&& pass "./test/tests/filterFuzzTest.pl scan-limit" \
|| fail "./test/tests/filterFuzzTest.pl scan-limit failed"

perl "./test/tests/filterFuzzTest.pl" monitor \
&& pass "./test/tests/filterFuzzTest.pl monitor" \
|| fail "./test/tests/filterFuzzTest.pl monitor failed"


# sync tests

info "running sync tests..."

perl "./test/tests/runSyncTests.pl" \
  && pass "./test/tests/syncTests.pl" \
  || fail "./test/tests/runSyncTests.pl failed"

pass "All tests passed."
