#!/bin/sh
set -e

echo "=== Generating test data ==="
perl test/genTestData.pl 5000 > /tmp/testdata.jsonl
echo "Generated $(wc -l < /tmp/testdata.jsonl) events"

echo ""
echo "=== Creating strfry config ==="
cat > /tmp/strfry.conf << 'CONF'
relay {
    bind = "127.0.0.1"
    port = 7777
    info {
        name = "test"
    }
    maxFilterLimit = 500
}
db {
    path = "/tmp/strfry-db/"
}
CONF

mkdir -p /tmp/strfry-db

echo ""
echo "=== Importing test data ==="
./strfry --config /tmp/strfry.conf import < /tmp/testdata.jsonl 2>&1 | tail -3

echo ""
echo "=== Running scan fuzz test (50 iterations) ==="
# We modify the test to run a fixed number of iterations instead of forever
SEED=42 timeout 120 perl -e '
use strict;
use JSON::XS;

# Source the test
do "test/filterFuzzTest.pl" if 0;

# Inline the needed parts
my $kinds = [qw/1 7 4 42 0 30 3 6/];
my $pubkeys = [];
my $ids = [];
my $topics = [qw/bitcoin nos nostr nostrnovember gitlog introductions jb55 damus chat meme cat dog art music/];

# Get actual pubkeys and ids from our test data
open my $fh, "<", "/tmp/testdata.jsonl" or die;
my %seen_pk;
my %seen_id;
while (<$fh>) {
    my $ev = decode_json($_);
    push @$pubkeys, $ev->{pubkey} unless $seen_pk{$ev->{pubkey}}++;
    push @$ids, $ev->{id} unless $seen_id{$ev->{id}}++;
    last if @$pubkeys >= 20 && @$ids >= 50;
}
close $fh;

$ENV{STRFRY_CONFIG} = "/tmp/strfry.conf";

srand(42);
my $pass = 0;
my $fail = 0;

for my $iter (1..50) {
    my $f = genRandomFilterGroup(0);
    my $fge = encode_json($f);

    my $resA = `./strfry --config /tmp/strfry.conf export --reverse 2>/dev/null | perl test/dumbFilter.pl \x27$fge\x27 | jq -r .id | sort | sha256sum`;
    my $resB = `./strfry --config /tmp/strfry.conf scan --pause 1 --metrics \x27$fge\x27 2>/dev/null | jq -r .id | sort | sha256sum`;

    if ($resA eq $resB) {
        $pass++;
        print "  scan iter $iter: PASS\n";
    } else {
        $fail++;
        print "  scan iter $iter: FAIL\n";
        print "    filter: $fge\n";
        print "    export|dumbFilter: $resA";
        print "    scan:              $resB";
    }
}

print "\n=== Scan results: $pass passed, $fail failed ===\n";
exit($fail > 0 ? 1 : 0);

sub genRandomFilterGroup {
    my $useLimit = shift;
    my $numFilters = $useLimit ? 1 : int(rand()*10)+1;
    my @filters;
    for (1..$numFilters) {
        my $f = {};
        while (!keys %$f) {
            if (rand() < .15) {
                $f->{ids} = [];
                for (1..(rand()*10)) {
                    push @{$f->{ids}}, $ids->[int(rand() * @$ids)];
                }
            }
            if (rand() < .3) {
                $f->{authors} = [];
                for (1..(rand()*5)) {
                    push @{$f->{authors}}, $pubkeys->[int(rand() * @$pubkeys)];
                }
            }
            if (rand() < .2) {
                $f->{kinds} = [];
                for (1..(rand()*5)) {
                    push @{$f->{kinds}}, 0+$kinds->[int(rand() * @$kinds)];
                }
            }
            if (rand() < .2) {
                $f->{"#e"} = [];
                for (1..(rand()*10)) {
                    push @{$f->{"#e"}}, $ids->[int(rand() * @$ids)];
                }
            }
            if (rand() < .2) {
                $f->{"#p"} = [];
                for (1..(rand()*5)) {
                    push @{$f->{"#p"}}, $pubkeys->[int(rand() * @$pubkeys)];
                }
            }
            if (rand() < .2) {
                $f->{"#t"} = [];
                for (1..(rand()*5)) {
                    push @{$f->{"#t"}}, $topics->[int(rand() * @$topics)];
                }
            }
            # NIP-91: AND tag filter
            if (rand() < .15) {
                $f->{"&t"} = [];
                for (1..(rand()*3)+1) {
                    push @{$f->{"&t"}}, $topics->[int(rand() * @$topics)];
                }
            }
        }
        if (rand() < .2) {
            $f->{since} = 1640300802 + int(rand() * 86400*365);
        }
        if (rand() < .2) {
            $f->{until} = 1640300802 + int(rand() * 86400*365);
        }
        if ($useLimit) {
            $f->{limit} = 1 + int(rand() * 1000);
        }
        if ($f->{since} && $f->{until} && $f->{since} > $f->{until}) {
            delete $f->{since};
            delete $f->{until};
        }
        push @filters, $f;
    }
    return \@filters;
}
' 2>&1

