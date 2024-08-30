#!/usr/bin/env perl

use strict;
use Data::Dumper;
use JSON::XS;
use IPC::Open2;


# ./strfry export|perl -MJSON::XS -nE '$z=decode_json($_); for my $t (@{$z->{tags}}) { say $t->[1] if $t->[0] eq "e"}'|sort|uniq -c|sort -rn|head -50|perl -nE '/\d+\s+(\w+)/ && say $1'


my $kinds = [qw/1 7 4 42 0 30 3 6/];

my $pubkeys = [qw{
887645fef0ce0c3c1218d2f5d8e6132a19304cdc57cd20281d082f38cfea0072
f4161c88558700d23af18d8a6386eb7d7fed769048e1297811dcc34e86858fb2
32e1827635450ebb3c5a7d12c1f8e7b2b514439ac10a67eef3d9fd9c5c68e245
3b57518d02e6acfd5eb7198530b2e351e5a52278fb2499d14b66db2b5791c512
3235036bd0957dfb27ccda02d452d7c763be40c91a1ac082ba6983b25238388c
2183e94758481d0f124fbd93c56ccaa45e7e545ceeb8d52848f98253f497b975
2d1ac20dbb78936ca088c7824683e7b59fb2774ac86a63c72480514d8cecc0aa
b2d670de53b27691c0c3400225b65c35a26d06093bcc41f48ffc71e0907f9d4a
3104f98515b3aa147d55d9c2951e0f953b829d8724381d8f0d824125d7727634
5c10ed0678805156d39ef1ef6d46110fe1e7e590ae04986ccf48ba1299cb53e2
c2b373077c70c490e540ce029a346949b893964dd9e06da1d2facbc49d6ffe5c
47f7163bed3bdb80dc8b514693293d588710607018855cb5a53f4bb6ddba8377
9ec7a778167afb1d30c4833de9322da0c08ba71a69e1911d5578d3144bb56437
6da123ce3bb5245484a84ad9f57c32f4da01f4d67f9905c530ca2e9691ea68de
f19c4407f08fc3e9b2957f290272f6d8c2ebae5854704a03f5900779b8aaa664
543210b5f6c3071c3135d850449f8bf91efffb5ed1153e5fcbb2d95b79262b57
00000000827ffaa94bfea288c3dfce4422c794fbb96625b6b31e9049f729d700
42b3f07844b2ad2717078abb47019369cee2aeae79469f8313ede9d75806cf61
4b5520fd1bdcb6f11a8847e2c980f07ba873488a097467186ffeb68f955b9273
552d5a1dcdc23eee687934791ae6da53e36e038924b314729cb7641745e78563
}];

