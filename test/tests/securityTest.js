import http from "node:http";
import os from "node:os";
import path from "node:path";
import WebSocket from "ws";
import { finalizeEvent } from "@nostr/tools";

import { WsClient, openWebSocket } from "../utils/websocketClient.js";
import { hexToBytes } from "../utils/events.js";
import {
  writeConfig,
  addEvent,
  cleanDb,
  runRelaySuite,
  config,
} from "../utils/relay.js";
import ids from "../utils/dummyIds.json" with { type: "json" };

const workDir = path.join(os.tmpdir(), "strfry-security-tests");

const pass = (msg) => console.log(`Pass: ${msg}`);

function expect(cond, msg) {
  if (!cond) throw new Error(msg);
}

function configWith(dbDir, port, extraRelay = "") {
  return config(dbDir, port).replace("relay {", `relay {\n${extraRelay}`);
}

function httpGet(port, requestPath, headers = {}) {
  return new Promise((resolve, reject) => {
    const req = http.get(
      { host: "127.0.0.1", port, path: requestPath, headers },
      (res) => {
        let body = "";
        res.on("data", (chunk) => (body += chunk));
        res.on("end", () => resolve({ status: res.statusCode, body }));
      },
    );
    req.setTimeout(5_000, () => req.destroy(new Error("http timeout")));
    req.on("error", reject);
  });
}

function openWithOptions(url, options) {
  return new Promise((resolve, reject) => {
    const ws = new WebSocket(url, options);
    ws.once("open", () => resolve(ws));
    ws.once("error", reject);
  });
}

function waitForClose(ws, timeoutMs = 5_000) {
  return new Promise((resolve, reject) => {
    const timer = setTimeout(() => reject(new Error("close timeout")), timeoutMs);
    ws.once("close", (code, reason) => {
      clearTimeout(timer);
      resolve({ code, reason: String(reason) });
    });
  });
}

function signedEvent({ kind = 1, content = "", tags = [], from = 0 }) {
  return finalizeEvent(
    {
      kind,
      created_at: Math.floor(Date.now() / 1000),
      tags,
      content,
    },
    hexToBytes(ids[from].sec),
  );
}

async function testNestedJsonDoesNotCrash({ client }) {
  client.ws.send("[".repeat(9_000) + "]".repeat(9_000));

  const notice = await client.waitFor(
    (m) => m[0] === "NOTICE" && String(m[1]).includes("nesting depth"),
    5_000,
  );
  expect(notice, "deeply nested JSON must be rejected with a NOTICE");

  client.send(["REQ", "alive-after-nest", { kinds: [1], limit: 1 }]);
  await client.waitFor(
    (m) => m[0] === "EOSE" && m[1] === "alive-after-nest",
    5_000,
  );

  pass("testNestedJsonDoesNotCrash");
}

async function testInvalidNpubNip11({ client }) {
  const res = await httpGet(40561, "/", {
    Accept: "application/nostr+json",
  });
  expect(res.status === 200, `NIP-11 must return 200, got ${res.status}`);

  const info = JSON.parse(res.body);
  expect(
    info.pubkey === "npub1invalid",
    "invalid npub must fall back to the raw configured string",
  );
  expect(
    info.self === "npub1invalid",
    "invalid self npub must fall back to the raw configured string",
  );

  client.send(["REQ", "alive-after-nip11", { kinds: [1], limit: 1 }]);
  await client.waitFor(
    (m) => m[0] === "EOSE" && m[1] === "alive-after-nip11",
    5_000,
  );

  pass("testInvalidNpubNip11");
}

async function testEmptyNegentropyFilter({ client }) {
  client.send(["NEG-OPEN", "empty-neg", { ids: [] }, ""]);

  const negErr = await client.waitFor(
    (m) => m[0] === "NEG-ERR" && m[1] === "empty-neg",
    5_000,
  );
  expect(
    String(negErr[2]).includes("empty filter"),
    "empty negentropy filter must return NEG-ERR",
  );

  client.send(["REQ", "alive-after-neg", { kinds: [1], limit: 1 }]);
  await client.waitFor(
    (m) => m[0] === "EOSE" && m[1] === "alive-after-neg",
    5_000,
  );

  pass("testEmptyNegentropyFilter");
}

