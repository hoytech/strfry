# Negentropy Syncing

This document describes a nostr protocol extension for syncing nostr events. It works for both client-relay and relay-relay scenarios. If both sides of the sync have events in common, then this protocol will use less bandwidth than transferring the full set of events (or even just their IDs).

It is a nostr-friendly wrapper around the [Negentropy](https://github.com/hoytech/negentropy) protocol.

## High-Level Protcol Description

We're going to call the two sides engaged in the sync the client and the relay (even though the initiator could be another relay instead of a client).

* (1) Client (initiator) chooses a nostr filter, and retrieves the set of events that it has locally that match this filter.
  * Client creates a `Negentropy` object, adds all events to it, seals it, and then calls `initiate()` to create the initial message.
* (2) Client sends a `NEG-OPEN` message to the relay, which includes the filter and the initial message.
* (3) Relay selects the set of events that it has locally that match the filter
  * Relay creates a `Negentropy` object, adds all events to it, and seals it.
* (4) Relay calls `reconcile()` on its `Negentropy` object, and returns the results as a `NEG-MSG` answer to the client.
* (5) Client calls `reconcile()` on its `Negentropy` object using the value sent by the relay.
  * If the empty string is returned, the sync is complete.
  * This call will return `have` and `need` arrays, which correspond to nostr IDs that should be uploaded and downloaded, respectively.
  * Otherwise, the result is sent back to the relay in another `NEG-MSG`. Goto step 4.

## Nostr Messages

### Initial message (client to relay):

```json
[
    "NEG-OPEN",
    <subscription ID string>,
    <nostr filter or event ID>,
    <initialMessage, lowercase hex-encoded>
]
```

* The subscription ID is used by each side to identify which query a message refers to. It only needs to be long enough to distinguish it from any other concurrent NEG requests on this websocket connection (an integer that increments once per `NEG-OPEN` is fine). If a `NEG-OPEN` is issued for a currently open subscription ID, the existing subscription is first closed.
* The nostr filter is as described in [NIP-01](https://github.com/nostr-protocol/nips/blob/master/01.md), or is an event ID whose `content` contains the JSON-encoded filter/array of filters.
* `initialMessage` is the string returned by `initiate()`, hex-encoded.

### Error message (relay to client):

If a request cannot be serviced by the relay, an error is returned to the client:

```json
[
    "NEG-ERR",
    <subscription ID string>,
    <reason code string>
]
```

Current reason codes are:

* `RESULTS_TOO_BIG`
  * Relays can optionally reject queries that would require them to process too many records, or records that are too old
  * The maximum number of records that can be processed can optionally be returned as the 4th element in the response
* `CLOSED`
  * Because the `NEG-OPEN` queries are stateful, relays may choose to time-out inactive queries to recover memory resources
* `FILTER_NOT_FOUND`
  * If an event ID is used as the filter, this error will be returned if the relay does not have this event. The client should retry with the full filter, or upload the event to the relay.
* `FILTER_INVALID`
  * The event's `content` was not valid JSON, or the filter was invalid for some other reason.

After a `NEG-ERR` is issued, the subscription is considered to be closed.

### Subsequent messages (bidirectional):

Relay and client alternate sending each other `NEG-MSG`s:

```json
[
    "NEG-MSG",
    <subscription ID string>,
    <message, lowercase hex-encoded>
]
```

### Close message (client to relay):

When finished, the client should tell the relay it can release its resources with a `NEG-CLOSE`:

```json
[
    "NEG-CLOSE",
    <subscription ID string>
]
```
