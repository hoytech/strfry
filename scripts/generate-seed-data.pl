#!/usr/bin/env perl

use strict;
use warnings;

use Getopt::Long qw(GetOptions);
use JSON::PP ();

my %cfg = (
    output            => 'seeds/events.jsonl',
    seed              => 1337,
    users             => 500,
    kind1_notes       => 70_000,
    kind4_dms         => 8_000,
    kind7_reactions   => 8_000,
    replaceable       => 6_000,
    param_replaceable => 10_000,
    ephemeral         => 4_000,
    deletions         => 2_500,
    other             => 3_000,
    duplicates        => 1_500,
    start_ts          => 1_700_000_000,
);

my $help;

GetOptions(
    'output|o=s'           => \$cfg{output},
    'seed=i'               => \$cfg{seed},
    'users=i'              => \$cfg{users},
    'kind1-notes=i'        => \$cfg{kind1_notes},
    'kind0-profiles=i'     => \$cfg{kind0_profiles},
    'kind3-contacts=i'     => \$cfg{kind3_contacts},
    'kind4-dms=i'          => \$cfg{kind4_dms},
    'kind7-reactions=i'    => \$cfg{kind7_reactions},
    'replaceable=i'        => \$cfg{replaceable},
    'param-replaceable=i'  => \$cfg{param_replaceable},
    'ephemeral=i'          => \$cfg{ephemeral},
    'deletions=i'          => \$cfg{deletions},
    'other=i'              => \$cfg{other},
    'duplicates=i'         => \$cfg{duplicates},
    'start-ts=i'           => \$cfg{start_ts},
    'help|h'               => \$help,
) or die usage();

if ($help) {
    print usage();
    exit 0;
}

validate_non_negative_int('seed', $cfg{seed});
validate_positive_int('users', $cfg{users});
validate_non_negative_int('start-ts', $cfg{start_ts});

$cfg{kind0_profiles} = $cfg{users} * 2 unless defined $cfg{kind0_profiles};
$cfg{kind3_contacts} = $cfg{users} unless defined $cfg{kind3_contacts};

for my $k (qw(
    kind0_profiles
    kind1_notes
    kind3_contacts
    kind4_dms
    kind7_reactions
    replaceable
    param_replaceable
    ephemeral
    deletions
    other
    duplicates
)) {
    validate_non_negative_int($k, $cfg{$k});
}

my $planned_total = 0;
for my $k (qw(
    kind0_profiles
    kind1_notes
    kind3_contacts
    kind4_dms
    kind7_reactions
    replaceable
    param_replaceable
    ephemeral
    deletions
    other
    duplicates
)) {
    $planned_total += $cfg{$k};
}

my $out_fh;
if ($cfg{output} eq '-') {
    $out_fh = *STDOUT;
} else {
    require File::Basename;
    require File::Path;
    my $parent = File::Basename::dirname($cfg{output});
    File::Path::make_path($parent) if $parent ne '' && $parent ne '.';
    open($out_fh, '>', $cfg{output}) or die "unable to open $cfg{output} for writing: $!";
}
binmode($out_fh, ':raw');

my $json = JSON::PP->new->ascii->canonical;

my @topics = qw(
    bitcoin nostr strfry relay dev test db query monitor sync import export
    music coding privacy opensource tech memes books data
);

my @words = qw(
    amber apple arc atlas bamboo basil beacon birch blade bloom breeze byte
    cedar cloud comet coral crater dawn delta drift ember fern fiber flame
    flux forest frost galaxy glider grain harbor hazel icon ivy jet kernel
    lake leaf linen logic lunar maple meadow metric mist native neon nexus
    node nova omega orbit pebble pine pixel pulse quartz quiet radar reef
    ridge ripple river saffron sage shard signal snow socket solar spark
    spring stone stream summit tensor tide trail vector velvet wave willow
    wind winter yarn zenith
);

my @reaction_contents = qw(+ - like boost agree wow nice);
my @replaceable_kinds = (10_000 .. 10_031);
my @param_kinds       = (30_000 .. 30_031);
my @ephemeral_kinds   = ((20_000 .. 20_031), 22_242);
my @other_kinds       = (2, 6, 8, 16, 40, 42, 50, 1_984, 9_734, 9_735, 9_802);

my $rng_state = $cfg{seed} & 0xffff_ffff;
$rng_state = 1 if $rng_state == 0;

