import os from "node:os";
import path from "node:path";
import { finalizeEvent } from "@nostr/tools";
import { writeConfig, runRelaySuite, config } from "../utils/relay.js";
import ids from "../utils/dummyIds.json" with { type: "json" };
import { hexToBytes, signEvent } from "../utils/events.js";

const workDir = path.join(os.tmpdir(), "strfry-tests");
const relayDbDir = path.join(workDir, "plugin-authed-db");
const relayCfgPath = path.join(workDir, "pluginAuthed.conf");
const relayPort = 40556;
const plugin = path.resolve("test/plugins/echoAuthed.js");

let counter = 0;

const pass = (msg) => console.log(`Pass: ${msg}`);

function expect(cond, msg) {
  if (!cond) throw new Error(msg);
}

function relayConfig() {
  return config(relayDbDir, relayPort).replace(
    "numThreads {",
    `writePolicy {\n    plugin = "${plugin}"\n  }\n\n  numThreads {`,
  );
}

function eventFrom(who) {
  return finalizeEvent(
    {
      kind: 1,
      created_at: Math.floor(Date.now() / 1000),
      tags: [],
      content: `plugin authed test ${counter++}`,
    },
    hexToBytes(ids[who].sec),
  );
}

async function publish(client, event) {
  client.send(["EVENT", event]);
  return await client.collectUntil((m) => m[0] === "OK" && m[1] === event.id);
}

async function authenticate(client, challenge, who) {
  client.send(["AUTH", signEvent(challenge, "wss://relay.test", ids[who].sec)]);
  await client.waitFor((m) => m[3] === "successfully authenticated", 10000);
}

async function testNothingBeforeAuth({ client }) {
  const msgs = await publish(client, eventFrom(1));
  expect(
    msgs.at(-1)[3] === "blocked: authed=none",
    `plugin should get no key before AUTH: ${JSON.stringify(msgs)}`,
  );
  expect(
    !msgs.some((m) => m[0] === "AUTH"),
    "a write should not start the AUTH flow unless configured to",
  );
  pass("testNothingBeforeAuth");
}

async function testAuthedKeyForAnotherAuthorsEvent({ client }) {
  client.send(["REQ", "get-challenge", { kinds: [4] }]);
  const challenge = await client.waitFor((m) => m[0] === "AUTH");
  await client.waitFor((m) => m[0] === "CLOSED");
  await authenticate(client, challenge[1], 0);

  const msgs = await publish(client, eventFrom(1));
  expect(
    msgs.at(-1)[3] === `blocked: authed=${ids[0].pub}`,
    `plugin should get the authenticated key: ${JSON.stringify(msgs)}`,
  );
  pass("testAuthedKeyForAnotherAuthorsEvent");
}

async function main() {
  console.log("* write policy plugin authed key tests");
  writeConfig(relayConfig(), relayCfgPath);

  await runRelaySuite({
    relayConfigPath: relayCfgPath,
    relayPort,
    relayDbPath: relayDbDir,
    tests: async ({ client }) => {
      await testNothingBeforeAuth({ client });
      await testAuthedKeyForAnotherAuthorsEvent({ client });
    },
  });
  console.log("All plugin authed key tests passed!");
}

main().catch((err) => {
  console.error(err && err.stack ? err.stack : String(err));
  process.exit(1);
});
