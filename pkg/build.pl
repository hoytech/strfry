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
        'strfry.conf' => '/etc/strfry.conf',
    },
    dirs => {
    },
    config_files => [
        '/etc/strfry.conf',
    ],
    #postinst => 'pkg/scripts/postinst',
    # ssl dev pkg so we don't hard-code openssl ABI version (works with multiple)
    deps => [qw/
        zlib1g
        libssl-dev
        liblmdb0
        libflatbuffers1
        libsecp256k1-0
        libzstd1
        systemd-coredump
    /],
});
