#!/usr/bin/env perl

use strict;

use Carp;
$SIG{ __DIE__ } = \&Carp::confess;

use Data::Dumper;
use JSON::XS;

$Data::Dumper::Sortkeys = 1;


my $ids = [
    {
        sec => 'c1eee22f68dc218d98263cfecb350db6fc6b3e836b47423b66c62af7ae3e32bb',
        pub => '003ba9b2c5bd8afeed41a4ce362a8b7fc3ab59c25b6a1359cae9093f296dac01',
    },
    {
        sec => 'a0b459d9ff90e30dc9d1749b34c4401dfe80ac2617c7732925ff994e8d5203ff',
        pub => 'cc49e2a58373abc226eee84bee9ba954615aa2ef1563c4f955a74c4606a3b1fa',
    },
];




doTest({
    desc => "Basic insert",
    events => [
        qq{--sec $ids->[0]->{sec} --content "hi" --kind 1 },
        qq{--sec $ids->[0]->{sec} --content "hi 2" --kind 1 },
    ],
    verify => [ 0, 1, ],
});


doTest({
    desc => "Replacement, newer timestamp",
    events => [
        qq{--sec $ids->[0]->{sec} --content "hi" --kind 10000 --created-at 5000 },
        qq{--sec $ids->[0]->{sec} --content "hi 2" --kind 10000 --created-at 5001 },
        qq{--sec $ids->[0]->{sec} --content "hi" --kind 10000 --created-at 5000 },
    ],
    verify => [ 1, ],
});



doTest({
    desc => "Replacement is dropped",
    events => [
        qq{--sec $ids->[0]->{sec} --content "hi" --kind 10000 --created-at 5001 },
        qq{--sec $ids->[0]->{sec} --content "hi 2" --kind 10000 --created-at 5000 },
    ],
    verify => [ 0, ],
});


doTest({
    desc => "Doesn't replace some else's event",
    events => [
        qq{--sec $ids->[0]->{sec} --content "hi" --kind 10000 --created-at 5000 },
        qq{--sec $ids->[1]->{sec} --content "hi 2" --kind 10000 --created-at 5001 },
    ],
    verify => [ 0, 1, ],
});


doTest({
    desc => "Doesn't replace different kind",
    events => [
        qq{--sec $ids->[0]->{sec} --content "hi" --kind 10001 --created-at 5000 },
        qq{--sec $ids->[1]->{sec} --content "hi 2" --kind 10000 --created-at 5001 },
    ],
    verify => [ 0, 1, ],
});


doTest({
    desc => "d tags are ignored in 10k-20k range",
    events => [
        qq{--sec $ids->[0]->{sec} --content "hi" --kind 10003 --created-at 5000 },
        qq{--sec $ids->[0]->{sec} --content "hi 2" --kind 10003 --created-at 5001 --tag d 'myrepl' },
    ],
    verify => [ 1, ],
});

doTest({
    desc => "Equal timestamps: replacement does not happen because new id > old id",
    events => [
        qq{--sec $ids->[0]->{sec} --content "c1" --kind 10000 --created-at 5000 },
        qq{--sec $ids->[0]->{sec} --content "c2" --kind 10000 --created-at 5000 },
    ],
    assertIds => [qw/ 7c ae /],
    verify => [ 0, ],
});

doTest({
    desc => "Equal timestamps: replacement does happen because new id < old id",
    events => [
        qq{--sec $ids->[0]->{sec} --content "c1" --kind 10000 --created-at 5000 },
        qq{--sec $ids->[0]->{sec} --content "c4" --kind 10000 --created-at 5000 },
    ],
    assertIds => [qw/ 7c 63 /],
    verify => [ 1, ],
});



doTest({
    desc => "Deletion",
    events => [
        qq{--sec $ids->[0]->{sec} --content "hi" --kind 1 --created-at 5000 },
        qq{--sec $ids->[0]->{sec} --content "hi" --kind 1 --created-at 5001 },
        qq{--sec $ids->[0]->{sec} --content "hi" --kind 1 --created-at 5002 },
        qq{--sec $ids->[0]->{sec} --content "blah" --kind 5 --created-at 6000 -e EV_2 -e EV_0 },
    ],
    verify => [ 1, 3, ],
});


doTest({
    desc => "Deletion, duplicate",
    events => [
        qq{--sec $ids->[0]->{sec} --content "hi" --kind 1 --created-at 5000 },
        qq{--sec $ids->[0]->{sec} --content "hi" --kind 1 --created-at 5001 },
        qq{--sec $ids->[0]->{sec} --content "hi" --kind 1 --created-at 5002 },
        qq{--sec $ids->[0]->{sec} --content "blah" --kind 5 --created-at 6000 -e EV_2 -e EV_2 },
    ],
    verify => [ 0, 1, 3, ],
});


doTest({
    desc => "Can't delete someone else's event",
    events => [
        qq{--sec $ids->[0]->{sec} --content "hi" --kind 1 --created-at 5000 },
        qq{--sec $ids->[1]->{sec} --content "blah" --kind 5 --created-at 6000 -e EV_0 },
    ],
    verify => [ 0, 1, ],
});