my $clock = $cfg{start_ts};
my $written = 0;

my @users;
my @events_by_user;
my @note_pool;
my @latest_kind0_ts;
my @latest_kind3_ts;
my %latest_replaceable_ts;
my @param_latest;
my @dup_pool;

my $note_pool_cap = 20_000;
my $dup_pool_cap = $cfg{duplicates} > 0 ? ($cfg{duplicates} < 5_000 ? $cfg{duplicates} : 5_000) : 0;

sub usage {
    return <<'USAGE';
Usage:
  perl scripts/generate-seed-data.pl [options]

Produces deterministic, line-delimited JSON (jsonl) test data for strfry import.
Generated events intentionally cover multiple kinds/tags and replacement/deletion scenarios.

Options:
  --output, -o <path>         Output file path. Use '-' for stdout. (default: seeds/events.jsonl)
  --seed <int>                Deterministic seed. (default: 1337)
  --users <int>               Number of users/authors. (default: 500)
  --kind1-notes <int>         Number of kind 1 notes. (default: 70000)
  --kind0-profiles <int>      Number of kind 0 profile events. (default: users*2)
  --kind3-contacts <int>      Number of kind 3 contact-list events. (default: users)
  --kind4-dms <int>           Number of kind 4 events. (default: 8000)
  --kind7-reactions <int>     Number of kind 7 events. (default: 8000)
  --replaceable <int>         Number of kind 10000-10031 events. (default: 6000)
  --param-replaceable <int>   Number of kind 30000-30031 events. (default: 10000)
  --ephemeral <int>           Number of kind 20000-20031 (+22242) events. (default: 4000)
  --deletions <int>           Number of kind 5 deletion events. (default: 2500)
  --other <int>               Number of additional mixed-kind events. (default: 3000)
  --duplicates <int>          Number of duplicate lines to append. (default: 1500)
  --start-ts <int>            Base created_at timestamp. (default: 1700000000)
  --help, -h                  Show this help.

Example:
  perl scripts/generate-seed-data.pl --users 800 --kind1-notes 90000 --output seed-100k.jsonl
USAGE
}

sub validate_non_negative_int {
    my ($name, $value) = @_;
    die "$name must be a non-negative integer\n" unless defined $value && $value =~ /\A\d+\z/;
}

sub validate_positive_int {
    my ($name, $value) = @_;
    die "$name must be a positive integer\n" unless defined $value && $value =~ /\A\d+\z/ && $value > 0;
}

sub rng_u32 {
    $rng_state = (1664525 * $rng_state + 1013904223) & 0xffff_ffff;
    return $rng_state;
}

sub rng_int {
    my ($max) = @_;
    return 0 if !defined($max) || $max <= 0;
    return rng_u32() % $max;
}

sub chance {
    my ($numerator, $denominator) = @_;
    return 0 if $denominator <= 0 || $numerator <= 0;
    return (rng_u32() % $denominator) < $numerator;
}

sub random_hex {
    my ($chars) = @_;
    my $out = '';
    while (length($out) < $chars) {
        $out .= sprintf('%08x', rng_u32());
    }
    return substr($out, 0, $chars);
}

sub random_sentence {
    my ($min_words, $max_words) = @_;
    $max_words = $min_words if $max_words < $min_words;
    my $count = $min_words + rng_int($max_words - $min_words + 1);
    my @chosen;
    for (1 .. $count) {
        push @chosen, $words[rng_int(scalar @words)];
    }
    return join(' ', @chosen);
}

sub pick_other_user_idx {
    my ($current_idx) = @_;
    my $count = scalar @users;
    return 0 if $count <= 1;
    my $idx = rng_int($count - 1);
    $idx++ if $idx >= $current_idx;
    return $idx;
}

sub next_ts {
    $clock += 1 + rng_int(4);
    return $clock;
}

sub maybe_old_ts {
    my ($latest) = @_;
    if ($latest && chance(1, 4) && $latest > $cfg{start_ts} + 120) {
        return $latest - (1 + rng_int(90));
    }
    return next_ts();
}

sub append_topic_tags {
    my ($tags_ref, $max_new_tags) = @_;
    my $to_add = rng_int($max_new_tags + 1);
    my %seen = map { $_->[1] => 1 } grep { $_->[0] eq 't' } @$tags_ref;
    for (1 .. $to_add) {
        my $topic = $topics[rng_int(scalar @topics)];
        next if $seen{$topic}++;
        push @$tags_ref, ['t', $topic];
    }
}

