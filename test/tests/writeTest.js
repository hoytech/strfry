import { spawnSync } from "node:child_process";
import { mkdirSync, rmSync } from "node:fs";
import os from "node:os";
import path from "node:path";
import { buildEvent } from "../utils/events.js";
import { cleanDb, addEvent, runStrfry, writeConfig, config } from "../utils/relay.js";
import ids from "../utils/dummyIds.json" with { type: "json" };

const workDir = path.join(os.tmpdir(), "strfry-tests");
const dbDir = path.join(workDir, "relay-db");
const cfgPath = path.join(workDir, "writeTest.conf");

function doTest(spec) {
  console.log("*", spec.desc || "unnamed");

  cleanDb(dbDir);
  writeConfig(config(dbDir), cfgPath);

  const eventIds = [];

  for (const ev of spec.events) {
    const replaceEV = (v) => {
      if (typeof v === "string") {
        return v.replace(/EV_(\d+)/g, (_, i) => eventIds[Number(i)]);
      }
      if (Array.isArray(v)) return v.map(replaceEV);
      if (v && typeof v === "object") {
        for (const k in v) v[k] = replaceEV(v[k]);
      }
      return v;
    };

    replaceEV(ev);

    const id = addEvent(cfgPath, ev).id;
    eventIds.push(id);
  }

  if (spec.assertIds) {
    for (let i = 0; i < spec.assertIds.length; i++) {
      if (!eventIds[i].startsWith(spec.assertIds[i])) {
        throw new Error("assertId incorrect");
      }
    }
  }

  const result = runStrfry(["--config", cfgPath, "export"]);

  if (result.error) throw result.error;

  const finalEventIds = result.stdout
    .trim()
    .split("\n")
    .filter(Boolean)
    .map((line) => JSON.parse(line).id);

  if (spec.verify.length !== finalEventIds.length) {
    throw new Error("incorrect eventIds lengths");
  }

  for (let i = 0; i < finalEventIds.length; i++) {
    if (eventIds[spec.verify[i]] !== finalEventIds[i]) {
      throw new Error("id mismatch");
    }
  }
}

const d = (v) => ["d", v];
const e = (v) => ["e", v];
const a = (v) => ["a", v];
const p = (v) => ["p", v];

doTest({
  desc: "Basic insert",
  events: [
    { content: "hi", kind: 1 },
    { from: 1, content: "hi 2", kind: 1 },
  ],
  verify: [0, 1],
});

doTest({
  desc: "Replacement, newer timestamp",
  events: [
    {
      content: "hi",
      kind: 10000,
      created_at: 5000,
    },
    {
      content: "hi 2",
      kind: 10000,
      created_at: 5001,
    },
    {
      content: "hi",
      kind: 10000,
      created_at: 5000,
    },
  ],
  verify: [1],
});

doTest({
  desc: "Replacement is dropped",
  events: [
    {
      content: "hi",
      kind: 10000,
      created_at: 5001,
    },
    {
      content: "hi 2",
      kind: 10000,
      created_at: 5000,
    },
  ],
  verify: [0],
});

doTest({
  desc: "Doesn't replace someone else's event",
  events: [
    {
      content: "hi",
      kind: 10000,
      created_at: 5000,
    },
    {
      from: 1,
      content: "hi 2",
      kind: 10000,
      created_at: 5001,
    },
  ],
  verify: [0, 1],
});

doTest({
  desc: "Doesn't replace different kind",
  events: [
    {
      content: "hi",
      kind: 10001,
      created_at: 5000,
    },
    {
      from: 1,
      content: "hi 2",
      kind: 10000,
      created_at: 5001,
    },
  ],
  verify: [0, 1],
});

doTest({
  desc: "d tags ignored in 10k-20k",
  events: [
    {
      content: "hi",
      kind: 10003,
      created_at: 5000,
    },
    {
      content: "hi 2",
      kind: 10003,
      created_at: 5001,
      tags: [d("myrepl")],
    },
  ],
  verify: [1],
});

