# XOR Syncing

This document describes a method for syncing nostr events over a nostr protocol extension. It is loosely based on the method described [here](https://github.com/AljoschaMeyer/set-reconciliation).

If both sides of the sync have common events, then this protocol will use less bandwidth than transferring the full set of events (or even just their IDs).

## High-Level Protocol

We'll call the two sides engaged in the sync the client and the relay (even though the initiating party may be another relay, not a regular client).

1. Client selects the parameters of a reconcilliation query and sends it to the relay:
  * A subscription ID that will be used by each side to identify which query a message refers to
  * A nostr filter, as described in [NIP-01](https://github.com/nostr-protocol/nips/blob/master/01.md), or an event ID whose `content` contains the JSON-encoded filter
  * The truncation byte size for IDs, called `idSize. An integer between 8 and 32, inclusive.
2. Both sides collect all the events they have stored/cached that match this filter.
3. Each side sorts the events by their `created_at` timestamp. If the timestamps are equivalent, events are sorted lexically by their `id` (the canonicalised hash of the event). 
4. Client creates an initial reconcilliation message and sends it to relay.
5. Each side processes an incoming message and replies with its own outgoing message, along with a list of "have IDs" and "need IDs", stopping when one party receives an empty message.

## Nostr Protocol

### Initial message (client to relay):

```json
[
    "XOR-OPEN",
    <subscription ID string>,
    <nostr filter or event ID>,
    <idSize>,
    <initialMessage, lowercase hex-encoded>
]
```

### Error message (relay to client):

If a request cannot be serviced, an error is returned (relay to client):

```json
[
    "XOR-ERR",
    <subscription ID string>,
    <reason code string>
]
```

Current reason codes are:

* `RESULTS_TOO_BIG`
* `FILTER_NOT_FOUND`

### Subsequent messages (bi-drectional):

```json
[
    "XOR-MSG",
    <subscription ID string>,
    <message, lowercase hex-encoded>,
    <have IDs, lowercase hex-encoded>,
    <need IDs, lowercase hex-encoded>
]
```

The have and need ID fields are event IDs truncated to `idSize`, concatenated together, then hex encoded. If there are `n` IDs, then the length after hex encoding is `n * idSize * 2`.

### Close message (client to relay):

```json
[
    "XOR-CLOSE",
    <subscription ID string>
]
```


## Reconcilliation Messages

### Varint

Varints are a format for storing unsigned integers in a small number of bytes, commonly called BER (Binary Encoded Representation). They are stored as base 128 digits, most significant digit first, with as few digits as possible. Bit eight (the high bit) is set on each byte except the last.

    Varint := <Digit+128>* <Digit>

### Bound

The protocol frequently transmits the bounds of a range of events. Each range is specified by a (inclusive) lower bound, followed by an (exclusive) upper bound.

Each bound is specified by a timestamp offset (see below), and a disambiguating prefix of an event (in case multiple events have the same timestamp):

    Bound := <timestampOffset (Varint)> <length (Varint)> <id (Byte)>*

Since the upper bound's timestamp is always `>=` to the lower bound's, ranges in a message are always encoded in ascending order, and ranges never overlap, each timestamp is encoded as an offset from the previous bound. The initial offset is 0.

### Range

A range consists of a pair of bounds, a mode, and a payload (determined by the mode):

    Range := <lower (Bound)> <upper (Bound)> <mode (Byte)> <Xor | IdList>

If the mode is 0, then the payload is the byte-wise exclusive OR of all the IDs in this range, truncated to `idSize`:

    Xor := <digest (Byte)>{idSize}

If the mode is `>= 8`, then this is a list of IDs in the range. The number of entries is equal to `mode - 8`:

    IdList := <length+8 (Varint)> <id (Byte)>{idSize}

### Message

A reconcilliation message is just an ordered list of ranges, or the empty string if no ranges need reconciling:

    Message := <Range>*