sub maybe_add_expiration_tag {
    my ($tags_ref, $created_at) = @_;
    return unless chance(1, 30);
    my $expires = $created_at + 120 + rng_int(86400 * 14);
    push @$tags_ref, ['expiration', "$expires"];
}

sub make_event {
    my (%args) = @_;
    return {
        id         => $args{id} // random_hex(64),
        pubkey     => $args{pubkey},
        created_at => $args{created_at},
        kind       => $args{kind},
        tags       => $args{tags} // [],
        content    => $args{content} // '',
        sig        => $args{sig} // random_hex(128),
    };
}

sub remember_user_event {
    my ($user_idx, $event_id) = @_;
    push @{ $events_by_user[$user_idx] }, $event_id;
    shift @{ $events_by_user[$user_idx] } while @{ $events_by_user[$user_idx] } > 40;
}

sub remember_note_ref {
    my ($event_id, $pubkey) = @_;
    if (@note_pool < $note_pool_cap) {
        push @note_pool, [$event_id, $pubkey];
        return;
    }

    if (chance(1, 15)) {
        $note_pool[rng_int($note_pool_cap)] = [$event_id, $pubkey];
    }
}

sub pick_note_ref {
    return if !@note_pool;
    return $note_pool[rng_int(scalar @note_pool)];
}

sub remember_param_latest {
    my ($user_idx, $kind, $d_tag, $created_at) = @_;
    my $key = "$kind:$d_tag";
    my $old = $param_latest[$user_idx]{$key} // 0;
    if ($created_at > $old) {
        $param_latest[$user_idx]{$key} = $created_at;
    }
}

sub pick_existing_param_key {
    my ($user_idx, $kind) = @_;
    my @keys = grep { index($_, "$kind:") == 0 } keys %{ $param_latest[$user_idx] };
    return if !@keys;
    return $keys[rng_int(scalar @keys)];
}

sub maybe_store_dup_line {
    my ($line) = @_;
    return if $dup_pool_cap == 0;
    if (@dup_pool < $dup_pool_cap) {
        push @dup_pool, $line;
        return;
    }
    if (chance(1, 80)) {
        $dup_pool[rng_int($dup_pool_cap)] = $line;
    }
}

sub emit_line {
    my ($line) = @_;
    print {$out_fh} $line, "\n" or die "failed writing output: $!";
    $written++;
}

sub emit_event {
    my ($event, %opts) = @_;
    my $line = $json->encode($event);
    emit_line($line);

    if (defined $opts{user_idx}) {
        remember_user_event($opts{user_idx}, $event->{id});
    }

    if ($opts{is_note}) {
        remember_note_ref($event->{id}, $event->{pubkey});
    }

    if ($opts{track_param}) {
        my ($kind, $d_tag) = @{ $opts{track_param} };
        remember_param_latest($opts{user_idx}, $kind, $d_tag, $event->{created_at});
    }

    maybe_store_dup_line($line);
}

@users = map { random_hex(64) } (1 .. $cfg{users});
@events_by_user = map { [] } @users;
@latest_kind0_ts = (0) x scalar(@users);
@latest_kind3_ts = (0) x scalar(@users);
@param_latest = map { {} } @users;

for (1 .. $cfg{kind0_profiles}) {
    my $user_idx = rng_int(scalar @users);
    my $created_at = maybe_old_ts($latest_kind0_ts[$user_idx]);
    $latest_kind0_ts[$user_idx] = $created_at if $created_at > $latest_kind0_ts[$user_idx];

    my $content = JSON::PP::encode_json({
        name         => "user-$user_idx",
        display_name => "Seed User $user_idx",
        about        => random_sentence(6, 14),
        website      => "https://example.test/u/$user_idx",
        nip05        => "user$user_idx\@example.test",
    });

    my @tags;
    push @tags, ['t', $topics[rng_int(scalar @topics)]] if chance(1, 6);

    emit_event(make_event(
        pubkey     => $users[$user_idx],
        created_at => $created_at,
        kind       => 0,
        tags       => \@tags,
        content    => $content,
    ), user_idx => $user_idx);
}

