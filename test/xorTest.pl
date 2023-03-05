#!/usr/bin/env perl

use IPC::Open2;
use Session::Token;

my $idSize = 16;

srand($ENV{SEED} || 0);
my $stgen = Session::Token->new(seed => "\x00" x 1024, alphabet => '0123456789abcdef', length => $idSize * 2);


while(1) {
    my $pid = open2(my $outfile, my $infile, './test/xor');

    my $num = rnd(10000) + 1;

    for (1..$num) {
        my $mode;
        if (rand() < 0.001) {
            $mode = rnd(2) + 1;
        } else {
            $mode = 3;
        }
        my $created = 1677970534 + rnd($num);
        my $id = $stgen->get;
        print $infile "$mode,$created,$id\n";
    }

    close($infile);

    while (<$outfile>) {
        print $_;
    }

    waitpid($pid, 0);
    my $child_exit_status = $?;
    die "failure" if $child_exit_status;

    print "\n-----------OK-----------\n";
}


sub rnd {
    my $n = shift;
    return int(rand() * $n);
}
