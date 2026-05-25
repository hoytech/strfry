#!/usr/bin/env perl

# CLI-level tests for the search_reindex dbutils tool.
#
# Covers three regressions surfaced during PR #160 testing:
#   - stale-index detection refuses to operate on a v1 index without --restart
#   - --restart actually clears the index (latent dup-deletion bug)
#   - --from-levid does not update SearchState (partial-reindex semantics)
#
# Style mirrors test/writeTest.pl: nostril-generated signed events imported
# via `./strfry import`, all CLI-driven, no WebSocket dependency.

use strict;
use warnings;
use Carp;
$SIG{ __DIE__ } = \&Carp::confess;

use JSON::XS;

my $CFG = 'test/cfgs/searchTest.conf';
my $SEC = 'c1eee22f68dc218d98263cfecb350db6fc6b3e836b47423b66c62af7ae3e32bb';


sub run_test {
    my ($desc, $sub) = @_;
    print "* $desc\n";
    cleanDb();
    importEvents();
    $sub->();
}


# --- Test 1: stale-index error path -----------------------------------------

run_test("search_reindex without --restart errors on a stale (v1) index" => sub {
    # Build a clean v2 index first so SearchState exists.
    runOK("./strfry --config $CFG search_reindex --restart");

    # Fake a v1 stored index by force-setting indexVersion=1.
    runOK("./strfry --config $CFG search_set_state --lev-id=1 --index-version=1 --allow-lower");

    # Plain search_reindex must refuse with a clear message.
    my ($out, $rc) = capture("./strfry --config $CFG search_reindex 2>&1");
    die "expected non-zero exit, got rc=$rc\nout=$out" if $rc == 0;
    die "expected 'stale' message, got:\n$out" unless $out =~ /stale\s*\(stored version 1, expected 2\)/;
    die "expected --restart hint, got:\n$out" unless $out =~ /--restart/;

    # --restart must succeed and bring us back to v2.
    runOK("./strfry --config $CFG search_reindex --restart");
});


# --- Test 2: --restart actually clears the index ----------------------------

run_test("search_reindex --restart is idempotent (no leftover postings)" => sub {
    my $first  = parseIndexedCount(capture("./strfry --config $CFG search_reindex --restart 2>&1"));
    my $second = parseIndexedCount(capture("./strfry --config $CFG search_reindex --restart 2>&1"));

    die "first --restart did not index anything (output parse failure?)" if !defined $first || $first == 0;
    die "second --restart indexed differently ($first vs $second). Suggests the clear loop left leftover dup data."
        if $first != $second;
});


# --- Test 3: --from-levid does not update SearchState -----------------------

run_test("search_reindex --from-levid leaves SearchState untouched" => sub {
    # Build a fresh v2 index.
    runOK("./strfry --config $CFG search_reindex --restart");

    # Plant a sentinel value into SearchState. search_set_state echoes the
    # previous values to stdout, which we use to assert in the next step.
    runOK("./strfry --config $CFG search_set_state --lev-id=987654321 --allow-lower");

    # Run a partial reindex. This is the operation that MUST NOT update state.
    runOK("./strfry --config $CFG search_reindex --from-levid=1");

    # Plant a different sentinel; the printed "previous" must still be the
    # 987654321 we set above, proving the --from-levid run did not write
    # to SearchState.
    my ($out, $rc) = capture("./strfry --config $CFG search_set_state --lev-id=987654320 --allow-lower 2>&1");
    die "search_set_state failed: rc=$rc\n$out" if $rc != 0;
    die "expected 'previous: levId=987654321' (SearchState updated unexpectedly), got:\n$out"
        unless $out =~ /previous:\s*levId=987654321\b/;
});


print "\nOK\n";


# --- helpers ---------------------------------------------------------------

sub cleanDb {
    system("mkdir -p strfry-db-test");
    system("rm -f strfry-db-test/data.mdb strfry-db-test/lock.mdb");
}

sub importEvents {
    # A handful of kind-1 events with varied tokens. Enough to exercise
    # multiple postings without being slow.
    my @contents = (
        "the quick brown fox jumps over the lazy dog",
        "hello world from nostr relay tests",
        "lightning bolts and bitcoin transactions",
        "search index regression coverage matters",
        "perl tests are still tests",
    );

    for my $i (0..$#contents) {
        my $content = $contents[$i];
        my $created = 5000 + $i;
        my $cmd = qq{nostril --sec $SEC --content "$content" --kind 1 --created-at $created > test-eventXYZ.json};
        system($cmd) == 0 or die "nostril failed";
        system(qq{ <test-eventXYZ.json ./strfry --config $CFG import 2>/dev/null }) == 0
            or die "strfry import failed";
    }
    unlink 'test-eventXYZ.json';
}

sub capture {
    my $cmd = shift;
    my $out = `$cmd`;
    my $rc  = $? >> 8;
    return ($out, $rc) if wantarray;
    return $out;
}

sub runOK {
    my $cmd = shift;
    my ($out, $rc) = capture("$cmd 2>&1");
    die "expected success, got rc=$rc for:\n  $cmd\n--- output ---\n$out\n--- end ---\n" if $rc != 0;
    return $out;
}

sub parseIndexedCount {
    my $out = shift;
    if (ref $out eq 'ARRAY') { $out = $out->[0]; }
    return undef unless defined $out;
    return $1 if $out =~ /Events indexed:\s*(\d+)/m;
    return undef;
}