for (1 .. $cfg{kind3_contacts}) {
    my $user_idx = rng_int(scalar @users);
    my $created_at = maybe_old_ts($latest_kind3_ts[$user_idx]);
    $latest_kind3_ts[$user_idx] = $created_at if $created_at > $latest_kind3_ts[$user_idx];

    my @tags;
    my %seen;
    my $num_contacts = 1 + rng_int(8);
    for (1 .. $num_contacts) {
        my $other = pick_other_user_idx($user_idx);
        next if $seen{$other}++;
        push @tags, ['p', $users[$other]];
    }
    append_topic_tags(\@tags, 1);

    emit_event(make_event(
        pubkey     => $users[$user_idx],
        created_at => $created_at,
        kind       => 3,
        tags       => \@tags,
        content    => '',
    ), user_idx => $user_idx);
}

for (1 .. $cfg{kind1_notes}) {
    my $user_idx = rng_int(scalar @users);
    my $created_at = next_ts();
    my @tags;

    append_topic_tags(\@tags, 3);

    my $ref = pick_note_ref();
    if ($ref && chance(7, 10)) {
        push @tags, ['e', $ref->[0], '', (chance(1, 2) ? 'reply' : 'root')];
        push @tags, ['p', $ref->[1]];
    }

    if (chance(1, 4)) {
        my $other = pick_other_user_idx($user_idx);
        push @tags, ['p', $users[$other]];
    }

    if (chance(1, 8)) {
        push @tags, ['r', 'wss://relay' . (1 + rng_int(4)) . '.example.test'];
    }

    maybe_add_expiration_tag(\@tags, $created_at);

    my $content = 'note ' . random_sentence(5, 18);
    if (@tags) {
        my @note_topics = map { $_->[1] } grep { $_->[0] eq 't' } @tags;
        $content .= ' #' . $note_topics[0] if @note_topics;
    }

    emit_event(make_event(
        pubkey     => $users[$user_idx],
        created_at => $created_at,
        kind       => 1,
        tags       => \@tags,
        content    => $content,
    ), user_idx => $user_idx, is_note => 1);
}

for (1 .. $cfg{kind4_dms}) {
    my $user_idx = rng_int(scalar @users);
    my $to_idx = pick_other_user_idx($user_idx);
    my $created_at = next_ts();
    my @tags = (['p', $users[$to_idx]]);

    my $ref = pick_note_ref();
    push @tags, ['e', $ref->[0]] if $ref && chance(1, 3);
    maybe_add_expiration_tag(\@tags, $created_at);

    emit_event(make_event(
        pubkey     => $users[$user_idx],
        created_at => $created_at,
        kind       => 4,
        tags       => \@tags,
        content    => 'dm ' . random_sentence(4, 12),
    ), user_idx => $user_idx);
}

for (1 .. $cfg{kind7_reactions}) {
    my $user_idx = rng_int(scalar @users);
    my $created_at = next_ts();
    my @tags;

    my $ref = pick_note_ref();
    if ($ref) {
        push @tags, ['e', $ref->[0]];
        push @tags, ['p', $ref->[1]];
    }
    append_topic_tags(\@tags, 1);

    emit_event(make_event(
        pubkey     => $users[$user_idx],
        created_at => $created_at,
        kind       => 7,
        tags       => \@tags,
        content    => $reaction_contents[rng_int(scalar @reaction_contents)],
    ), user_idx => $user_idx);
}

