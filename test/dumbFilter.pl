#!/usr/bin/env perl

use strict;

use JSON::XS;

binmode(STDOUT, ":utf8");

my $filterJson = shift || die "need filter";
my $filter = decode_json($filterJson);

while(<STDIN>) {
    my $ev = decode_json($_);

    if (doesMatch($ev, $filter)) {
        print $_;
    }
}


sub doesMatch {
    my $ev = shift;
    my $filter = shift;

    $filter = [$filter] if ref $filter eq 'HASH';

    foreach my $singleFilter (@$filter) {
        return 1 if doesMatchSingle($ev, $singleFilter);
    }

    return 0;
}

sub doesMatchSingle {
    my $ev = shift;
    my $filter = shift;

    if (defined $filter->{since}) {
        return 0 if $ev->{created_at} < $filter->{since};
    }

    if (defined $filter->{until}) {
        return 0 if $ev->{created_at} > $filter->{until};
    }

    if ($filter->{ids}) {
        my $found;
        foreach my $id (@{ $filter->{ids} }) {
            if ($ev->{id} eq $id) {
                $found = 1;
                last;
            }
        }
        return 0 if !$found;
    }

    if ($filter->{authors}) {
        my $found;
        foreach my $author (@{ $filter->{authors} }) {
            if ($ev->{pubkey} eq $author) {
                $found = 1;
                last;
            }
        }
        return 0 if !$found;
    }

    if ($filter->{kinds}) {
        my $found;
        foreach my $kind (@{ $filter->{kinds} }) {
            if ($ev->{kind} == $kind) {
                $found = 1;
                last;
            }
        }
        return 0 if !$found;
    }

    # AND / OR tag handling (including NIP-119 AND filters)
    my %tagAnd;
    my %tagOr;
    for my $k (keys %$filter) {
        if ($k =~ /^#(.)$/) {
            $tagOr{$1} = $filter->{$k};
        } elsif ($k =~ /^&(.)$/) {
            $tagAnd{$1} = $filter->{$k};
        }
    }

    # Remove overlaps: AND values are ignored in OR sets for the same tag
    for my $tag (keys %tagAnd) {
        next unless $tagOr{$tag};
        my %andVals = map { $_ => 1 } @{ $tagAnd{$tag} };
        my @remaining = grep { !exists $andVals{$_} } @{ $tagOr{$tag} };
        if (@remaining) {
            $tagOr{$tag} = \@remaining;
        } else {
            delete $tagOr{$tag};
        }
    }

    # AND: every required value must be present
    for my $tag (keys %tagAnd) {
        for my $required (@{ $tagAnd{$tag} }) {
            my $found;
            foreach my $evTag (@{ $ev->{tags} }) {
                next if @$evTag < 2;
                if ($evTag->[0] eq $tag && $evTag->[1] eq $required) {
                    $found = 1;
                    last;
                }
            }
            return 0 if !$found;
        }
    }

    # OR: at least one value must be present per tag key
    for my $tag (keys %tagOr) {
        my $found;
        foreach my $search (@{ $tagOr{$tag} }) {
            foreach my $evTag (@{ $ev->{tags} }) {
                next if @$evTag < 2;
                if ($evTag->[0] eq $tag && $evTag->[1] eq $search) {
                    $found = 1;
                    last;
                }
            }
            last if $found;
        }
        return 0 if !$found;
    }

    return 1;
}
