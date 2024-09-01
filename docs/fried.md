# Fried Exports

When importing events with `strfry import`, most of the CPU time is spent on JSON parsing (assuming you use `--no-verify` to disable signature verification).

In order to speed this up, the `strfry export` and `strfry import` commands accept a `--fried` parameter. This causes the exported JSON events to have a `fried` field that contains a hex-encoded dump of the corresponding `PackedEvent`. This field must always be the *last* entry in each JSON line. Other than this extra field, these exports are just regular JSONL dumps.

When importing in fried mode, no JSON parsing will be performed. Instead, the packed data will be extracted directly from the JSON and installed into the DB. The fried field will be removed so it isn't stored or sent to clients. No signature verification or other validity checks are performed.

This optimisation speeds up import about 10x and the bottleneck becomes building the LMDB indices. Because the fried data is hex-encoded, it compresses quite well with the (mostly also hex-encoded) JSON fields. After compression, the overhead of `--fried` is ~7%.

Fried *export* functionality has been back-ported to the 0.9 strfry releases, but import only exists for the 1.0 series (since the packed data format has changed).

## PackedEvent format

PackedEvent contains the minimal set of data required for indexing a nostr event:

    // <offset>: <fieldName> (<size>)
    //
    // PackedEvent
    //   0: id (32)
    //  32: pubkey (32)
    //  64: created_at (8)
    //  72: kind (8)
    //  80: expiration (8)
    //  88: tags[] (variable)
    //
    // each tag:
    //   0: tag char (1)
    //   1: length (1)
    //   2: value (variable)

* Only indexable (single character) tags are included
* Tag values cannot be longer than 255 octets
* `e` and `p` tags are unpacked as raw 32 bytes (so they are not double hex-encoded in fried output)
* Integers are encoded in little-endian
* An expiration of `0` means no expiration
