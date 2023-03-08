# XOR Syncing

This document describes a method for syncing nostr events over a nostr protocol extension. It is roughly based on the method described [here](https://github.com/AljoschaMeyer/set-reconciliation).

If both sides of the sync have common events, then this protocol will use less bandwidth than transferring the full set of events (or even just their IDs).

## High-Level Protocol

We'll call the two sides engaged in the sync the client and the relay (even though the initiating party may be another relay, not a regular client).

1. Client selects the parameters of a reconcilliation query and sends it to the relay:
    * A subscription ID that will be used by each side to identify which query a message refers to
    * A nostr filter, as described in [NIP-01](https://github.com/nostr-protocol/nips/blob/master/01.md), or an event ID whose `content` contains the JSON-encoded filter
    * The truncation byte size for IDs, called `idSize. An integer between 8 and 32, inclusive.
1. Both sides collect all the events they have stored/cached that match this filter.
1. Each side sorts the events by their `created_at` timestamp. If the timestamps are equivalent, events are sorted lexically by their `id` (the canonicalised hash of the event). 
1. Client creates an initial reconcilliation message and sends it to relay.
1. Each side processes an incoming message and replies with its own outgoing message, along with a list of "have IDs" and "need IDs", stopping when one party receives an empty message.

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

Varints (variable-sized integers) are a format for storing unsigned integers in a small number of bytes, commonly called BER (Binary Encoded Representation). They are stored as base 128 digits, most significant digit first, with as few digits as possible. Bit eight (the high bit) is set on each byte except the last.

    Varint := <Digit+128>* <Digit>

### Bound

The protocol frequently transmits the bounds of a range of events. Each range is specified by a (inclusive) lower bound, followed by an (exclusive) upper bound.

Each bound is specified by a timestamp (see below), and a disambiguating prefix of an event (in case multiple events have the same timestamp):

    Bound := <encodedTimestamp (Varint)> <length (Varint)> <id (Byte)>*

The timestamp is encoded specially. The value `0` is reserved for the maximum allowable timestamp (such that all valid events preceed it). All other values are `1 + offset`, where offset is the difference between this timestamp and the previously encoded timestamp. The initial offset starts at 0 and resets at the beginning of each message.

Offsets are always positive numbers since the upper bound's timestamp is always `>=` to the lower bound's, ranges in a message are always encoded in ascending order, and ranges never overlap.

### Range

A range consists of a pair of bounds, a mode, and a payload (determined by the mode):

    Range := <lower (Bound)> <upper (Bound)> <mode (Byte)> <Xor | IdList>

If the mode is 0, then the payload is the bitwise eXclusive OR of all the IDs in this range, truncated to `idSize`:

    Xor := <digest (Byte)>{idSize}

If the mode is `>= 8`, then this is a list of IDs in the range. The number of entries is equal to `mode - 8`:

    IdList := <length+8 (Varint)> <id (Byte)>{idSize}

### Message

A reconcilliation message is just an ordered list of ranges, or the empty string if no ranges need reconciling:

    Message := <Range>*


## Algorithm

After the initial setup, the algorithm is the same for both client and relay (who alternate between sender and receiver).

The message receiver should loop over all the ranges of its incoming message and build its next outgoing message at the same time.

Each range in a message encodes the information for the receiving side to determine if their portion of the event set matches the sender's portion. For every incoming range, the receiver will add 0 more ranges to its next outgoing message, depending on whether more syncing needs to occur for this range or not.

### Base Case

If the mode of a range is `>= 8`, then the sender has decided to send all the IDs it has within this range. Typically it will choose a cut-off, and use this mode if there are fewer than `N` events to send.

For each received ID, the receiver should determine if it has this event. If not, it should add it to its next `needIDs` message. Additionally, each event that the receiver has that does not appear in the received IDs list should be added to the next `haveIDs` message. Since this range has been fully reconciled, no additional ranges should be pushed onto the next outgoing message.

### Splitting

If the mode is `0`, then the sender is sending a bitwise eXclusive OR of all the IDs it has within this range, indicating that it has too many events to send outright, and would like to determine if the two sides have some common events.

Upon receipt of an XOR range, the receiver should compute and compare the XOR of its own events within this range. If they are identical, then the receiver should simply ignore the range. Otherwise, it should respond with additional ranges. If its own set of events within this range is small, it can reply with the base case mode above.

However, if a receiver's set of events within the range is large, it should instead "split" the range into multiple sub-ranges, each of which gets appended onto the next outgoing message. These sub-ranges must totally cover the original range, meaning that the start of the first sub-range must match the original range's start (*not* the first matching element in its set). Likewise for the end of the range. Also, each sub-range's lower bound must be equal to the previous range's upper bound. The overhead of this duplication is mostly erased by the timestamp offset encoding described above.

How to split the range is implementation-dependent. The simplest way is to divide the elements that fall within the range into N equal-sized buckets, and emit an XOR-mode sub-range for each bucket. However, an implementation could choose different grouping criteria. For example, events with similar timestamps could be grouped into a single bucket. If the implementation believes the other side is less likely to have recent events, it could make the most recent bucket a base-case-mode.

Note that if alternate grouping strategies are used, an implementation should never reply to an XOR-range with another single XOR-range, otherwise the protocol may never terminate.

### Termination

If a receiver has looped over all incoming ranges and has added no new ranges to the next outgoing message, the sets have been fully reconciled. It still must send a final message with the value of an empty string (`""`) to let the other side know it is finished, as well as to transmit any final have/need IDs.





## Extensions

Don't need to push all your ranges right away if message is getting too big

Don't need to send need or have IDs if you're not interested in syncing in that direction
