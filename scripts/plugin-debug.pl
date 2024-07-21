#!/usr/bin/env perl

use strict;
use IPC::Open2;
use JSON::XS;

my $cmd = shift // die "usage: $0 <plugin-command>";
my $timeout = 5; # seconds

my ($childOut, $childIn);
my $pid = open2($childOut, $childIn, $cmd);

STDOUT->autoflush(1);
$childIn->autoflush(1);

eval {
    process();
};

if ($@) {
    kill 'TERM', $pid;
    die "PLUGIN-DEBUG failure: $@";
} else {
    die "PLUGIN-DEBUG exited normally";
}


sub process {
    while (my $inLine = <STDIN>) {
        my $parsedIn = parseJson($inLine);
        my $id = $parsedIn->{event}->{id};

        my $outLine;

        eval {
            local $SIG{ALRM} = sub { die "timeout\n" };
            alarm $timeout;

            print $childIn $inLine;
            $outLine = <$childOut>;

            alarm 0;

            my $parsedOut = parseJson($outLine);
            if ($parsedOut->{id} ne $id) {
                die "id mismatch (got $parsedOut->{id})";
            }

            print $outLine;
        };

        if ($@) {
            die "error on id $id: $@";
        }
    }
}

sub parseJson {
    my $inp = shift;
    chomp $inp;

    my $out;

    eval {
        $out = decode_json($inp);
    };

    if ($@) {
        die "failure decoding JSON: $inp ($@)";
    }

    return $out;
}