async function testLongTags({ client, relayConfigPath }) {
  const longTag = "t".repeat(300);
  const longD1 = "d1" + "x".repeat(300);
  const longD2 = "d2" + "y".repeat(300);
  const longD3 = "d3" + "z".repeat(300);

  const longTagEvent = addEvent(relayConfigPath, {
    kind: 1,
    from: 0,
    content: "long-tag",
    tags: [["t", longTag]],
  });

  client.send(["REQ", "long-tag-query", { "#t": [longTag] }]);
  const tagMsgs = await client.collectUntil(
    (m) => m[0] === "EOSE" && m[1] === "long-tag-query",
    5_000,
  );
  const tagEvents = tagMsgs
    .filter((m) => m[0] === "EVENT" && m[1] === "long-tag-query")
    .map((m) => m[2]);
  expect(
    tagEvents.some((ev) => ev.id === longTagEvent.id),
    "event with a 300-byte tag must be queryable by that tag",
  );

  const d1Event = addEvent(relayConfigPath, {
    kind: 30_000,
    from: 0,
    content: "long-d-1",
    tags: [["d", longD1]],
  });
  const d2Event = addEvent(relayConfigPath, {
    kind: 30_000,
    from: 0,
    content: "long-d-2",
    tags: [["d", longD2]],
  });
  const d3Event = addEvent(relayConfigPath, {
    kind: 30_000,
    from: 0,
    content: "long-d-deleted",
    tags: [["d", longD3]],
  });

  client.send(["REQ", "long-d-query", { kinds: [30_000] }]);
  const dMsgs = await client.collectUntil(
    (m) => m[0] === "EOSE" && m[1] === "long-d-query",
    5_000,
  );
  const dIds = dMsgs
    .filter((m) => m[0] === "EVENT" && m[1] === "long-d-query")
    .map((m) => m[2].id);
  expect(
    dIds.includes(d1Event.id) && dIds.includes(d2Event.id),
    "two different long d-tags must not collide",
  );

  addEvent(relayConfigPath, {
    kind: 5,
    from: 0,
    content: "delete long-d-3",
    tags: [["a", `30000:${ids[0].pub}:${longD3}`]],
  });

  client.send(["REQ", "long-a-delete-query", { kinds: [30_000] }]);
  const afterDeleteMsgs = await client.collectUntil(
    (m) => m[0] === "EOSE" && m[1] === "long-a-delete-query",
    5_000,
  );
  const afterDeleteIds = afterDeleteMsgs
    .filter((m) => m[0] === "EVENT" && m[1] === "long-a-delete-query")
    .map((m) => m[2].id);
  expect(
    !afterDeleteIds.includes(d3Event.id),
    "a-tag deletion must work for long d-tags",
  );
  expect(
    afterDeleteIds.includes(d1Event.id) && afterDeleteIds.includes(d2Event.id),
    "unrelated long-d events must survive an a-tag deletion",
  );

  const protectedEvent = signedEvent({
    kind: 1,
    content: "long-protected-tag",
    tags: [["-", "p".repeat(300)]],
  });
  client.send(["EVENT", protectedEvent]);
  const ok = await client.waitFor(
    (m) => m[0] === "OK" && m[1] === protectedEvent.id,
    5_000,
  );
  expect(
    ok[2] === false && String(ok[3]).includes("protected"),
    "a long protected '-' tag must still mark the event as protected",
  );

  pass("testLongTags");
}

async function testUntilZero({ client }) {
  client.send(["REQ", "until-zero", { until: 0 }]);
  const msgs = await client.collectUntil(
    (m) => m[0] === "EOSE" && m[1] === "until-zero",
    5_000,
  );
  expect(
    !msgs.some((m) => m[0] === "EVENT"),
    "until:0 must not return events with created_at > 0",
  );

  pass("testUntilZero");
}

async function testCompressedPayloadLimit({ wsUrl }) {
  const ws = await openWithOptions(wsUrl, { perMessageDeflate: true });
  expect(
    String(ws.extensions || "").includes("permessage-deflate"),
    "compression must be negotiated for this test",
  );

  const client = new WsClient(ws);
  const closed = waitForClose(ws, 5_000);

  // A highly compressible message that decompresses well beyond the 4096-byte
  // configured payload limit. The compressed frame itself is tiny.
  ws.send('["REQ","compressed",{"kinds":[1]}]' + " ".repeat(20_000));

  const { code } = await closed;
  expect(
    code === 1009 || code === 1006,
    `oversized compressed message must close the connection, got ${code}`,
  );

  // The relay must still be alive after rejecting the oversized message.
  const alive = await openWebSocket(wsUrl);
  const aliveClient = new WsClient(alive);
  aliveClient.send(["REQ", "alive-after-compressed", { kinds: [1], limit: 1 }]);
  await aliveClient.waitFor(
    (m) => m[0] === "EOSE" && m[1] === "alive-after-compressed",
    5_000,
  );
  await aliveClient.close();

  pass("testCompressedPayloadLimit");
}

