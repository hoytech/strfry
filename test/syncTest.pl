#!/usr/bin/env perl

## zstdcat ../nostr-dumps/nostr-wellorder-early-500k-v1.jsonl.zst | head -100000 | perl test/syncTest.pl 1 1 10000 '{}'

use strict;

my $prob1 = shift // 1;
my $prob2 = shift // 1;
my $prob3 = shift // 98;
my $filter = shift // '{}';

{
    my $total = $prob1 + $prob2 + $prob3;
    die "zero prob" if $total == 0;
    $prob1 = $prob1 / $total;
    $prob2 = $prob2 / $total;
    $prob3 = $prob3 / $total;
}

srand($ENV{SEED} || 0);
system("mkdir -p strfry-db-test-1 strfry-db-test-2");
system("rm -f strfry-db-test-1/data.mdb strfry-db-test-2/data.mdb");


my $ids1 = {};
my $ids2 = {};

{
    open(my $infile1, '|-', "./strfry --config test/cfgs/syncTest1.conf import");
    open(my $infile2, '|-', "./strfry --config test/cfgs/syncTest2.conf import");

    while (<STDIN>) {
        /"id":"(\w+)"/ || next;
        my $id = $1;

        my $modeRnd = rand();

        if ($modeRnd < $prob1) {
            print $infile1 $_;
            $ids1->{$id} = 1;
        } elsif ($modeRnd < $prob1 + $prob2) {
            print $infile2 $_;
            $ids2->{$id} = 1;
        } else {
            print $infile1 $_;
            print $infile2 $_;
            $ids1->{$id} = 1;
            $ids2->{$id} = 1;
        }
    }
}


withRelay(sub {
    system("./strfry --config test/cfgs/syncTest2.conf sync ws://127.0.0.1:40551 --dir both --filter '$filter'");
});

my $hash1 = `./strfry --config test/cfgs/syncTest1.conf export | perl test/dumbFilter.pl '$filter' | sort | sha256sum`;
my $hash2 = `./strfry --config test/cfgs/syncTest2.conf export | perl test/dumbFilter.pl '$filter' | sort | sha256sum`;

die "hashes differ" unless $hash1 eq $hash2;

print "OK.\n";


sub withRelay {
    my $cb = shift;

    my $relayPid = startRelay();

    $cb->();

    kill 'KILL', $relayPid;
    wait;
}

sub startRelay {
    my $pid = fork();

    if (!$pid) {
        exec("./strfry --config test/cfgs/syncTest1.conf relay") || die "couldn't exec strfry";
    }

    sleep 1; ## FIXME
    return $pid;
}