doTest({
  desc: "Equal timestamps no replace (id >)",
  events: [
    {
      content: "c1",
      kind: 10000,
      created_at: 5000,
    },
    {
      content: "c2",
      kind: 10000,
      created_at: 5000,
    },
  ],
  assertIds: ["7c", "ae"],
  verify: [0],
});

doTest({
  desc: "Equal timestamps replace (id <)",
  events: [
    {
      content: "c1",
      kind: 10000,
      created_at: 5000,
    },
    {
      content: "c4",
      kind: 10000,
      created_at: 5000,
    },
  ],
  assertIds: ["7c", "63"],
  verify: [1],
});

doTest({
  desc: "Deletion",
  events: [
    {
      content: "hi",
      kind: 1,
      created_at: 5000,
    },
    {
      content: "hi",
      kind: 1,
      created_at: 5001,
    },
    {
      content: "hi",
      kind: 1,
      created_at: 5002,
    },
    {
      content: "blah",
      kind: 5,
      created_at: 6000,
      tags: [e("EV_2"), e("EV_0")],
    },
  ],
  verify: [1, 3],
});

doTest({
  desc: "Deletion duplicate",
  events: [
    {
      content: "hi",
      kind: 1,
      created_at: 5000,
    },
    {
      content: "hi",
      kind: 1,
      created_at: 5001,
    },
    {
      content: "hi",
      kind: 1,
      created_at: 5002,
    },
    {
      content: "blah",
      kind: 5,
      created_at: 6000,
      tags: [e("EV_2"), e("EV_2")],
    },
  ],
  verify: [0, 1, 3],
});

doTest({
  desc: "Can't delete someone else's",
  events: [
    {
      content: "hi",
      kind: 1,
      created_at: 5000,
    },
    {
      from: 1,
      content: "blah",
      kind: 5,
      created_at: 6000,
      tags: [e("EV_0")],
    },
  ],
  verify: [0, 1],
});

doTest({
  desc: "Deletion prevents re-add",
  events: [
    {
      content: "hi",
      kind: 1,
      created_at: 5000,
    },
    {
      content: "blah",
      kind: 5,
      created_at: 6000,
      tags: [e("EV_0")],
    },
    {
      content: "hi",
      kind: 1,
      created_at: 5000,
    },
  ],
  verify: [1],
});

doTest({
  desc: "Delete a deletion",
  events: [
    {
      content: "hi",
      kind: 1,
      created_at: 5000,
    },
    {
      content: "blah",
      kind: 5,
      created_at: 6000,
      tags: [e("EV_0")],
    },
    {
      content: "blah",
      kind: 5,
      created_at: 7000,
      tags: [e("EV_1")],
    },
    {
      content: "hi",
      kind: 1,
      created_at: 5000,
    },
  ],
  verify: [3, 2],
});

// ---- A-TAG TESTS ----

const aTag = (kind, pub, d) => `${kind}:${pub}:${d}`;

doTest({
  desc: "Deletion by a-tag blocks same",
  events: [
    {
      content: "hi",
      kind: 30000,
      created_at: 5000,
      tags: [d("hello")],
    },
    {
      content: "blah",
      kind: 5,
      created_at: 6000,
      tags: [a(aTag(30000, ids[0].pub, "hello"))],
    },
    {
      content: "hi",
      kind: 30000,
      created_at: 5000,
      tags: [d("hello")],
    },
  ],
  verify: [1],
});

doTest({
  desc: "Parameterized replaceable",
  events: [
    {
      content: "hi1",
      kind: 30001,
      created_at: 5000,
      tags: [d("myrepl")],
    },
    {
      content: "hi2",
      kind: 30001,
      created_at: 5001,
      tags: [d("myrepl")],
    },
  ],
  verify: [1],
});

