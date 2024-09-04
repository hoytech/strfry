#!/usr/bin/env perl

use strict;

## Full DB sync tests
{
    my $f = '{}';
    test(qq{ 1 0 0 '$f' });
    test(qq{ 0 1 0 '$f' });
    test(qq{ 0 0 1 '$f' });
    test(qq{ 1 1 1000 '$f' });
}

## Vector DB sync tests
{
    my $f = '{"kinds":[1]}';
    test(qq{ 1 0 0 '$f' });
    test(qq{ 0 1 0 '$f' });
    test(qq{ 0 0 1 '$f' });
    test(qq{ 1 1 1000 '$f' });
}

## Full DB sync tests with time bounds
{
    my $f = '{"since":1652985767,"until":1662969916}';
    test(qq{ 1 1 1000 '$f' }, 100000);
    test(qq{ 0 0 1 '$f' }, 100000);

    $f = '{"since":1652985767}';
    test(qq{ 1 1 1100 '$f' }, 100000);

    $f = '{"until":1662969916}';
    test(qq{ 1 1 1200 '$f' }, 100000);
}


print "All OK\n";

sub test {
    my $params = shift;
    my $num = shift // 1000;

    print "---------------------------\n";
    print "TEST: params = $params  num = $num\n";

    my $redir = $ENV{VERBOSE} ? '' : '2>/dev/null';

    my $cmd = qq{ zstdcat ../nostr-dumps/nostr-wellorder-early-500k-v1.jsonl.zst | head -$num | perl test/syncTest.pl $params $redir};
    print "CMD: $cmd\n";
    system($cmd) && die "failed";
    print "\n";
}
