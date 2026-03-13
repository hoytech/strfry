#!/usr/bin/env perl

use strict;

use lib 'golpe/';
use BuildLib;


my $version = `git describe --tags 2>/dev/null` || die "couldn't get version";


BuildLib::fpm({
    types => [qw/ deb /],
    name => 'strfry',
    version => $version,
    description => 'strfry',
    files => {
        'strfry' => '/usr/local/bin/strfry',
    },
    dirs => {
    },
    config_files => [
    ],
    # -dev packages so we don't have to hard-code ABI versions
    deps => [qw/
        zlib1g
        libssl-dev
        liblmdb-dev
        libflatbuffers-dev
        libsecp256k1-dev
        libzstd-dev
        libre2-dev
        systemd-coredump
    /],
});