my $ids = [qw{
25e5c82273a271cb1a840d0060391a0bf4965cafeb029d5ab55350b418953fbb
ca178c4ecea83fa7f7b04345be4587cf03c7d8775f50014e31caf6869a626354
4211fc228be5af10923f56e60b1b11b8e63bf0ac7dbd3e1e3d767392fdaed4a4
f06a690997a1b7d8283c90a7224eb8b7fe96b7c3d3d8cc7b2e7f743532c02b42
2b48218edd23e88fd33ec23d6d91fd7203a26497d74d4ba54cbae91e3b6e169e
6c99281bf6ff2715fddcdd1d255db5b93a852930acea28a09374d9de868dcfab
936041f4a0b0625e08982e98b85795396b391400750638698bd71269271f5bdd
7ac6abd15d03736ef883716ac152ad8d066a748fc8e048b542decea52496c12b
9ba8717d61d9dfdad7d7b260ae33566241e3a55ecd26c2dcb944b47b1ef21eb7
3561b3054737b1b126e607d574f230ca17ababe6ef803070e8967c3de607a620
59c23027c484936ccbf408369fc8105467b15e142213737631fcf3518017e168
b1791d7fc9ae3d38966568c257ffb3a02cbf8394cdb4805bc70f64fc3c0b6879
52cab2e3e504ad6447d284b85b5cc601ca0613b151641e77facfec851c2ca816
a382aed3ba436a7d6c98ec41e2477b370e0332689cdb04b09b8dd8a95d1210f9
453c1d471f6cfd6d99fbb344e61229f9a0a1d8c96764b5ac1a8f0aa785e293a5
020b587a1627e42d4b94f14b29d6cd9328635712b1e75daed9c178815d6b2f5f
c773cfe264b3035ebfbbc2b5c874a1859f671320ab24e09bd56559ec4e48e903
fbd74e99301046798d0dbbef6ff3e14ea1305884eeb09068f84a00361501a0d4
83ee9be878407dc4a9f8a6cfff54227a66745532a78b56443e36b4c3c3711189
342060554ca30a9792f6e6959675ae734aed02c23e35037d2a0f72ac6316e83d
01846005bb00245e06bdb9ab4f85f0b0624ac408816bf1c0c691ebb6dcaba23e
846b8aa598d81379f7f36f5a94165d1a3b5e4cc080f3badd681e75aa03e8a806
08470808369f03c2157607ffcb441f91305d207f249aae4c08373bdbba2431ea
1f304d32f1db468ca84fb15b7182a38a8511a991ece50920683efc23461550c7
81888882d8183843299dc6625746c69d5cc37281c1c62aa69d63aa6e9f197a31
ff693a532e4f2bd3ae7657b12174d338ca906fc9bea18910b06a795c4552d4c5
2b7a291d69c07f5523837616634a9f182ab2833f2a3ce21312b8c400963f366b
b0d766f01b1cc883a21c5dc2553f1a7246254d61f08760717413c9b570510f88
032356b66ee1608f156c800f261b36aac254d49f895e2cd725f19bbdfeb8a8c5
a6e1e4ea75050c57a814bb4f098d0690b3577cd84fbc0db74b0fd3e924db3071
81ed7bbaeb3bbec4f3816605dfb45cad85dfd99931df266df8d018d65d874bbd
786485a61011ceed9e373abdf6485c5ca070e2bcc50c457b8f817cd275bcbf00
00000ea184b1d9e3688ddbfd13d2f8bc0893ab73d8a5c539b85c7d168ec5423e
f25c0cddcca28603db780fdefceeebf1cc3b3ce69f48bb0dcbb4c1d0bfd68d6c
b7436420d5ce4521ba3130e522414eb146814dae74434108688dcd225d5db5ea
d4c9bc1ca5dff4a371f8f10f24013211c67e789d353913f28ae24be13f267c58
26b2d82df41d68da1b684ac99b4adadc2d272d49590155850789251cc3c80f84
6717375662b966120041dbe5cad98d6704861d57589a40ad7cfc5e250d653511
ec25b9c7ff8fa8ccdc7d2e3bfa06df82448a88c40212c6d19bce4a6f747b736b
508874b1c08c9c57f174c5101eda831362f30cbc4147e96f5b9b5338b7c3654d
07c9ee7d5704d544cec36f7888b3fa6183cee744e598f603fde5e06be8f88c81
9a8e1f60401d277c36986fad81234d14a655463ecdeae74b2f89754bc07a109a
ea5d104277e42b35ca260fa7006a119c4d2b1404d5c53c94d67973c6668acf5c
4e79025204e0860dc601a2d7147005f6173d7ae7a9cd782da71e6dbab9d22b37
d6c246cc94a9348bdf4f71e867db235d2ba457007b669984003d845c4dd7237f
0333ed329f08aadabb62d099809ab0fa05de0c4bcc2c5aeea9244456ae607e71
47b2a3875e37296ac5b872f94bda9a9dab52a71e1d161b861b5f7691bca2189f
6c09c4dca9a3466f22305084639f685faf2d5d62765a57064a35f3d20fe70559
c1e5e04d92d9bd20701bff4cbdac1cdc317d405035883b7adcf9a6a5308d0f54
3a15cb7cf951de54a23585ca003c96ca9a7c49fbf8e436575ff9bb710af301f0
}];

my $topics = [qw{
bitcoin
nos
nostr
nostrnovember
gitlog
introductions
jb55
damus
chat
nosuchtopic
}];

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
    while (1) {
        my $fg = genRandomFilterGroup();
        testScan($fg);
    }
} elsif ($cmd eq 'scan-limit') {
    while (1) {
        my $fg = genRandomFilterGroup(1);
        testScan($fg);
    }
} elsif ($cmd eq 'monitor') {
    while (1) {
        my ($monCmds, $interestFg) = genRandomMonitorCmds();
        testMonitor($monCmds, $interestFg);
    }
} else {
    die "unknown cmd: $cmd";
}
