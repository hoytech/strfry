#!/usr/bin/env node

// A write policy plugin for tests: it refuses every event and says in the refusal
// which key strfry told it the connection authenticated as.

import readline from "node:readline";

const rl = readline.createInterface({ input: process.stdin, terminal: false });

rl.on("line", (line) => {
  const req = JSON.parse(line);
  console.log(JSON.stringify({
    id: req.event.id,
    action: "reject",
    msg: `blocked: authed=${req.authed ?? "none"}`,
  }));
});