doTest({
  desc: "d tag only works 30k-40k",
  events: [
    {
      content: "hi1",
      kind: 1,
      created_at: 5000,
      tags: [d("myrepl")],
    },
    {
      content: "hi2",
      kind: 1,
      created_at: 5001,
      tags: [d("myrepl")],
    },
  ],
  verify: [0, 1],
});

doTest({
  desc: "d tags must match",
  events: [
    {
      content: "hi1",
      kind: 30001,
      created_at: 5000,
      tags: [d("myrepl")],
    },
    {
      content: "hi2",
      kind: 30001,
      created_at: 5001,
      tags: [d("myrepl2")],
    },
    {
      content: "hi3",
      kind: 30001,
      created_at: 5002,
      tags: [d("myrepl")],
    },
  ],
  verify: [1, 2],
});

doTest({
  desc: "Kinds must match",
  events: [
    {
      content: "hi1",
      kind: 30001,
      created_at: 5000,
      tags: [d("myrepl")],
    },
    {
      content: "hi2",
      kind: 30002,
      created_at: 5001,
      tags: [d("myrepl")],
    },
  ],
  verify: [0, 1],
});

doTest({
  desc: "Pubkeys must match",
  events: [
    {
      content: "hi1",
      kind: 30001,
      created_at: 5000,
      tags: [d("myrepl")],
    },
    {
      from: 1,
      content: "hi2",
      kind: 30001,
      created_at: 5001,
      tags: [d("myrepl")],
    },
  ],
  verify: [0, 1],
});

doTest({
  desc: "Newer param replaceable not replaced",
  events: [
    {
      content: "hi1",
      kind: 30001,
      created_at: 5001,
      tags: [d("myrepl")],
    },
    {
      content: "hi2",
      kind: 30001,
      created_at: 5000,
      tags: [d("myrepl")],
    },
  ],
  verify: [0],
});

doTest({
  desc: "Explicit empty d tag",
  events: [
    {
      content: "hi",
      kind: 30003,
      created_at: 5000,
    },
    {
      content: "hi 2",
      kind: 30003,
      created_at: 5001,
      tags: [d("")],
    },
    {
      content: "hi",
      kind: 30003,
      created_at: 5000,
    },
  ],
  verify: [1],
});

// ---- NIP-59 GIFT WRAP TESTS ----

doTest({
  desc: "Gift wrap recipient can delete",
  events: [
    {
      content: "wrapped",
      kind: 1059,
      created_at: 5000,
      tags: [p(ids[1].pub)],
    },
    {
      from: 1,
      content: "",
      kind: 5,
      created_at: 6000,
      tags: [e("EV_0")],
    },
  ],
  verify: [1],
});

doTest({
  desc: "Gift wrap deletion prevents re-add",
  events: [
    {
      content: "wrapped",
      kind: 1059,
      created_at: 5000,
      tags: [p(ids[1].pub)],
    },
    {
      from: 1,
      content: "",
      kind: 5,
      created_at: 6000,
      tags: [e("EV_0")],
    },
    {
      content: "wrapped",
      kind: 1059,
      created_at: 5000,
      tags: [p(ids[1].pub)],
    },
  ],
  verify: [1],
});

doTest({
  desc: "Non-recipient can't delete gift wrap",
  events: [
    {
      content: "wrapped",
      kind: 1059,
      created_at: 5000,
      tags: [p(ids[1].pub)],
    },
    {
      from: 2,
      content: "",
      kind: 5,
      created_at: 6000,
      tags: [e("EV_0")],
    },
  ],
  verify: [0, 1],
});

doTest({
  desc: "Ephemeral gift wrap (21059) recipient can delete",
  events: [
    {
      content: "wrapped",
      kind: 21059,
      created_at: 5000,
      tags: [p(ids[1].pub)],
    },
    {
      from: 1,
      content: "",
      kind: 5,
      created_at: 6000,
      tags: [e("EV_0")],
    },
  ],
  verify: [1],
});

console.log("All OK");
