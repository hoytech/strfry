# strfry router

Although strfry is primarily a relay implementation, it can also function as a limited nostr client for the purpose of replicating events to or from other relays. While this can be done with `strfry stream`, this document describes a more flexible mechanism: `strfry router`.

With router, a single process can open up multiple different nostr connections. These connections can stream events to or from (or both) remote relays. All events can be streamed, or a subset of events can be selected by nostr filters and/or [plugins](plugins.md).

## Config file

The `strfry router` command takes one argument: A path to a config file (in [taocpp::config format](https://github.com/taocpp/config/blob/main/doc/Writing-Config-Files.md)):

    $ strfry router strfry-router.config

When the router starts, it will read the config file. If there are any parse errors it will fail immediately. Otherwise, it will connect to all specified relays and begin streaming. If any relay cannot be connected to, the router will wait 5 or 10 seconds and attempt to re-connect, forever.

If the config file is modified, then the router will load and parse this new file. If there are any errors, a message will be logged and it will continue with the old configuration. On success, the router will determine the minimally invasive modifications required to reconcile its current state with the newly specified configuration. For example, if a new relay is added, it will not interrupt live connections to any other relays, but simply open a new connection.

The config file must have a section `streams`. Within that, you may have as many stream sections as desired, with whatever names you choose. Inside each stream section, various parameters can be specified. Only `dir` and `urls` are required.

### Example

    connectionTimeout = 20

    streams {
        ## Stream down events from our friend relays

        friends {
            dir = "down"
            pluginDown = "/home/user/spam-filter.js"

            urls = [
                "wss://nos.lol"
                "wss://relayable.org"
            ]
        }

        ## Bi-directional streaming within our cluster

        cluster {
            dir = "both"

            urls = [
                "wss://eu.example.com"
                "wss://na.example.com"
            ]
        }

        ## Backup profiles and contact lists to our directory node

        directory {
            dir = "up"
            filter = { "kinds": [0, 3] }

            urls = [
                "ws://internal-directory.example.com"
            ]
        }
    }

### Stream Section Fields

#### dir

This is short for "direction", and must be one of `up`, `down`, or `both`.

The `up` direction will monitor the router's DB for any new events, and upload them to the specified urls. The `down` direction will subscribe to events from the remote relays and store them in the router's DB. `both` does both of these simultaneously.

Changing the `dir` field in the config file will currently close and re-open the connections within this section. This may be fixed eventually.

With `both` it will currently echo back an event to a relay it has just downloaded it from (which will typically then reject it as a duplicate). This is inefficient and may be fixed eventually.

#### urls

This is an non-empty array of websocket URLs. The relay will connect to *all* of these and apply the same policies to each of these connections.

#### filter

This is a nostr filter that can be used to narrow the set of events streamed. By default it is `{}`, which matches all events.

When a filter is applied to a section with direction `down` or `both`, this filter will be used with the `REQ` subscription upon connecting to each relay (with an implicit `"limit":0` added to suppress the downloading of previously stored events).

When a filter is applied to a section with direction `up` or `both`, before uploading any events the router will check if the filter matches. Only if so will the events be uploaded.

Changing the `filter` field in the config file will currently close and re-open the connections within this section. This may be fixed eventually.

#### pluginDown/pluginUp

These fields are paths to [plugins](plugins.md), or shell commands to invoke plugins.

If `pluginDown` is configured, the plugin will be consulted before storing any events received by connections within this section.

Similarly, if `pluginUp` is configured, the plugin will be consulted before transmitting any events to the connections within this section.
