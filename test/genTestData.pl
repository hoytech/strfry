#!/usr/bin/env perl

# Generates synthetic nostr-like events for testing filters including NIP-91 AND tag filters.
# Output is JSONL suitable for `strfry import`.

use strict;
use JSON::XS;

my $numEvents = shift || 5000;

sub fakehex { sprintf("%064x", int(rand() * 2**48) ^ ($_[0] * 7919)) }
my @pubkeys = map { fakehex($_) } (0..19);
my @event_ids = map { fakehex($_ + 1000) } (0..49);
my @topics = qw(bitcoin nos nostr nostrnovember gitlog introductions jb55 damus chat meme cat dog art music);
my @kinds = (0, 1, 3, 4, 6, 7, 30, 42);

srand(42); # deterministic

for my $i (0..$numEvents-1) {
    my $pubkey = $pubkeys[int(rand() * @pubkeys)];
    my $kind = $kinds[int(rand() * @kinds)];
    my $created_at = 1640300802 + int(rand() * 86400 * 365);
    my $content = "test event $i";

    my @tags;

    # Add e-tags
    if (rand() < 0.3) {
        my $num_e = int(rand() * 3) + 1;
        for (1..$num_e) {
            push @tags, ["e", $event_ids[int(rand() * @event_ids)]];
        }
    }

    # Add p-tags
    if (rand() < 0.3) {
        my $num_p = int(rand() * 2) + 1;
        for (1..$num_p) {
            push @tags, ["p", $pubkeys[int(rand() * @pubkeys)]];
        }
    }

    # Add t-tags (important for NIP-91 AND filter testing)
    if (rand() < 0.5) {
        my $num_t = int(rand() * 4) + 1;
        my %used;
        for (1..$num_t) {
            my $topic = $topics[int(rand() * @topics)];
            next if $used{$topic}++;
            push @tags, ["t", $topic];
        }
    }

    # Compute a fake but valid-looking id
    my $id = sprintf("%064x", int(rand() * 2**48) ^ ($i * 104729));

    # Fake sig (128 hex chars)
    my $sig = sprintf("%064x", int(rand() * 2**48)) . sprintf("%064x", int(rand() * 2**48));

    my $event = {
        id => $id,
        pubkey => $pubkey,
        created_at => $created_at + 0,
        kind => $kind + 0,
        tags => \@tags,
        content => $content,
        sig => $sig,
    };

    print encode_json($event), "\n";
}
