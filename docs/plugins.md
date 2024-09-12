# Event-sifter plugins

In order to reduce complexity, strfry's design attempts to keep policy logic out of its core relay functionality. Instead, this logic can be implemented by operators by installing policy plugins to decide which events to store. Among other things, plugins can be used for the following:

* White/black-lists (particular pubkeys can/can't post events)
* Rate-limits
* Spam filtering

A plugin can be implemented in any programming language that supports reading lines from stdin, decoding JSON, and printing JSON to stdout. If a plugin is installed, strfry will send the event (along with some other information like IP address) to the plugin over stdin. The plugin should then decide what to do with it and print out a JSON object containing this decision.

Currently strfry always waits until it receives a response from a plugin before sending another request. In the future, multiple requests may be sent concurrently, which is why output messages must include the event ID.

The plugin command can be any shell command, which lets you set environment variables, command-line switches, etc. If the plugin command contains no spaces, it is assumed to be a path to a script. In this case, whenever the script's modification-time changes, the plugin will be reloaded upon the next write attempt.

If the plugin's command in `strfry.conf` (or a router config file) change, then the plugin will also be reloaded.


## Input messages

Input messages contain the following keys:

* `type`: Currently always `new`
* `event`: The event posted by the client, with all the required fields such as `id`, `pubkey`, etc
* `receivedAt`: Unix timestamp of when this event was received by the relay
* `sourceType`: The channel where this event came from: `IP4`, `IP6`, `Import`, `Stream`, `Sync`, or `Stored`.
* `sourceInfo`: Specifics of the event's source. Usually an IP address.


## Output messages

In response to `new` events, the plugin should print a JSONL message (minified JSON followed by a newline). It should contain the following keys:

* `id`: The event ID taken from the `event.id` field of the input message
* `action`: Either `accept`, `reject`, or `shadowReject`
* `msg`: The NIP-20 response message to be sent to the client. Only used for `reject`


## Example: Whitelist

Here is a simple example `whitelist.js` plugin that will reject all events except for those in a whitelist:

    #!/usr/bin/env node

    const whiteList = {
        '003ba9b2c5bd8afeed41a4ce362a8b7fc3ab59c25b6a1359cae9093f296dac01': true,
    };

    const rl = require('readline').createInterface({
      input: process.stdin,
      output: process.stdout,
      terminal: false
    });

    rl.on('line', (line) => {
        let req = JSON.parse(line);

        if (req.type !== 'new') {
            console.error("unexpected request type"); // will appear in strfry logs
            return;
        }

        let res = { id: req.event.id }; // must echo the event's id

        if (whiteList[req.event.pubkey]) {
            res.action = 'accept';
        } else {
            res.action = 'reject';
            res.msg = 'blocked: not on white-list';
        }

        console.log(JSON.stringify(res));
    });

To install:

* Make the script executable: `chmod a+x whitelist.js`
* In `strfry.conf`, configure `relay.writePolicy.plugin` to `./whitelist.js`


## Notes

* If applicable, you should ensure stdout is *line buffered*
  * In perl use `$|++`
  * In python, write with `print(response, flush=True)`
* If events are being rejected with `error: internal error`, then check the strfry logs. The plugin is misconfigured or failing.
* Normally when a plugin blocks an event, it will log a message. Especially when using plugins in `stream`, `router`, etc, this might be too verbose. In order to silence these logs, return an empty string for `msg` (or no `msg` at all).
* When returning an action of `accept`, it doesn't necessarily guarantee that the event will be accepted. The regular strfry checks are still subsequently applied, such as expiration, deletion, etc.
