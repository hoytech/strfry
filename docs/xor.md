# XOR Syncing

This document describes a method for syncing nostr events over a nostr protocol extension. It is roughly based on the method described [here](https://github.com/AljoschaMeyer/set-reconciliation).

If both sides of the sync have common events, then this protocol will use less bandwidth than transferring the full set of events (or even just their IDs).

## High-Level Protocol

We'll call the two sides engaged in the sync the client and the relay (even though the initiating party may be another relay, not a regular client).

1. Client selects the parameters of a reconcilliation query and sends it to the relay:
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

* The subscription ID is used by each side to identify which query a message refers to. It only needs to be long enough to distinguish it from an other concurrent XOR requests on this websocket connection (an integer that increases once per `XOR-OPEN` is fine)
* The nostr filter is as described in [NIP-01](https://github.com/nostr-protocol/nips/blob/master/01.md), or an event ID whose `content` contains the JSON-encoded filter, or an array of filters
* `idSize` indicates the truncation byte size for IDs, and should be an integer between 8 and 32, inclusive.
* `initialMessage` is described below.

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
* `EXPIRED`

### Subsequent messages (bi-drectional):

```json
[
    "XOR-MSG",
    <subscription ID string>,
    <message, lowercase hex-encoded>,
    <have IDs, lowercase hex-encoded - optional>,
    <need IDs, lowercase hex-encoded - optional>
]
```

The have and need ID fields are event IDs truncated to `idSize`, concatenated together. If there are `n` IDs in a field, then its length after hex encoding is `n * idSize * 2`. If one or both of these fields are omitted, they are assumed to be the empty string.

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

* The timestamp is encoded specially. The value `0` is reserved for the "infinity timestamp" (such that all valid events preceed it). All other values are encoded as `1 + offset`, where offset is the difference between this timestamp and the previously encoded timestamp. The initial offset starts at 0 and resets at the beginning of each message.

  Offsets are always non-negative since the upper bound's timestamp is always `>=` to the lower bound's timestamp, ranges in a message are always encoded in ascending order, and ranges never overlap.

* The `id` parameter's size is encoded in `length`, and can be between `0` and `idSize` bytes. Efficient implementations will use the shortest possible length to separate the first element of this range from the last element of the previous range. If these elements' timestamps differ, then the length will be 0, otherwise it will be the byte-length of their common prefix plus 1.

  If the `id` length is less than `idSize` then the unspecified trailing bytes are filled with 0 bytes.

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

How to split the range is implementation-defined. The simplest way is to divide the elements that fall within the range into N equal-sized buckets, and emit an XOR-mode sub-range for each bucket. However, an implementation could choose different grouping criteria. For example, events with similar timestamps could be grouped into a single bucket. If the implementation believes the other side is less likely to have recent events, it could make the most recent bucket a base-case-mode.

Note that if alternate grouping strategies are used, an implementation should never reply to a range with another single XOR-range, otherwise the protocol may never terminate (if the other side does the same).

## Initial Message

The initial message should have at least one range, and the first range's lower bound should have timestamp 0 and an empty (all zeros) `id`. The last range's upper bound should have the infinity timestamp (and the `id` doesn't matter, so should be empty also).

How many ranges are used depends on the implementation. The most obvious implementation is to use the same logic as described above, either using the base case or splitting, depending on set size. However, an implementation may choose to use fewer or more buckets in its initial message, and may use different grouping strategies.

### Termination

If a receiver has looped over all incoming ranges and has added no new ranges to the next outgoing message, the sets have been fully reconciled. It still must send a final message with the value of an empty string (`""`) to let the other side know it is finished, as well as to transmit any final have/need IDs.



## Analysis

If you are searching for a single element in an ordered array, binary search allows you to find the element with a logarithmic number of operations. This is because each operation cuts the search space in half. So, searching a list of 1 million items will take about 20 operations:

    log(1e6)/log(2) = 19.9316

For effective performance, this protocol requires minimising the number of "round-trips" between the two sides. A sync that takes 20 back-and-forth communications to determine a single difference would take unacceptably long. Fortunately we can expend a small amount of extra bandwidth by splitting our ranges in more than 2 ranges. This has the effect of increasing the base of the logarithm. For example, if we split it into 16 pieces instead:

    log(1e6)/log(16) = 4.98289

Additionally, each direction of the protocol's communication can result in a split, so since we are measuring round-trips, we divide this by two:

    log(1e6)/log(16)/2 = 2.49145

This means that in general, three round-trip communications will be required to synchronise two sets of 1 million elements that differ by 1 element. With an `idSize` of 16, each communication will take `16*16 + overhead` bytes -- roughly 300. So total bandwidth in one direction would be about 900 bytes and the other direction about 600 bytes.

What if they differ by multiple elements? Because communication is batched, the splitting of multiple differing ranges can happen in parallel. So, the number of round-trips will not be affected (assuming that every message can be delivered in exactly one packet transmission, independent of size, which is of course not entirely true on real networks).

The amount of bandwidth consumed will grow linearly with the number of differences, but this would of course be true even assuming a perfect synchronisation method that had no overhead other than transmitting the differing elements.



## Implementation Enhancements

### Deferred Range Processing

If there are too many differences and/or they are too randomly distributed throughout the range, then message sizes may become unmanageably large. This may be undesirable because of the memory required for buffering, and also because limiting batch sizes allows work pipelining, where the synchronised elements can be processed while additional syncing is occurring.

Because of this, an implementation may choose to defer the processing of ranges. Rather than transmit all the ranges it has detected need syncing, it can transmit a smaller number and keep the remaining for subsequent message rounds. This will decrease the message size at the expense of increasing the number of messaging round-trips.

An implementation could target fixed size messages, or could dynamically tune the message sizes based on its throughput metrics.

### Unidirection Syncing

The protocol above describes bi-directional syncing. This means at at the end of a sync, each side knows exactly which elements it has that the other doesn't, and what elements the other has that it doesn't. Because it (obviously) knows what elements it has itself, this implies it knows the precise set of the other side's elements too.

In many cases this is not necessary. For example, in most cases a nostr client will be the one coordinating the sync. After it has determined which elements it has or needs, it will either download or upload the elements according to how it is configured.

FIXME: expand on this. need protocol addition?

## Internal Representation

While a general-purpose relay implementation must be able to run a query on its DB and store a temporary data-structure of matching events for as long as the `XOR-OPEN` subscription is active, some optimisations are possible in certain cases.

In particular, a relay might like to support full-DB syncing. This is where another relay is configured to replicate another relay, and so it should ensure it always has exactly the same data-set. For this, a match-all filter of `{}` would be used.

In this case, or in the case of filters that contain only `until` and/or `since`, a relay may choose to maintain a persistent data-structure that reflects all stored events. This data-structure can be stored as a pre-computed tree, meaning that getting the XOR of the full data-set or a range of the data-set could be done very efficiently.

A production version of this should implement the tree as a copy-on-write data-structure so that inexpensive historical snapshots can be taken and stored alongside the `XOR-OPEN` subscription. Otherwise, any writes to the DB would corrupt the currently running protocol.
