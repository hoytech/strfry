import os from "node:os";
import path from "node:path";
import { writeFileSync, rmSync } from "node:fs";
import { setTimeout as delay } from "node:timers/promises";
import { openWebSocket, WsClient } from "../utils/websocketClient.js";
import {
  writeConfig,
  cleanDb,
  runStrfry,
  runRelaySuite,
  config,
} from "../utils/relay.js";
import ids from "../utils/dummyIds.json" with { type: "json" };
import { signEvent } from "../utils/events.js";

const workDir = path.join(os.tmpdir(), "strfry-tests");
const relayDbDir = path.join(workDir, "allowlist-relay-db");
const syncDbDir = path.join(workDir, "allowlist-sync-db");
const relayCfgPath = path.join(workDir, "readAllowlistRelay.conf");
const syncCfgPath = path.join(workDir, "readAllowlistSync.conf");
const allowlistPath = path.join(workDir, "read-allowlist.txt");
const relayPort = 40556;

const pass = (msg) => console.log(`Pass: ${msg}`);

function expect(cond, msg) {
  if (!cond) throw new Error(msg);
}

function allowlistConfig(dbDir, port) {
  return config(dbDir, port).replace(
    /restrictReadToInvolvedPubkey = \w+/,
    (m) => `${m}\n    readAllowlistFile = "${allowlistPath}"`,
  );
}

async function authAs(client, challenge, idx) {
  client.send(["AUTH", signEvent(challenge, "wss://relay.test", ids[idx].sec)]);
  const ok = await client.waitFor((m) => m[0] === "OK", 10_000);
  expect(ok[2] === true, `AUTH as ids[${idx}] should succeed`);
}

async function testUnauthedReqAndCountGetAuthRequired({ client }) {
  client.send(["REQ", "public-req", { kinds: [1] }]);
  const auth = await client.waitFor((m) => m[0] === "AUTH");
  const closed = await client.waitFor(
    (m) => m[0] === "CLOSED" && m[1] === "public-req",
  );
  expect(
    String(closed[2]).includes("auth-required"),
    "unauthenticated REQ for a public kind must be closed with auth-required",
  );

  client.send(["COUNT", "public-count", { kinds: [1] }]);
  const closedCount = await client.waitFor(
    (m) => m[0] === "CLOSED" && m[1] === "public-count",
  );
  expect(
    String(closedCount[2]).includes("auth-required"),
    "unauthenticated COUNT must be closed with auth-required",
  );
  pass("testUnauthedReqAndCountGetAuthRequired");
  return auth[1];
}

async function testUnlistedPubkeyIsRestricted({ client }, challenge) {
  await authAs(client, challenge, 0);
  client.send(["REQ", "unlisted-req", { kinds: [1] }]);
  const closed = await client.waitFor(
    (m) => m[0] === "CLOSED" && m[1] === "unlisted-req",
  );
  expect(
    String(closed[2]).includes("restricted:"),
    "authenticated but unlisted pubkey must get restricted:",
  );
  pass("testUnlistedPubkeyIsRestricted");
}

async function testListedPubkeyCanRead({ client }) {
  writeFileSync(allowlistPath, `# comment\n\n${ids[0].pub}\nnot-a-pubkey\n`);
  await delay(1_200); // the file is checked at most once per second

  client.send(["REQ", "listed-req", { kinds: [1] }]);
  const msgs = await client.collectUntil(
    (m) => m[0] === "EOSE" && m[1] === "listed-req",
    4_000,
  );
  const events = msgs.filter((m) => m[0] === "EVENT");
  expect(events.length === 1, "listed pubkey should see the seeded kind 1 event");
  client.send(["CLOSE", "listed-req"]);
  pass("testListedPubkeyCanRead");
}

async function testRemovedPubkeyIsRestrictedAgain({ client }) {
  writeFileSync(allowlistPath, `${ids[1].pub}\n`);
  await delay(1_200);
  client.send(["REQ", "removed-req", { kinds: [1] }]);
  const closed = await client.waitFor(
    (m) => m[0] === "CLOSED" && m[1] === "removed-req",
  );
  expect(
    String(closed[2]).includes("restricted:"),
    "pubkey removed from the file must be restricted again",
  );
  pass("testRemovedPubkeyIsRestrictedAgain");
}

function testNegentropyRequiresAuth({ wsUrl }) {
  cleanDb(syncDbDir);
  writeConfig(config(syncDbDir, 40557), syncCfgPath);
  const res = runStrfry(
    ["--config", syncCfgPath, "sync", wsUrl, "--filter", '{"kinds":[1]}', "--print-missing", "--timeout", "10"],
    { stdio: ["ignore", "pipe", "pipe"] },
  );
  const logs = `${res.stdout}\n${res.stderr}`;
  expect(
    res.status !== 0 && logs.includes("NEG-ERR") && logs.includes("auth-required"),
    "negentropy sync of a public kind must fail with auth-required",
  );
  pass("testNegentropyRequiresAuth");
}

async function testDisabledWhenEmpty() {
  // Same relay config without readAllowlistFile: public reads work unauthenticated.
  writeConfig(config(relayDbDir, relayPort), relayCfgPath);
  await delay(3_000); // config reload
  const ws = await openWebSocket(`ws://127.0.0.1:${relayPort}`);
  const client = new WsClient(ws);
  client.send(["REQ", "open-req", { kinds: [1] }]);
  const msgs = await client.collectUntil(
    (m) => m[0] === "EOSE" && m[1] === "open-req",
    4_000,
  );
  expect(
    msgs.some((m) => m[0] === "EVENT"),
    "with readAllowlistFile empty, unauthenticated reads must work as before",
  );
  await client.close();
  pass("testDisabledWhenEmpty");
}

async function main() {
  console.log("* read allowlist (relay.auth.readAllowlistFile) tests");

  rmSync(allowlistPath, { force: true });
  writeConfig(allowlistConfig(relayDbDir, relayPort), relayCfgPath);

  await runRelaySuite({
    relayConfigPath: relayCfgPath,
    relayPort,
    relayDbPath: relayDbDir,
    tests: async ({ wsUrl, client }) => {
      const challenge = await testUnauthedReqAndCountGetAuthRequired({ client });
      await testUnlistedPubkeyIsRestricted({ client }, challenge);
      await testListedPubkeyCanRead({ client });
      await testRemovedPubkeyIsRestrictedAgain({ client });
      testNegentropyRequiresAuth({ wsUrl });
      await testDisabledWhenEmpty();
    },
  });
  console.log("All read allowlist tests passed!");
}

main().catch((err) => {
  console.error(err && err.stack ? err.stack : String(err));
  process.exit(1);
});
