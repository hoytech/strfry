#!/usr/bin/env perl

use strict;

use Data::Dumper;
use JSON::XS;


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



## Basic insert

doTest({
    events => [
        qq{--sec $ids->[0]->{sec} --content "hi" --kind 1 },
        qq{--sec $ids->[0]->{sec} --content "hi 2" --kind 1 },
    ],
    verify => [ 0, 1, ],
});

## Replacement, newer timestamp

doTest({
    events => [
        qq{--sec $ids->[0]->{sec} --content "hi" --kind 10000 --created-at 5000 },
        qq{--sec $ids->[0]->{sec} --content "hi 2" --kind 10000 --created-at 5001 },
        qq{--sec $ids->[0]->{sec} --content "hi" --kind 10000 --created-at 5000 },
    ],
    verify => [ 1, ],
});

## Replacement is dropped

doTest({
    events => [
        qq{--sec $ids->[0]->{sec} --content "hi" --kind 10000 --created-at 5001 },
        qq{--sec $ids->[0]->{sec} --content "hi 2" --kind 10000 --created-at 5000 },
    ],
    verify => [ 0, ],
});

## Doesn't replace some else's event

doTest({
    events => [
        qq{--sec $ids->[0]->{sec} --content "hi" --kind 10000 --created-at 5000 },
        qq{--sec $ids->[1]->{sec} --content "hi 2" --kind 10000 --created-at 5001 },
    ],
    verify => [ 0, 1, ],
});

## Doesn't replace different kind

doTest({
    events => [
        qq{--sec $ids->[0]->{sec} --content "hi" --kind 10001 --created-at 5000 },
        qq{--sec $ids->[1]->{sec} --content "hi 2" --kind 10000 --created-at 5001 },
    ],
    verify => [ 0, 1, ],
});


## Deletion

doTest({
    events => [
        qq{--sec $ids->[0]->{sec} --content "hi" --kind 1 --created-at 5000 },
        qq{--sec $ids->[0]->{sec} --content "hi" --kind 1 --created-at 5001 },
        qq{--sec $ids->[0]->{sec} --content "hi" --kind 1 --created-at 5002 },
        qq{--sec $ids->[0]->{sec} --content "blah" --kind 5 --created-at 6000 -e EV_2 -e EV_0 },
    ],
    verify => [ 1, 3, ],
});

## Can't delete someone else's event

doTest({
    events => [
        qq{--sec $ids->[0]->{sec} --content "hi" --kind 1 --created-at 5000 },
        qq{--sec $ids->[1]->{sec} --content "blah" --kind 5 --created-at 6000 -e EV_0 },
    ],
    verify => [ 0, 1, ],
});

## Deletion prevents re-adding same event

doTest({
    events => [
        qq{--sec $ids->[0]->{sec} --content "hi" --kind 1 --created-at 5000 },
        qq{--sec $ids->[0]->{sec} --content "blah" --kind 5 --created-at 6000 -e EV_0 },
        qq{--sec $ids->[0]->{sec} --content "hi" --kind 1 --created-at 5000 },
    ],
    verify => [ 1, ],
});



## Parameterized Replaceable Events

doTest({
    events => [
        qq{--sec $ids->[0]->{sec} --content "hi1" --kind 1 --created-at 5000 --tag d myrepl },
        qq{--sec $ids->[0]->{sec} --content "hi2" --kind 1 --created-at 5001 --tag d myrepl },
    ],
    verify => [ 1, ],
});

## d tags have to match

doTest({
    events => [
        qq{--sec $ids->[0]->{sec} --content "hi1" --kind 1 --created-at 5000 --tag d myrepl },
        qq{--sec $ids->[0]->{sec} --content "hi2" --kind 1 --created-at 5001 --tag d myrepl2 },
        qq{--sec $ids->[0]->{sec} --content "hi3" --kind 1 --created-at 5002 --tag d myrepl },
    ],
    verify => [ 1, 2, ],
});

## Kinds have to match

doTest({
    events => [
        qq{--sec $ids->[0]->{sec} --content "hi1" --kind 1 --created-at 5000 --tag d myrepl },
        qq{--sec $ids->[0]->{sec} --content "hi2" --kind 2 --created-at 5001 --tag d myrepl },
    ],
    verify => [ 0, 1, ],
});

## Pubkeys have to match

doTest({
    events => [
        qq{--sec $ids->[0]->{sec} --content "hi1" --kind 1 --created-at 5000 --tag d myrepl },
        qq{--sec $ids->[1]->{sec} --content "hi2" --kind 1 --created-at 5001 --tag d myrepl },
    ],
    verify => [ 0, 1, ],
});

## Timestamp

doTest({
    events => [
        qq{--sec $ids->[0]->{sec} --content "hi1" --kind 1 --created-at 5001 --tag d myrepl },
        qq{--sec $ids->[0]->{sec} --content "hi2" --kind 1 --created-at 5000 --tag d myrepl },
    ],
    verify => [ 0, ],
});



sub doTest {
    my $spec = shift;

    cleanDb();

    my $eventIds = [];

    for my $ev (@{ $spec->{events} }) {
        $ev =~ s{EV_(\d+)}{$eventIds->[$1]}eg;
        push @$eventIds, addEvent($ev);
    }

    my $finalEventIds = [];

    {
        open(my $fh, '-|', './strfry --config test/strfry.conf export 2>/dev/null') || die "$!";
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

    system(qq{ <test-eventXYZ.json ./strfry --config test/strfry.conf import --no-gc 2>/dev/null });

    system(qq{ rm test-eventXYZ.json });

    my $event = decode_json($eventJson);

    return $event->{id};
}
