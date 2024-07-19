# strfry - a nostr relay

![strfry logo](docs/strfry.svg)

strfry is a relay for the [nostr protocol](https://github.com/nostr-protocol/nostr)

* Supports most applicable NIPs: 1, 2, 4, 9, 11, 12, 15, 16, 20, 22, 28, 33, 40
* No external database required: All data is stored locally on the filesystem in LMDB
* Hot reloading of config file: No server restart needed for many config param changes
* Zero downtime restarts, for upgrading binary without impacting users
* Websocket compression: permessage-deflate with optional sliding window, when supported by clients
* Built-in support for real-time streaming (up/down/both) events from remote relays, and bulk import/export of events from/to jsonl files
* [negentropy](https://github.com/hoytech/negentropy)-based set reconcilliation for efficient syncing with remote relays

If you are using strfry, please [join our telegram chat](https://t.me/strfry_users). Hopefully soon we'll migrate this to nostr.


## Syncing

The most original feature of strfry is a set reconcillation protocol based on [negentropy](https://github.com/hoytech/negentropy). This is implemented over a [nostr protocol extension](https://github.com/hoytech/strfry/blob/master/docs/negentropy.md) that allows two parties to synchronise their sets of stored messages with minimal bandwidth overhead. Although primarily designed for relay-to-relay communication, this can also be used by clients.

Either the full set of messages in the DB can be synced, or the results of one or more nostr filter expressions. If the two parties to the sync share common subsets of identical events, then there will be significant bandwidth savings compared to downloading the full set.



## Usage

### Compile

A C++20 compiler is required, along with a few other common dependencies. On Debian/Ubuntu use these commands:

#### Linux

    sudo apt install -y git g++ make libssl-dev zlib1g-dev liblmdb-dev libflatbuffers-dev libsecp256k1-dev libzstd-dev
    git clone https://github.com/hoytech/strfry && cd strfry/
    git submodule update --init
    make setup-golpe
    make -j4

#### FreeBSD

    pkg install -y gcc gmake cmake git perl5 openssl lmdb flatbuffers libuv libinotify zstr secp256k1 zlib-ng
    git clone https://github.com/hoytech/strfry && cd strfry/
    git submodule update --init
    gmake setup-golpe
    gmake -j4


### Running a relay

Here is how to run the relay:

    ./strfry relay

For dev/testing, the config file `./strfry.conf` is used by default. It stores data in the `./strfry-db/` directory.

In production, you'll probably want a systemd unit file and a reverse proxy such as nginx (details coming soon).

### Importing data

The `strfry import` command reads line-delimited JSON (jsonl) from its standard input and imports events that validate into the DB in batches of 10,000 at a time:

    cat my-nostr-dump.jsonl | ./strfry import

* By default, it will verify the signatures and other fields of the events. If you know the messages are valid, you can speed up the import a bit by passing the `--no-verify` flag.

### Exporting data

The `strfry export` command will print events from the DB to standard output in jsonl, ordered by their `created_at` field (ascending).

Optionally, you can limit the time period exported with the `--since` and `--until` flags.


### DB Upgrade

In the past, incompatible changes have been made to the DB format. If you try to use a `strfry` binary with an incompatible DB version, an error will be thrown. Only the `strfry export` command will work.

In order to upgrade the DB, you should export and then import again:

    ./strfry export > dbdump.jsonl
    mv strfry-db/data.mdb data.mdb.bak
    ./strfry import < dbdump.jsonl

After you have confirmed everything is working OK, the `dbdump.jsonl` and `data.mdb.bak` files can be deleted.


### Zero Downtime Restarts

strfry can have multiple different running instances simultaneously listening on the same port, because it uses the `REUSE_PORT` linux socket option. One of the reasons you may want to do this is to restart the relay without impacting currently connected users. This allows you to upgrade the strfry binary, or perform major configuration changes (for the subset of config options that require a restart).

If you send a `SIGUSR1` signal to a strfry process, it will initiate a "graceful shutdown". This means that it will no longer accept new websocket connections, and after its last existing websocket connection is closed, it will exit.

So, the typical flow for a zero downtime restart is:

* Record the PID of the currently running strfry instance.

* Start a new relay process using the same configuration as the currently running instance:

      strfry relay

  At this point, both instances will be accepting new connections.

* Initiate the graceful shutdown:

      kill -USR1 $OLD_PID

  Now only the new strfry instance will be accepting connections. The old one will exit once all its connections have been closed.


### Stream

This command opens a websocket connection to the specified relay and makes a nostr `REQ` request with filter `{"limit":0}`:

    ./strfry stream wss://relay.example.com

All events that are streamed back are inserted into the DB (after validation, checking for duplicates, etc). If the connection is closed for any reason, the command will try reconnecting every 5 seconds.

You can also run it in the opposite direction, which monitors your local DB for any new events and posts them to the specified relay:

    ./strfry stream wss://relay.example.com --dir up

Both of these operations can be concurrently multiplexed over the same websocket:

    ./strfry stream wss://relay.example.com --dir both

`strfry stream` will compress messages with permessage-deflate in both directions, if supported by the server. Sliding window compression is not supported for now.


### Sync

This command uses the negentropy protocol and performs a set reconcilliation between the local DB and the specified relay's remote DB.

Effectively what this does is figure out which events the remote relay has that you don't, and vice versa. Assuming that you both have common subsets of events, it does this more efficiently than simply transferring the full set of events (or even just their ids).

You can read about the algorithm used on the [negentropy project page](https://github.com/hoytech/negentropy). There are both C++ and Javascript reference implementations.

Here is how to perform a "full DB" set reconcilliation against a remote server:

    ./strfry sync wss://relay.example.com

This will download all missing events from the remote relay and insert them into your DB. Similar to `stream`, you can also sync in the `up` or `both` directions:

    ./strfry sync wss://relay.example.com --dir both

`both` is especially efficient, because performing the set reconcilliation automatically determines the missing members on each side.

Instead of a "full DB" sync, you can also sync the result of a nostr filter (or multiple filters, use a JSON array of them):

    ./strfry sync wss://relay.example.com '{"authors":["003b"]}'

Warning: Syncing can consume a lot of memory and bandwidth if the DBs are highly divergent (for example if your local DB is empty and your filter matches many events).



## Architecture

strfry uses concepts from various proprietary systems I have worked on in the past but consists solely of independently-developed open source code.

The [golpe](https://github.com/hoytech/golpe) application framework is used for basic services such as command-line arg parsing, logging, config files, etc.

### Database

strfry is built on the embedded [LMDB](https://www.symas.com/lmdb) database (using the [lmdbxx](https://github.com/hoytech/lmdbxx/) C++ interface). This means that records are accessed directly from the page cache. The read data-path requires no locking/system calls and it scales optimally with additional cores.

Database records are serialised with [Flatbuffers](https://google.github.io/flatbuffers/) serialisation, which allows fast and zero-copy access to individual fields within the records. A [RasgueaDB](https://github.com/hoytech/rasgueadb) layer is used for maintaining indices and executing queries.

The query engine is quite a bit less flexible than a general-purpose SQL engine, however the types of queries that can be performed via the nostr protocol are fairly constrained, so we can ensure that almost all queries have good index support. All possible query plans are determined at compile-time, so there is no SQL generation/parsing overhead, or risk of SQL injection.

When an event is inserted, indexable data (id, pubkey, tags, kind, and created_at) is loaded into a flatbuffers object. Signatures and non-indexed tags are removed, along with recommended relay fields, etc, to keep the record size minimal (and therefore improve cache usage). The full event's raw JSON is stored separately. The raw JSON is re-serialised to remove any unauthenticated fields from the event.

Various indices are created based on the indexed fields. Almost all indices are "clustered" with the event's `created_at` timestamp, allowing efficient `since`/`until` scans. Many queries can be serviced by index-only scans, and don't need to load the flatbuffers object at all.

I've tried to build the query engine with efficiency and performance in mind, but it is possible a SQL engine could find better execution plans, perhaps depending on the query. I haven't done any benchmarking or profiling yet, so your mileage may vary.

One benefit of a custom query engine is that we have the flexibility to optimise it for real-time streaming use-cases more than we could a general-purpose DB. For example, a user on a slow connection should not unnecessarily tie up resources. Our query engine supports pausing a query and storing it (it takes up a few hundred to a few thousand bytes, depending on query complexity), and resuming it later when the client's socket buffer has drained. Additionally, we can pause long-running queries to satisfy new queries as quickly as possible. This is all done without any data-base thread pools. There *are* worker threads, but they only exist to take advantage of multiple CPUs, not to block on client I/O.


### Threads and Inboxes

strfry starts multiple OS threads that communicate with each-other via two channels:

* Non-copying message queues
* The LMDB database

This means that no in-memory data-structures are accessed concurrently. This is sometimes called "shared nothing" architecture.

Each individual thread has an "inbox". Typically a thread will block waiting for a batch of messages to arrive in its inbox, process them, queue up new messages in the inboxes of other threads, and repeat.

### Websocket

This thread is responsible for accepting new websocket connections, routing incoming requests to the Ingesters, and replying with responses.

The Websocket thread is a single thread that multiplexes IO to/from multiple connections using the most scalable OS-level interface available (for example, epoll on Linux). It uses [my fork of uWebSockets](https://github.com/hoytech/uWebSockets).

Since there is only one of these threads, it is critical for system latency that it perform as little CPU-intensive work as possible. No request parsing or JSON encoding/decoding is done on this thread, nor any DB operations.

The Websocket thread does however handle compression and TLS, if configured. In production it is recommended to terminate TLS before strfry, for example with nginx.

#### Compression

If supported by the client, compression can reduce bandwidth consumption and improve latency.

Compression can run in two modes, either "per-message" or "sliding-window". Per-message uses much less memory, but it cannot take advantage of cross-message redundancy. Sliding-window uses more memory for each client, but the compression is typically better since nostr messages often contain serial redundancy (subIds, repeated pubkeys and event IDs in subsequent messages, etc).

The CPU usage of compression is typically small enough to make it worth it. However, strfry also supports running multiple independent strfry instances on the same machine (using the same DB backing store). This can distribute the compression overhead over several threads, according to the kernel's `REUSE_PORT` policy.

### Ingester

These threads perform the CPU-intensive work of processing incoming messages:

* Decoding JSON
* Validating and hashing new events
* Verifying event signatures
* Compiling filters

A particular connection's requests are always routed to the same ingester.

### Writer

This thread is responsible for most DB writes:

* Adding new events to the DB
* Performing event deletion (NIP-09)
* Deleting replaceable events (NIP-16)

It is important there is only 1 writer thread, because LMDB has an exclusive-write lock, so multiple writers would imply contention. Additionally, when multiple events queue up, there is work that can be amortised across the batch. This serves as a natural counterbalance against high write volumes.

### ReqWorker

Incoming `REQ` messages have two stages. The first stage is retrieving "old" data that already existed in the DB at the time of the request.

Servicing this stage is the job of the ReqWorker thread pool. Like Ingester, messages are consistently delivered to a thread according to connection ID. This is important so that (for example) CLOSE messages are matched with corresponding REQs.

When this stage is complete the next stage (monitoring) begins. When a ReqWorker thread completes the first stage for a subscription, the subscription is then sent to a ReqMonitor thread. ReqWorker is also responsible for forwarding unsubscribe (`CLOSE`) and socket disconnection messages to ReqMonitor. This forwarding is necessary to avoid a race condition where a message closing a subscription would be delivered while that subscription is pending in the ReqMonitor thread's inbox.

#### Filters

In nostr, each `REQ` message from a subscriber can contain multiple filters. We call this collection a `FilterGroup`. If one or more of the filters in the group matches an event, that event should be sent to the subscriber.

A `FilterGroup` is a vector of `Filter` objects. When the Ingester receives a `REQ`, the JSON filter items are compiled into `Filter`s and the original JSON is discarded. Each filter item's specified fields are compiled into sorted lookup tables called filter sets.

In order to determine if an event matches against a `Filter`, first the `since` and `until` fields are checked. Then, each field of the event for which a filter item was specified is looked up in the corresponding lookup table. Specifically, the upper-bound index is determined using a binary search (for example `std::upper_bound`). This is the first element greater than the event's item. Then the preceeding table item is checked for either a prefix (`ids`/`authors`) or exact (everything else) match.

Since testing `Filter`s against events is performed so frequently, it is a performance-critical operation and some optimisations have been applied. For example, each filter item in the lookup table is represented by a 4 byte data structure, one of which is the first byte of the field and the rest are offset/size lookups into a single memory allocation containing the remaining bytes. Under typical scenarios, this will greatly reduce the amount of memory that needs to be loaded to process a filter. Filters with 16 or fewer items can often be rejected with the load of a single cache line. Because filters aren't scanned linearly, the number of items in a filter (ie amount of pubkeys) doesn't have a significant impact on processing resources.

#### DBScan

The DB querying engine used by ReqWorker is called `DBScan`. This engine is designed to take advantage of indices that have been added to the database. The indices have been selected so that no filters require full table scans (over the `created_at` index), except ones that only use `since`/`until` (or nothing).

Because events are stored in the same flatbuffers format in memory and "in the database" (there isn't really any difference with LMDB), compiled filters can be applied to either.

When a user's `REQ` is being processed for the initial "old" data, each `Filter` in its `FilterGroup` is analysed and the best index is determined according to a simple heuristic. For each filter item in the `Filter`, the index is scanned backwards starting at the upper-bound of that filter item. Because all indices are composite keyed with `created_at`, the scanner also jumps to the `until` time when possible. Each event is compared against the compiled `Filter` and, if it matches, sent to the Websocket thread to be sent to the subscriber. The scan completes when one of the following is true:

* The key no longer matches the filter item (exact or prefix, depending on field)
* The event's `created_at` is before the `since` filter field
* The filter's `limit` field of delivered events has been reached

Once this completes, a scan begins for the next item in the filter field. Note that a filter only ever uses one index. If a filter specifies both `ids` and `authors`, only the `ids` index will be scanned. The `authors` filters will be applied when the whole filter is matched prior to sending.

An important property of `DBScan` is that queries can be paused and resumed with minimal overhead. This allows us to ensure that long-running queries don't negatively affect the latency of short-running queries. When ReqWorker first receives a query, it creates a DBScan for it. The scan will be run with a "time budget" (for example 10 milliseconds). If this is exceeded, the query is put to the back of a queue and new queries are checked for. This means that new queries will always be processed before resuming any queries that have already run for 10ms.


### ReqMonitor

The second stage of a REQ request is comparing newly-added events against the REQ's filters. If they match, the event should be sent to the subscriber.

ReqMonitor is not directly notified when new events have been written. This is important because new events can be added in a variety of ways. For instance, the `strfry import` command, event syncing, and multiple independent strfry servers using the same DB (ie, `REUSE_PORT`).

Instead, ReqMonitor watches for file change events using the OS's inotify API. When the file has changed, it scans all the events that were added to the DB since the last time it ran.

Note that because of this design decision, ephemeral events work differently than in other relay implementations. They *are* stored to the DB, however they have a very short retention-policy lifetime and will be deleted after 5 minutes (by default).

#### ActiveMonitors

Even though filter scanning is quite fast, strfry further attempts to optimise the case where a large number of concurrent REQs need to be monitored for.

When ReqMonitor first receives a subscription, it first compares its filter group against all the events that have been written since the subscription's DBScan started (since those are omitted from DBScan).

After the subscription is all caught up to the current transaction's snapshot, the filter group is broken up into its individual filters, and then each filter has one field selected (because all fields in a query must have a match, it is sufficient to choose one). This field is broken up into its individual filter items (ie a list of `ids`) and these are added to a sorted data-structure called a monitor set.

Whenever a new event is processed, all of its fields are looked up in the various monitor sets, which provides a list of filters that should be fully processed to check for a match. If an event has no fields in common with a filter, a match will not be attempted for this filter.

For example, for each prefix in the `authors` field in a filter, an entry is added to the `allAuthors` monitor set. When a new event is subsequently detected, the `pubkey` is looked up in `allAuthors` according to a binary search. Then the data-structure is scanned until it stops seeing records that are prefix matches against the `pubkey`. All of these matching records are pointers to corresponding `Filter`s of the REQs that have subscribed to this author. The filters must then be processed to determine if the event satisfies the other parameters of each filter (`since`/`until`/etc).

After comparing the event against each filter detected via the inverted index, that filter is marked as "up-to-date" with this event's ID, whether the filter matched or not. This prevents needlessly re-comparing this filter against the same event in the future (in case one of the *other* index lookups matches it). If a filter *does* match, then the entire filter group is marked as up-to-date. This prevents sending the same event multiple times in case multiple filters in a filter group match, and also prevents needlessly comparing other filters in the group against an event that has already been sent.

After an event has been processed, all the matching connections and subscription IDs are sent to the Websocket thread along with a single copy of the event's JSON. This prevents intermediate memory bloat that would occur if a copy was created for each subscription.


### Negentropy

These threads implements the provider-side of the [negentropy syncing protocol](https://github.com/hoytech/negentropy).

When [NEG-OPEN](https://github.com/hoytech/strfry/blob/master/docs/negentropy.md) requests are received, these threads perform DB queries in the same way as [ReqWorker](#ReqWorker) threads do. However, instead of sending the results back to the client, the IDs of the matching events are kept in memory, so they can be queried with future `NEG-MSG` queries.



### Cron

This thread is responsible for periodic maintenance operations. Currently this consists of applying a retention-policy and deleting ephemeral events.


## Testing

How to run the tests is described in the `test/README.md` file.

### Fuzz tests

The query engine is the most complicated part of the relay, so there is a differential fuzzing test framework to exercise it.

To bootstrap the tests, we load in a set of [real-world nostr events](https://wiki.wellorder.net/wiki/nostr-datasets/).

There is a simple but inefficient filter implementation in `test/dumbFilter.pl` that can be used to check if an event matches a filter. In a loop, we randomly generate a complicated filter group and pipe the entire DB's worth of events through the dumb filter and record which events it matched. Next, we perform the query using strfry's query engine (using a `strfry scan`) and ensure it matches. This gives us confidence that querying for "old" records in the DB will be performed correctly.

Next, we need to verify that monitoring for "new" records will function also. For this, in a loop we create a set of hundreds of random filters and install them in the monitoring engine. One of which is selected as a sample. The entire DB's worth of events is "posted to the relay" (actually just iterated over in the DB using `strfry monitor`), and we record which events were matched. This is then compared against a full-DB scan using the same query.

Both of these tests have run for several hours with no observed failures.



## Author and Copyright

strfry © 2023 Doug Hoyte.

GPLv3 license. See the LICENSE file.
