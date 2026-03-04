#!/usr/bin/env node

// This is a write policy plug-in that can fail in various ways. It is useful for testing purposes
// during development, but obviously don't use in the real world.

const { execSync } = require('child_process');

function sleepSync(ms) {
    execSync(`sleep ${ms / 1000}`);
}

// JUST SLEEP AND DON'T READ ANYTHING
//sleepSync(1000000 * 1000);

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

    // TIMEOUT
    //sleepSync(5000);

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

    // MALFORMED JSON
    //return console.log("{");

    // MAKE REQUEST TOO BIG
    //res.tooBigJunk = 'A'.repeat(100000);

    // SURROUNDING JUNK LINES
    //return console.log("SOME JUNK\n" + JSON.stringify(res) + "\nMORE JUNK");

    console.log(JSON.stringify(res));
});
