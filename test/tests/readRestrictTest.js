import os from "node:os";
import path from "node:path";
import { openWebSocket, WsClient } from "../utils/websocketClient.js";
import {
  writeConfig,
  addEvent,
  cleanDb,
  runStrfry,
  runRelaySuite,
} from "../utils/relay.js";
import ids from "../utils/dummyIds.json" with { type: "json" };
import { signEvent } from "../utils/events.js";

const workDir = path.join(os.tmpdir(), "strfry-read-restrict-tests");
const relayDbDir = path.join(workDir, "relay-db");
const syncDbDir = path.join(workDir, "sync-db");
const relayCfgPath = path.join(workDir, "readRestrictRelay.conf");
const syncCfgPath = path.join(workDir, "readRestrictSync.conf");
const relayPort = 40553;

let authChallengeString = null;

const pass = (msg) => console.log(`Pass: ${msg}`);

// pre auth
async function testRestrictedReqAndCountRequireAuth({ wsUrl, client }) {
  client.send(["REQ", "restricted-req", { kinds: [4] }]);
  const authReq = await client.waitFor((m) => m[0] === "AUTH");
  expect(
    typeof authReq[1] === "string" && authReq[1].length > 0,
    "REQ must return AUTH challenge",
  );

  authChallengeString = authReq[1];

  const closedReq = await client.waitFor(
    (m) => m[0] === "CLOSED" && m[1] === "restricted-req",
  );
  expect(
    String(closedReq[2]).includes("auth-required"),
    "REQ must be closed with auth-required",
  );

  client.send(["COUNT", "restricted-count", { kinds: [4] }]);

  const closedCount = await client.waitFor(
    (m) => m[0] === "CLOSED" && m[1] === "restricted-count",
  );
  expect(
    String(closedCount[2]).includes("auth-required"),
    "COUNT must be closed with auth-required",
  );
  pass("testRestrictedReqAndCountRequireAuth");
}

async function testCountUnrestrictedAllowed({ wsUrl, client }) {
  client.send(["COUNT", "count-open", { kinds: [1] }]);
  const count = await client.waitFor(
    (m) => m[0] === "COUNT" && m[1] === "count-open",
    4_000,
  );
  expect(
    typeof count[2]?.count === "number",
    "COUNT on unrestricted kinds should return a count body",
  );
  pass("testCountUnrestrictedAllowed");
}

async function testReqWorkerFiltersRestrictedInitialScan({ wsUrl, client }) {
  client.send(["REQ", "mixed-initial", { kinds: [1, 4] }]);
  let msgs = await client.collectUntil(
    (m) => m[0] === "EOSE" && m[1] === "mixed-initial",
    4_000,
  );

  let events = msgs
    .filter((m) => m[0] === "EVENT" && m[1] === "mixed-initial")
    .map((m) => m[2]);
  let kinds = events.map((ev) => ev.kind);
  expect(
    kinds.length >= 1,
    "mixed initial REQ should return at least one event",
  );
  expect(
    kinds.every((k) => k === 1),
    "mixed initial REQ should not return restricted kind 4",
  );

  client.send(["REQ", "omitted-kinds", {}]);
  msgs = await client.collectUntil(
    (m) => m[0] === "EOSE" && m[1] === "omitted-kinds",
    4_000,
  );

  events = msgs
    .filter((m) => m[0] === "EVENT" && m[1] === "omitted-kinds")
    .map((m) => m[2]);
  kinds = events.map((ev) => ev.kind);
  expect(
    kinds.length >= 1,
    "REQ with omitted kinds should return at least one event",
  );
  expect(
    kinds.every((k) => k !== 4),
    "REQ with omitted kinds should not return restricted kind 4",
  );
  pass("testReqWorkerFiltersRestrictedInitialScan");
}

async function testReqMonitorFiltersRestrictedLiveEvents({ wsUrl, client }) {
  client.send(["REQ", "mixed-live", { kinds: [1, 4] }]);
  await client.waitFor((m) => m[0] === "EOSE" && m[1] === "mixed-live", 4_000);

  addEvent(relayCfgPath, {
    kind: 4,
    from: 0,
    tags: [["p", ids[1].pub]],
    content: "live-restricted",
  });
  addEvent(relayCfgPath, { kind: 1, from: 0, content: "live-public" });

  const firstLiveEvent = await client.waitFor(
    (m) =>
      m[0] === "EVENT" &&
      m[1] === "mixed-live" &&
      m[2].content === "live-public",
    5_000,
  );
  expect(
    firstLiveEvent[2].kind === 1,
    "live mixed REQ should deliver kind 1 event",
  );

  pass("testReqMonitorFiltersRestrictedLiveEvents");
}

