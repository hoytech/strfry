# strfry testing docs

Tests should be run from the *root* of the project.

## Tests for event writing, including replacements, deletions, etc:

    node test/writeTest.js

## Restricted read tests (REQ/COUNT/negentropy + ReadRestrictor logic):

    node test/readRestrictTest.js

## Security regression tests (nesting depth, NIP-11 config, negentropy,
## long tags, metrics, compressed payload limits, COUNT restrictions):

    node test/tests/securityTest.js

## Fuzz tests

Note that these tests need a well populated DB. For best coverage, use the [wellordered 500k](https://wiki.wellorder.net/wiki/nostr-datasets/) data-set:

    zstdcat ../nostr-dumps/nostr-wellorder-early-500k-v1.jsonl.zst | ./strfry import

If successful, these tests will run forever, so you can run them overnight to see if any errors occur.

The tests are deterministic, but you can change the seed by setting the `SEED` env variable.

These commands test the query engine, with and without `limit`:

    perl test/filterFuzzTest.pl scan-limit
    perl test/filterFuzzTest.pl scan

These commands test the monitor engine:

    perl test/filterFuzzTest.pl monitor
