#!/usr/bin/env perl

use strict;
use Data::Dumper;
use JSON::XS;
use IPC::Open2;

# ./strfry export|perl -MJSON::XS -nE '$z=decode_json($_); for my $t (@{$z->{tags}}) { say $t->[1] if $t->[0] eq "e"}'|sort|uniq -c|sort -rn|head -50|perl -nE '/\d+\s+(\w+)/ && say $1'

my ($ids, $pubkeys, $kinds, $topics) = ([], [], [], []);

open(my $fh, "-|", "./strfry --config test/cfgs/test.conf export 2>/dev/null | head -10000");

while (<$fh>) {
    my $ev = eval { decode_json($_) } or next;

    push @$ids, $ev->{id} if $ev->{id};
    push @$pubkeys, $ev->{pubkey} if $ev->{pubkey};
    push @$kinds, $ev->{kind} if defined $ev->{kind};

    for my $t (@{$ev->{tags} // []}) {
        push @$topics, $t->[1] if $t->[0] eq 't';
    }
}
close $fh;

push @topics, "nosuchtopic";

sub genRandomFilterGroup {
    my $useLimit = shift;

    my $numFilters = $useLimit ? 1 : (rand()*10)+1;

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
                $f->{'#e'} = [];
                for (1..(rand()*10)) {
                    push @{$f->{'#e'}}, $ids->[int(rand() * @$ids)];
                }
            }

            if (rand() < .2) {
                $f->{'#p'} = [];
                for (1..(rand()*5)) {
                    push @{$f->{'#p'}}, $pubkeys->[int(rand() * @$pubkeys)];
                }
            }

            if (rand() < .2) {
                $f->{'#t'} = [];
                for (1..(rand()*5)) {
                    push @{$f->{'#t'}}, $topics->[int(rand() * @$topics)];
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

sub genRandomMonitorCmds {
    my $nextConnId = 1;
    my @out;

    my $interestFg = genRandomFilterGroup();

    my $iters = int(rand() * 1000) + 100;

    for my $i (1..$iters) {
        if ($i == int($iters / 2)) {
            push @out, ["sub", 1000000, "mysub", $interestFg];
            push @out, ["interest", 1000000, "mysub"];
        } elsif (rand() < .9) {
            push @out, ["sub", $nextConnId++, "s" . int(rand() * 4), genRandomFilterGroup()];
        } elsif (rand() < .75) {
            push @out, ["removeSub", int(rand() * $nextConnId) + 1, "s" . int(rand() * 4)];
        } else {
            push @out, ["closeConn", int(rand() * $nextConnId) + 1];
        }
    }

    return (\@out, $interestFg);
}


sub testScan {
    my $fg = shift;
    my $fge = encode_json($fg);

    #print JSON::XS->new->pretty(1)->encode($fg);
    print "$fge\n";

    my $headCmd = @$fg == 1 && $fg->[0]->{limit} ? "| head -n $fg->[0]->{limit}" : "";

    my $resA = `./strfry export --reverse 2>/dev/null | perl test/dumbFilter.pl '$fge' $headCmd | jq -r .id | sort | sha256sum`;
    my $resB = `./strfry scan --pause 1 --metrics '$fge' | jq -r .id | sort | sha256sum`;

    print "$resA\n$resB\n";

    if ($resA ne $resB) {
        print STDERR "$fge\n";
        die "MISMATCH";
    }

    print "-----------MATCH OK-------------\n\n\n";
}


sub testMonitor {
    my $monCmds = shift;
    my $interestFg = shift;

    my $fge = encode_json($interestFg);
    print "filt: $fge\n\n";

    # HACK for debugging:
    #$fge = q{[{"#t":["nostrnovember","nostr"]}]};
    #$monCmds = [
    #    ["sub",1000000,"mysub",decode_json($fge)],
    #    ["interest",1000000,"mysub"],
    #];

    print "DOING MONS\n";
    my $pid = open2(my $outfile, my $infile, './strfry monitor | jq -r .id | sort | sha256sum');
    for my $c (@$monCmds) { print $infile encode_json($c), "\n"; }
    close($infile);

    my $resA = <$outfile>;

    waitpid($pid, 0);
    my $child_exit_status = $? >> 8;
    die "monitor cmd died" if $child_exit_status;

    print "DOING SCAN\n";
    my $resB = `./strfry scan '$fge' 2>/dev/null | jq -r .id | sort | sha256sum`;

    print "$resA\n$resB\n";

    if ($resA eq $resB) {
        print "-----------MATCH OK-------------\n\n\n";
    } else {
        print STDERR "$fge\n";
        die "MISMATCH";
    }
}



srand($ENV{SEED} || 0);

my $cmd = shift;

if ($cmd eq 'scan') {
    for (1..100) {
        my $fg = genRandomFilterGroup();
        testScan($fg);
    }
} elsif ($cmd eq 'scan-limit') {
    for (1..100) {
        my $fg = genRandomFilterGroup(1);
        testScan($fg);
    }
} elsif ($cmd eq 'monitor') {
    for (1..100) {
        my ($monCmds, $interestFg) = genRandomMonitorCmds();
        testMonitor($monCmds, $interestFg);
    }
} else {
    die "unknown cmd: $cmd";
}