async function testInvalidRealIpHeader({ wsUrl }) {
  const ws = await openWithOptions(wsUrl, {
    headers: { "x-real-ip": "not-an-ip" },
  });
  const client = new WsClient(ws);

  client.send(["REQ", "alive-after-bad-ip", { kinds: [1], limit: 1 }]);
  await client.waitFor(
    (m) => m[0] === "EOSE" && m[1] === "alive-after-bad-ip",
    5_000,
  );
  await client.close();

  pass("testInvalidRealIpHeader");
}

async function testAuthGaugeAndMetricCardinality({ client, relayPort }) {
  // Open and close an unauthenticated connection. The AUTH gauge must not go
  // negative.
  const extra = await openWebSocket(`ws://127.0.0.1:${relayPort}`);
  await extra.close();

  let res = await httpGet(relayPort, "/metrics");
  const gaugeMatch = res.body.match(
    /strfry_authenticated_connections_current\s+(-?\d+)/,
  );
  expect(gaugeMatch, "metrics must include the authenticated connections gauge");
  expect(
    Number(gaugeMatch[1]) === 0,
    `authenticated connections gauge must stay at 0, got ${gaugeMatch[1]}`,
  );

  // Publish events with many distinct kinds. The labeled counter must cap
  // its label set instead of growing without bound.
  const baseKind = 50_000;
  const numKinds = 1_050;
  const sent = [];
  for (let i = 0; i < numKinds; i++) {
    const ev = signedEvent({
      kind: baseKind + i,
      content: `kind-${baseKind + i}`,
    });
    sent.push(ev);
    client.send(["EVENT", ev]);
  }

  const pendingIds = new Set(sent.map((ev) => ev.id));
  while (pendingIds.size > 0) {
    const msg = await client.nextMessage(15_000);
    if (msg[0] !== "OK" || !pendingIds.has(msg[1])) continue;

    expect(msg[2] === true, `event ${msg[1]} must be accepted`);
    pendingIds.delete(msg[1]);
  }

  res = await httpGet(relayPort, "/metrics");
  const kindLabels = [...res.body.matchAll(/nostr_events_total\{kind="[^"]*"\}/g)];
  expect(
    kindLabels.length <= 1_001,
    `event-kind metric labels must be capped, got ${kindLabels.length}`,
  );

  pass("testAuthGaugeAndMetricCardinality");
}

async function testCountRestrictedWithoutKinds({ client }) {
  client.send(["COUNT", "count-all", {}]);
  const auth = await client.waitFor((m) => m[0] === "AUTH", 5_000);
  expect(typeof auth[1] === "string" && auth[1].length > 0, "COUNT {} must request AUTH");

  const closed = await client.waitFor(
    (m) => m[0] === "CLOSED" && m[1] === "count-all",
    5_000,
  );
  expect(
    String(closed[2]).includes("auth-required"),
    "COUNT {} must require AUTH when restricted kinds are configured",
  );

  client.send(["COUNT", "count-mixed", { kinds: [1, 4] }]);
  const mixedClosed = await client.waitFor(
    (m) => m[0] === "CLOSED" && m[1] === "count-mixed",
    5_000,
  );
  expect(
    String(mixedClosed[2]).includes("auth-required"),
    "COUNT with a mixed kind list must require AUTH",
  );

  const authEvent = finalizeEvent(
    {
      kind: 22242,
      created_at: Math.floor(Date.now() / 1000),
      tags: [
        ["relay", "wss://relay.test"],
        ["challenge", auth[1]],
      ],
      content: "",
    },
    hexToBytes(ids[0].sec),
  );
  client.send(["AUTH", authEvent]);
  await client.waitFor((m) => m[0] === "OK" && m[2] === true, 5_000);

  client.send(["COUNT", "count-all-authed", {}]);
  const authedClosed = await client.waitFor(
    (m) => m[0] === "CLOSED" && m[1] === "count-all-authed",
    5_000,
  );
  expect(
    String(authedClosed[2]).includes("count-failed"),
    "COUNT {} must be scoped when restrictReadToInvolvedPubkey is enabled",
  );

  pass("testCountRestrictedWithoutKinds");
}

async function main() {
  console.log("* security regression tests");

  const nestCfg = path.join(workDir, "nest.conf");
  const nip11Cfg = path.join(workDir, "nip11.conf");
  const negCfg = path.join(workDir, "neg.conf");
  const tagsCfg = path.join(workDir, "tags.conf");
  const metricsCfg = path.join(workDir, "metrics.conf");
  const payloadCfg = path.join(workDir, "payload.conf");
  const realIpCfg = path.join(workDir, "realip.conf");
  const countCfg = path.join(workDir, "count.conf");

  writeConfig(config(path.join(workDir, "nest-db"), 40560), nestCfg);
  await runRelaySuite({
    relayConfigPath: nestCfg,
    relayPort: 40560,
    relayDbPath: path.join(workDir, "nest-db"),
    tests: async ({ client }) => {
      await testNestedJsonDoesNotCrash({ client });
    },
  });

  writeConfig(
    configWith(
      path.join(workDir, "nip11-db"),
      40561,
      '  info {\n    pubkey = "npub1invalid"\n    self = "npub1invalid"\n  }',
    ),
    nip11Cfg,
  );
  await runRelaySuite({
    relayConfigPath: nip11Cfg,
    relayPort: 40561,
    relayDbPath: path.join(workDir, "nip11-db"),
    tests: async ({ client }) => {
      await testInvalidNpubNip11({ client });
    },
  });

  writeConfig(config(path.join(workDir, "neg-db"), 40562), negCfg);
  await runRelaySuite({
    relayConfigPath: negCfg,
    relayPort: 40562,
    relayDbPath: path.join(workDir, "neg-db"),
    tests: async ({ client }) => {
      await testEmptyNegentropyFilter({ client });
    },
  });

  writeConfig(config(path.join(workDir, "tags-db"), 40563), tagsCfg);
  await runRelaySuite({
    relayConfigPath: tagsCfg,
    relayPort: 40563,
    relayDbPath: path.join(workDir, "tags-db"),
    tests: async ({ client }) => {
      await testLongTags({ client, relayConfigPath: tagsCfg });
      await testUntilZero({ client });
    },
  });

  writeConfig(config(path.join(workDir, "metrics-db"), 40564), metricsCfg);
  await runRelaySuite({
    relayConfigPath: metricsCfg,
    relayPort: 40564,
    relayDbPath: path.join(workDir, "metrics-db"),
    tests: async ({ client }) => {
      await testAuthGaugeAndMetricCardinality({ client, relayPort: 40564 });
    },
  });

  writeConfig(
    configWith(
      path.join(workDir, "payload-db"),
      40565,
      '  maxWebsocketPayloadSize = 4096',
    ),
    payloadCfg,
  );
  await runRelaySuite({
    relayConfigPath: payloadCfg,
    relayPort: 40565,
    relayDbPath: path.join(workDir, "payload-db"),
    tests: async ({ wsUrl }) => {
      await testCompressedPayloadLimit({ wsUrl });
    },
  });

  writeConfig(
    configWith(
      path.join(workDir, "realip-db"),
      40566,
      '  realIpHeader = "x-real-ip"',
    ),
    realIpCfg,
  );
  await runRelaySuite({
    relayConfigPath: realIpCfg,
    relayPort: 40566,
    relayDbPath: path.join(workDir, "realip-db"),
    tests: async ({ wsUrl }) => {
      await testInvalidRealIpHeader({ wsUrl });
    },
  });

  writeConfig(config(path.join(workDir, "count-db"), 40567), countCfg);
  await runRelaySuite({
    relayConfigPath: countCfg,
    relayPort: 40567,
    relayDbPath: path.join(workDir, "count-db"),
    tests: async ({ client }) => {
      await testCountRestrictedWithoutKinds({ client });
    },
  });

  console.log("All security regression tests passed!");
}

main().catch((err) => {
  console.error(err && err.stack ? err.stack : String(err));
  process.exit(1);
});