function testNegentropyMixedFilterBlocksRestrictedWithoutAuth({ wsUrl }) {
  cleanDb(syncDbDir);
  writeConfig(`db = "${syncDbDir}/"\n`, syncCfgPath);

  const syncRes = runStrfry(
    [
      "--config",
      syncCfgPath,
      "sync",
      wsUrl,
      "--filter",
      '{"kinds":[1,4]}',
      "--print-missing",
      "--timeout",
      "10",
    ],
    { stdio: ["ignore", "pipe", "pipe"] },
  );

  if (syncRes.status !== 0) {
    throw new Error(
      `mixed negentropy sync failed: ${syncRes.stderr || syncRes.stdout}`,
    );
  }

  const needLines = syncRes.stdout
    .trim()
    .split("\n")
    .filter((line) => line.startsWith("need,"));
  expect(
    needLines.length === 2,
    "mixed negentropy sync should only expose non-restricted event ids",
  );
  pass("testNegentropyMixedFilterBlocksRestrictedWithoutAuth");
}

function testNegentropyRestrictedFilterRequiresAuth({ wsUrl }) {
  cleanDb(syncDbDir);
  writeConfig(`db = "${syncDbDir}/"\n`, syncCfgPath);

  const syncRes = runStrfry(
    [
      "--config",
      syncCfgPath,
      "sync",
      wsUrl,
      "--filter",
      '{"kinds":[4]}',
      "--print-missing",
      "--timeout",
      "10",
    ],
    { stdio: ["ignore", "pipe", "pipe"] },
  );

  expect(
    syncRes.status !== 0,
    "restricted negentropy sync should fail without auth",
  );
  const logs = `${syncRes.stdout}\n${syncRes.stderr}`;
  expect(
    logs.includes("NEG-ERR") && logs.includes("auth-required"),
    "restricted negentropy sync should fail with auth-required NEG-ERR",
  );
  pass("testNegentropyRestrictedFilterRequiresAuth");
}

// post auth

async function testRestrictedFilterReturnsAllIfAuthenticatedAndInvolvementNotRequired({
  wsUrl,
  client,
}) {
  // send AUTH message with challenge string
  const authEvent = signEvent(
    authChallengeString,
    "wss://relay.test",
    ids[0].sec,
  );
  client.send(["AUTH", authEvent]);

  const authOk = await client.waitFor(
    (m) => m[3] === "successfully authenticated",
    10000,
  );

  expect(
    authOk[0] === "OK",
    "AUTH should return 'successfully authenticated' with 'ok' status",
  );

  // send REQ with mixed filter
  client.send(["REQ", "restricted-auth", { kinds: [4] }]);
  const msgs = await client.collectUntil(
    (m) => m[0] === "EOSE" && m[1] === "restricted-auth",
    4_000,
  );

  const events = msgs
    .filter((m) => m[0] === "EVENT" && m[1] === "restricted-auth")
    .map((m) => m[2]);

  const kinds = events.map((ev) => ev.kind);
  expect(
    kinds.length >= 1,
    "restricted authenticated REQ should return at least one event",
  );
  expect(
    kinds.includes(4),
    "restricted authenticated REQ should return restricted kind 4 events",
  );
  pass(
    "testRestrictedFilterReturnsAllIfAuthenticatedAndInvolvementNotRequired",
  );
}

async function testRestrictedFilterCountAuthenticatedNotScoped({
  wsUrl,
  client,
}) {
  client.send(["COUNT", "count-restricted-unscoped", { kinds: [4] }]);
  const count = await client.waitFor(
    (m) => m[1] === "count-restricted-unscoped",
    4_000,
  );
  expect(
    count[0] === "COUNT",
    "COUNT must be successful when authenticated and restrictReadToInvolvedPubkey is false",
  );
  pass("testRestrictedFilterCountAuthenticatedNotScoped");
}