echo ""
echo "=== Running scan-limit fuzz test (50 iterations) ==="
SEED=42 timeout 120 perl -e '
use strict;
use JSON::XS;

my $kinds = [qw/1 7 4 42 0 30 3 6/];
my $pubkeys = [];
my $ids = [];
my $topics = [qw/bitcoin nos nostr nostrnovember gitlog introductions jb55 damus chat meme cat dog art music/];

open my $fh, "<", "/tmp/testdata.jsonl" or die;
my %seen_pk;
my %seen_id;
while (<$fh>) {
    my $ev = decode_json($_);
    push @$pubkeys, $ev->{pubkey} unless $seen_pk{$ev->{pubkey}}++;
    push @$ids, $ev->{id} unless $seen_id{$ev->{id}}++;
    last if @$pubkeys >= 20 && @$ids >= 50;
}
close $fh;

srand(42);
my $pass = 0;
my $fail = 0;

for my $iter (1..50) {
    my $f = genRandomFilterGroup(1);
    my $fge = encode_json($f);

    my $headCmd = "| head -n $f->[0]->{limit}";
    my $resA = `./strfry --config /tmp/strfry.conf export --reverse 2>/dev/null | perl test/dumbFilter.pl \x27$fge\x27 $headCmd | jq -r .id | sort | sha256sum`;
    my $resB = `./strfry --config /tmp/strfry.conf scan --pause 1 --metrics \x27$fge\x27 2>/dev/null | jq -r .id | sort | sha256sum`;

    if ($resA eq $resB) {
        $pass++;
        print "  scan-limit iter $iter: PASS\n";
    } else {
        $fail++;
        print "  scan-limit iter $iter: FAIL\n";
        print "    filter: $fge\n";
        print "    export|dumbFilter: $resA";
        print "    scan:              $resB";
    }
}

print "\n=== Scan-limit results: $pass passed, $fail failed ===\n";
exit($fail > 0 ? 1 : 0);

sub genRandomFilterGroup {
    my $useLimit = shift;
    my $numFilters = $useLimit ? 1 : int(rand()*10)+1;
    my @filters;
    for (1..$numFilters) {
        my $f = {};
        while (!keys %$f) {
            if (rand() < .15) {
                $f->{ids} = [];
                for (1..(rand()*10)) {
                    push @{$f->{ids}}, $ids->[int(rand() * @$ids)];
                }
            }
            if (rand() < .3) {
                $f->{authors} = [];
                for (1..(rand()*5)) {
                    push @{$f->{authors}}, $pubkeys->[int(rand() * @$pubkeys)];
                }
            }
            if (rand() < .2) {
                $f->{kinds} = [];
                for (1..(rand()*5)) {
                    push @{$f->{kinds}}, 0+$kinds->[int(rand() * @$kinds)];
                }
            }
            if (rand() < .2) {
                $f->{"#e"} = [];
                for (1..(rand()*10)) {
                    push @{$f->{"#e"}}, $ids->[int(rand() * @$ids)];
                }
            }
            if (rand() < .2) {
                $f->{"#p"} = [];
                for (1..(rand()*5)) {
                    push @{$f->{"#p"}}, $pubkeys->[int(rand() * @$pubkeys)];
                }
            }
            if (rand() < .2) {
                $f->{"#t"} = [];
                for (1..(rand()*5)) {
                    push @{$f->{"#t"}}, $topics->[int(rand() * @$topics)];
                }
            }
            # NIP-91: AND tag filter
            if (rand() < .15) {
                $f->{"&t"} = [];
                for (1..(rand()*3)+1) {
                    push @{$f->{"&t"}}, $topics->[int(rand() * @$topics)];
                }
            }
        }
        if (rand() < .2) {
            $f->{since} = 1640300802 + int(rand() * 86400*365);
        }
        if (rand() < .2) {
            $f->{until} = 1640300802 + int(rand() * 86400*365);
        }
        if ($useLimit) {
            $f->{limit} = 1 + int(rand() * 1000);
        }
        if ($f->{since} && $f->{until} && $f->{since} > $f->{until}) {
            delete $f->{since};
            delete $f->{until};
        }
        push @filters, $f;
    }
    return \@filters;
}
' 2>&1

echo ""
echo "=== All tests complete ==="
