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

const workDir = path.join(os.tmpdir(), "strfry-read-restrict-tests");
const relayDbDir = path.join(workDir, "relay-db");
const syncDbDir = path.join(workDir, "sync-db");
const relayCfgPath = path.join(workDir, "readRestrictRelay.conf");
const syncCfgPath = path.join(workDir, "readRestrictSync.conf");
const relayPortRestricted = 40552;
const relayPortOpen = 40553;

async function testRestrictedReqAndCountRequireAuth(wsUrl) {
  const ws = await openWebSocket(wsUrl);
  const client = new WsClient(ws);

  client.send(["REQ", "restricted-req", { kinds: [4] }]);
  const authReq = await client.waitFor((m) => m[0] === "AUTH");
  expect(
    typeof authReq[1] === "string" && authReq[1].length > 0,
    "REQ must return AUTH challenge",
  );
  const closedReq = await client.waitFor(
    (m) => m[0] === "CLOSED" && m[1] === "restricted-req",
  );
  expect(
    String(closedReq[2]).includes(
      "auth-required: requested filter requires authentication",
    ),
    "REQ must be closed with auth-required",
  );

  client.send(["COUNT", "restricted-count", { kinds: [4] }]);
  const authCount = await client.waitFor((m) => m[0] === "AUTH");
  expect(
    typeof authCount[1] === "string" && authCount[1].length > 0,
    "COUNT must return AUTH challenge",
  );

  const closedCount = await client.waitFor(
    (m) => m[0] === "CLOSED" && m[1] === "restricted-count",
  );
  expect(
    String(closedCount[2]).includes(
      "auth-required: requested filter requires authentication",
    ),
    "COUNT must be closed with auth-required",
  );

  await client.close();
}

async function testCountUnrestrictedAllowed(wsUrl) {
  const ws = await openWebSocket(wsUrl);
  const client = new WsClient(ws);

  client.send(["COUNT", "count-open", { kinds: [1] }]);
  const count = await client.waitFor(
    (m) => m[0] === "COUNT" && m[1] === "count-open",
    4_000,
  );
  expect(
    typeof count[2]?.count === "number",
    "COUNT on unrestricted kinds should return a count body",
  );

  await client.close();
}

async function testReqWorkerFiltersRestrictedInitialScan(wsUrl) {
  const ws = await openWebSocket(wsUrl);
  const client = new WsClient(ws);

  client.send(["REQ", "mixed-initial", { kinds: [1, 4] }]);
  const msgs = await client.collectUntil(
    (m) => m[0] === "EOSE" && m[1] === "mixed-initial",
    4_000,
  );

  const events = msgs
    .filter((m) => m[0] === "EVENT" && m[1] === "mixed-initial")
    .map((m) => m[2]);
  const kinds = events.map((ev) => ev.kind);
  expect(
    kinds.length >= 1,
    "mixed initial REQ should return at least one event",
  );
  expect(
    kinds.every((k) => k === 1),
    "mixed initial REQ should not return restricted kind 4",
  );

  await client.close();
}

async function testReqMonitorFiltersRestrictedLiveEvents(wsUrl, configPath) {
  const ws = await openWebSocket(wsUrl);
  const client = new WsClient(ws);

  client.send(["REQ", "mixed-live", { kinds: [1, 4] }]);
  await client.waitFor((m) => m[0] === "EOSE" && m[1] === "mixed-live", 4_000);

  addEvent(configPath, {
    kind: 4,
    from: 0,
    tags: [["p", ids[1].pub]],
    content: "live-restricted",
  });
  addEvent(configPath, { kind: 1, from: 0, content: "live-public" });

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

  const extra = [];
  const until = Date.now() + 1_500;
  while (Date.now() < until) {
    try {
      extra.push(await client.nextMessage(200));
    } catch {}
  }

  const gotRestricted = extra.some(
    (m) => m[0] === "EVENT" && m[1] === "mixed-live" && m[2]?.kind === 4,
  );
  expect(
    !gotRestricted,
    "live mixed REQ should not deliver restricted kind 4 events",
  );

  await client.close();
}

async function testNegentropyMixedFilterBlocksRestrictedWithoutAuth(wsUrl) {
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
}

function testNegentropyRestrictedFilterRequiresAuth(wsUrl) {
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
    config: config(relayDbDir, relayPortRestricted, true),
    relayConfigPath: relayCfgPath,
    relayPort: relayPortRestricted,
    relayDbPath: relayDbDir,
    tests: async ({ wsUrl, relayConfigPath }) => {
      await testRestrictedReqAndCountRequireAuth(wsUrl);
      await testCountUnrestrictedAllowed(wsUrl);
      await testReqWorkerFiltersRestrictedInitialScan(wsUrl);
      await testReqMonitorFiltersRestrictedLiveEvents(wsUrl, relayConfigPath);
      testNegentropyMixedFilterBlocksRestrictedWithoutAuth(wsUrl);
      testNegentropyRestrictedFilterRequiresAuth(wsUrl);
    },
  });

  await runRelaySuite({
    config: config(relayDbDir, relayPortOpen, false),
    relayConfigPath: relayCfgPath,
    relayPort: relayPortOpen,
    relayDbPath: relayDbDir,
    tests: async ({ wsUrl }) => {},
  });

  console.log("All read restriction tests passed");
}

main().catch((err) => {
  console.error(err && err.stack ? err.stack : String(err));
  process.exit(1);
});