doTest({
    desc => "Deletion prevents re-adding same event",
    events => [
        qq{--sec $ids->[0]->{sec} --content "hi" --kind 1 --created-at 5000 },
        qq{--sec $ids->[0]->{sec} --content "blah" --kind 5 --created-at 6000 -e EV_0 },
        qq{--sec $ids->[0]->{sec} --content "hi" --kind 1 --created-at 5000 },
    ],
    verify => [ 1, ],
});




doTest({
    desc => "Parameterized Replaceable Events",
    events => [
        qq{--sec $ids->[0]->{sec} --content "hi1" --kind 30001 --created-at 5000 --tag d myrepl },
        qq{--sec $ids->[0]->{sec} --content "hi2" --kind 30001 --created-at 5001 --tag d myrepl },
    ],
    verify => [ 1, ],
});

doTest({
    desc => "d tag only works in range 30k-40k",
    events => [
        qq{--sec $ids->[0]->{sec} --content "hi1" --kind 1 --created-at 5000 --tag d myrepl },
        qq{--sec $ids->[0]->{sec} --content "hi2" --kind 1 --created-at 5001 --tag d myrepl },
    ],
    verify => [ 0, 1, ],
});

doTest({
    desc => "d tags have to match",
    events => [
        qq{--sec $ids->[0]->{sec} --content "hi1" --kind 30001 --created-at 5000 --tag d myrepl },
        qq{--sec $ids->[0]->{sec} --content "hi2" --kind 30001 --created-at 5001 --tag d myrepl2 },
        qq{--sec $ids->[0]->{sec} --content "hi3" --kind 30001 --created-at 5002 --tag d myrepl },
    ],
    verify => [ 1, 2, ],
});


doTest({
    desc => "Kinds have to match",
    events => [
        qq{--sec $ids->[0]->{sec} --content "hi1" --kind 30001 --created-at 5000 --tag d myrepl },
        qq{--sec $ids->[0]->{sec} --content "hi2" --kind 30002 --created-at 5001 --tag d myrepl },
    ],
    verify => [ 0, 1, ],
});


doTest({
    desc => "Pubkeys have to match",
    events => [
        qq{--sec $ids->[0]->{sec} --content "hi1" --kind 30001 --created-at 5000 --tag d myrepl },
        qq{--sec $ids->[1]->{sec} --content "hi2" --kind 30001 --created-at 5001 --tag d myrepl },
    ],
    verify => [ 0, 1, ],
});


doTest({
    desc => "Newer param replaceable event isn't replaced",
    events => [
        qq{--sec $ids->[0]->{sec} --content "hi1" --kind 30001 --created-at 5001 --tag d myrepl },
        qq{--sec $ids->[0]->{sec} --content "hi2" --kind 30001 --created-at 5000 --tag d myrepl },
    ],
    verify => [ 0, ],
});


doTest({
    desc => "Explicit empty d tag",
    events => [
        qq{--sec $ids->[0]->{sec} --content "hi" --kind 30003 --created-at 5000 },
        qq{--sec $ids->[0]->{sec} --content "hi 2" --kind 30003 --created-at 5001 --tag d '' },
        qq{--sec $ids->[0]->{sec} --content "hi" --kind 30003 --created-at 5000 },
    ],
    verify => [ 1, ],
});



print "\nOK\n";


sub doTest {
    my $spec = shift;

    print "* ", ($spec->{desc} || 'unnamed'), "\n";

    cleanDb();

    my $eventIds = [];

    for my $ev (@{ $spec->{events} }) {
        $ev =~ s{EV_(\d+)}{$eventIds->[$1]}eg;
        push @$eventIds, addEvent($ev);
    }

    for (my $i = 0; $i < @{ $spec->{assertIds} || [] }; $i++) {
        die "assertId incorrect" unless rindex($eventIds->[$i], $spec->{assertIds}->[$i], 0) == 0;
    }

    my $finalEventIds = [];

    {
        open(my $fh, '-|', './strfry --config test/cfgs/writeTest.conf export 2>/dev/null') || die "$!";
        while(<$fh>) {
            push @$finalEventIds, decode_json($_)->{id};
        }
    }

    die "incorrect eventIds lengths" if @{$spec->{verify}} != @$finalEventIds;

    for (my $i = 0; $i < @$finalEventIds; $i++) {
        die "id mismatch" if $eventIds->[$spec->{verify}->[$i]] ne $finalEventIds->[$i];
    }
}


sub cleanDb {
    system("mkdir -p strfry-db-test");
    system("rm -f strfry-db-test/data.mdb");
}

sub addEvent {
    my $ev = shift;

    system(qq{ nostril $ev >test-eventXYZ.json });

    my $eventJson = `cat test-eventXYZ.json`;

    system(qq{ <test-eventXYZ.json ./strfry --config test/cfgs/writeTest.conf import 2>/dev/null });

    system(qq{ rm test-eventXYZ.json });

    my $event = decode_json($eventJson);
    print Dumper($event) if $ENV{DUMP_EVENTS};

    return $event->{id};
}