async function testOnlyReturnsMyOwnEventsIfInvolvedRequired({ wsUrl, client }) {
  client.send(["REQ", "only-my-own", {}]);
  const msgs = await client.collectUntil(
    (m) => m[0] === "EOSE" && m[1] === "only-my-own",
    4_000,
  );

  const events = msgs
    .filter((m) => m[0] === "EVENT" && m[1] === "only-my-own")
    .map((m) => m[2]);

  expect(
    events.every((ev) => ev.pubkey === ids[0].pub),
    "Must only return my own events when restrictReadToInvolvedPubkey is set",
  );

  pass("testOnlyReturnsMyOwnEventsIfInvolvedRequired");
}

async function testCountFailsWhenFilterNotFullyScopedIfInvolvedRequired({
  wsUrl,
  client,
}) {
  client.send(["COUNT", "failing-count", { kinds: [4] }]);

  const msg = await client.waitFor((m) => m[1] === "failing-count");

  expect(String(msg[2]).includes("count-failed"));

  pass("testCountFailsWhenFilterNotFullyScopedIfInvolvedRequired");
}

async function testCountSuccessfulWhenFilterFullyScoped({ wsUrl, client }) {
  client.send([
    "COUNT",
    "succeeding-count",
    { kinds: [4], authors: [ids[0].pub] },
  ]);

  const msg = await client.waitFor((m) => m[1] === "succeeding-count");

  expect(
    Number.isFinite(msg[2].count),
    "returned count should be a valid number",
  );

  pass("testCountSuccessFulWhenFilterFullyScoped");
}

function config(relayDb, relayPort, restrictReadToInvolvedPubkey) {
  return `
db = "${relayDb}/"

relay {
  bind = "127.0.0.1"
  port = ${relayPort}
  nofiles = 0
  autoPingSeconds = 0

  auth {
    enabled = true
    serviceUrl = "wss://relay.test"
    restrictedReadKinds = "4, 1059"
    restrictReadToInvolvedPubkey = ${restrictReadToInvolvedPubkey ? "true" : "false"}
  }

  numThreads {
    ingester = 1
    reqWorker = 1
    reqMonitor = 1
    negentropy = 1
  }

  negentropy {
    enabled = true
    maxSyncEvents = 100000
  }
}
`;
}

function expect(cond, msg) {
  if (!cond) throw new Error(msg);
}

async function main() {
  console.log("* read restriction relay integration tests");

  cleanDb(syncDbDir);
  writeConfig(`db = "${syncDbDir}/"\n`, syncCfgPath);

  await runRelaySuite({
    config: config(relayDbDir, relayPort, true),
    relayConfigPath: relayCfgPath,
    relayPort: relayPort,
    relayDbPath: relayDbDir,
    tests: async ({ wsUrl, client }) => {
      writeConfig(config(relayDbDir, relayPort, false), relayCfgPath);
      await testRestrictedReqAndCountRequireAuth({ wsUrl, client });
      await testCountUnrestrictedAllowed({ wsUrl, client });
      await testReqWorkerFiltersRestrictedInitialScan({ wsUrl, client });
      await testReqMonitorFiltersRestrictedLiveEvents({ wsUrl, client });
      testNegentropyMixedFilterBlocksRestrictedWithoutAuth({ wsUrl });
      testNegentropyRestrictedFilterRequiresAuth({ wsUrl });
      await testRestrictedFilterReturnsAllIfAuthenticatedAndInvolvementNotRequired(
        { wsUrl, client },
      );
      await testRestrictedFilterCountAuthenticatedNotScoped({ wsUrl, client });
      writeConfig(config(relayDbDir, relayPort, true), relayCfgPath);
      console.log("Writing new config, wait...");
      await new Promise((resolve) => setTimeout(resolve, 3000));
      await testOnlyReturnsMyOwnEventsIfInvolvedRequired({ wsUrl, client });
      await testCountFailsWhenFilterNotFullyScopedIfInvolvedRequired({
        wsUrl,
        client,
      });
      await testCountSuccessfulWhenFilterFullyScoped({ wsUrl, client });
    },
  });
  console.log("All read restriction tests passed!");
}

main().catch((err) => {
  console.error(err && err.stack ? err.stack : String(err));
  process.exit(1);
});
