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

    if ($filter->{'#e'}) {
        my $found;
        foreach my $search (@{ $filter->{'#e'} }) {
            foreach my $tag (@{ $ev->{tags} }) {
                if ($tag->[0] eq 'e' && $tag->[1] eq $search) {
                    $found = 1;
                    last;
                }
            }
        }
        return 0 if !$found;
    }

    if ($filter->{'#p'}) {
        my $found;
        foreach my $search (@{ $filter->{'#p'} }) {
            foreach my $tag (@{ $ev->{tags} }) {
                if ($tag->[0] eq 'p' && $tag->[1] eq $search) {
                    $found = 1;
                    last;
                }
            }
        }
        return 0 if !$found;
    }

    if ($filter->{'#t'}) {
        my $found;
        foreach my $search (@{ $filter->{'#t'} }) {
            foreach my $tag (@{ $ev->{tags} }) {
                if ($tag->[0] eq 't' && $tag->[1] eq $search) {
                    $found = 1;
                    last;
                }
            }
        }
        return 0 if !$found;
    }

    return 1;
}