for (1 .. $cfg{replaceable}) {
    my $user_idx = rng_int(scalar @users);
    my $kind = $replaceable_kinds[rng_int(scalar @replaceable_kinds)];
    my $key = "$user_idx:$kind";
    my $created_at = maybe_old_ts($latest_replaceable_ts{$key} // 0);
    $latest_replaceable_ts{$key} = $created_at if $created_at > ($latest_replaceable_ts{$key} // 0);

    my @tags;
    push @tags, ['d', 'ignored-' . rng_int(64)] if chance(1, 3);
    append_topic_tags(\@tags, 2);

    emit_event(make_event(
        pubkey     => $users[$user_idx],
        created_at => $created_at,
        kind       => $kind,
        tags       => \@tags,
        content    => "replaceable kind $kind " . random_sentence(3, 11),
    ), user_idx => $user_idx);
}

for (1 .. $cfg{param_replaceable}) {
    my $user_idx = rng_int(scalar @users);
    my $kind = $param_kinds[rng_int(scalar @param_kinds)];
    my $existing_key = pick_existing_param_key($user_idx, $kind);
    my $d_tag;

    if (defined $existing_key && chance(3, 5)) {
        (undef, $d_tag) = split(/:/, $existing_key, 2);
    } else {
        $d_tag = chance(1, 25) ? '' : 'd-' . rng_int(20_000);
    }

    my $param_key = "$kind:$d_tag";
    my $created_at = maybe_old_ts($param_latest[$user_idx]{$param_key} // 0);
    my @tags = (['d', $d_tag]);

    append_topic_tags(\@tags, 2);
    if (chance(1, 4)) {
        my $other = pick_other_user_idx($user_idx);
        push @tags, ['p', $users[$other]];
    }

    emit_event(make_event(
        pubkey     => $users[$user_idx],
        created_at => $created_at,
        kind       => $kind,
        tags       => \@tags,
        content    => "param kind $kind d=$d_tag " . random_sentence(3, 10),
    ), user_idx => $user_idx, track_param => [$kind, $d_tag]);
}

for (1 .. $cfg{ephemeral}) {
    my $user_idx = rng_int(scalar @users);
    my $kind = $ephemeral_kinds[rng_int(scalar @ephemeral_kinds)];
    my $created_at = next_ts();
    my @tags;

    if (chance(1, 2)) {
        my $other = pick_other_user_idx($user_idx);
        push @tags, ['p', $users[$other]];
    }
    append_topic_tags(\@tags, 1);
    maybe_add_expiration_tag(\@tags, $created_at);

    emit_event(make_event(
        pubkey     => $users[$user_idx],
        created_at => $created_at,
        kind       => $kind,
        tags       => \@tags,
        content    => "ephemeral kind $kind " . random_sentence(3, 8),
    ), user_idx => $user_idx);
}

for (1 .. $cfg{other}) {
    my $user_idx = rng_int(scalar @users);
    my $kind = $other_kinds[rng_int(scalar @other_kinds)];
    my $created_at = next_ts();
    my @tags;

    if ($kind == 6 || $kind == 16 || $kind == 42 || $kind == 9_734 || $kind == 9_735) {
        my $ref = pick_note_ref();
        if ($ref) {
            push @tags, ['e', $ref->[0]];
            push @tags, ['p', $ref->[1]];
        }
    }
    append_topic_tags(\@tags, 2);

    emit_event(make_event(
        pubkey     => $users[$user_idx],
        created_at => $created_at,
        kind       => $kind,
        tags       => \@tags,
        content    => "kind $kind " . random_sentence(3, 10),
    ), user_idx => $user_idx);
}

for (1 .. $cfg{deletions}) {
    my $user_idx = rng_int(scalar @users);
    my @tags;
    my %seen;

    my $event_ids = $events_by_user[$user_idx];
    if (@$event_ids) {
        my $num_e = 1 + rng_int(3);
        for (1 .. $num_e) {
            my $id = $event_ids->[rng_int(scalar @$event_ids)];
            next if $seen{"e:$id"}++;
            push @tags, ['e', $id];
        }
    }

    my @param_keys = keys %{ $param_latest[$user_idx] };
    if (@param_keys && chance(2, 3)) {
        my $num_a = 1 + rng_int(2);
        for (1 .. $num_a) {
            my $key = $param_keys[rng_int(scalar @param_keys)];
            next if $seen{"a:$key"}++;
            my ($kind, $d_tag) = split(/:/, $key, 2);
            push @tags, ['a', "$kind:$users[$user_idx]:$d_tag"];
        }
    }

    push @tags, ['e', random_hex(64)] if !@tags;

    emit_event(make_event(
        pubkey     => $users[$user_idx],
        created_at => next_ts(),
        kind       => 5,
        tags       => \@tags,
        content    => 'delete ' . random_sentence(2, 6),
    ), user_idx => $user_idx);
}

for (1 .. $cfg{duplicates}) {
    last if !@dup_pool;
    emit_line($dup_pool[rng_int(scalar @dup_pool)]);
}

close($out_fh) if $cfg{output} ne '-';

my $destination = $cfg{output} eq '-' ? 'stdout' : $cfg{output};
print STDERR "wrote $written events (planned $planned_total) to $destination with seed=$cfg{seed}\n";
