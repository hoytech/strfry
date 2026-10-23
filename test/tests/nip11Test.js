import os from "node:os";
import { isDeepStrictEqual } from "node:util";
import path from "node:path";
import { setTimeout as delay } from "node:timers/promises";
import {
  writeConfig,
  runStrfry,
  runRelaySuite,
  config,
} from "../utils/relay.js";

const workDir = path.join(os.tmpdir(), "strfry-tests");
const relayDbDir = path.join(workDir, "nip11-relay-db");
const relayCfgPath = path.join(workDir, "nip11Relay.conf");
const badCfgPath = path.join(workDir, "nip11Bad.conf");
const relayPort = 40558;

const pass = (msg) => console.log(`Pass: ${msg}`);

function expect(cond, msg) {
  if (!cond) throw new Error(msg);
}

const fees = {
  admission: [{ amount: 1000000, unit: "msats" }],
  subscription: [{ amount: 5000000, unit: "msats", period: 2592000 }],
  publication: [{ kinds: [1], amount: 1000, unit: "msats" }],
};

// Adds `extra` lines to the relay.info block, plus optional top-level relay settings.
function nip11Config({ info = [], relay = [] } = {}) {
  return config(relayDbDir, relayPort)
    .replace(/relay \{\n/, `relay {\n  ${relay.join("\n  ")}\n  info {\n    ${info.join("\n    ")}\n  }\n`)
    .replace(/^/, `events {\n  maxNumTags = 1234\n  rejectEventsOlderThanSeconds = 86400\n  rejectEventsNewerThanSeconds = 60\n}\n`);
}

async function fetchNip11(wsUrl) {
  const res = await fetch(wsUrl.replace("ws://", "http://"), {
    headers: { Accept: "application/nostr+json" },
  });
  expect(res.status === 200, `NIP-11 request failed: ${res.status}`);
  return await res.json();
}

async function testDefaults({ wsUrl }) {
  const info = await fetchNip11(wsUrl);
  const l = info.limitation;
  expect(l.max_event_tags === 1234, "max_event_tags from events.maxNumTags");
  expect(l.created_at_lower_limit === 86400, "created_at_lower_limit from events.rejectEventsOlderThanSeconds");
  expect(l.created_at_upper_limit === 60, "created_at_upper_limit from events.rejectEventsNewerThanSeconds");
  expect(l.max_subid_length === 64, "max_subid_length is the NIP-01 subscription id limit");
  expect(l.default_limit === l.max_limit && l.max_limit === 500, "default_limit equals max_limit");
  expect(l.auth_required === false, "auth_required defaults to false");
  expect(l.payment_required === false, "payment_required defaults to false");
  expect(l.restricted_writes === false, "restricted_writes is false without a write policy plugin");
  expect(!("fees" in info) && !("payments_url" in info), "no fees/payments_url unless configured");
  expect(!("min_pow_difficulty" in l) && !("max_content_length" in l), "no limits that strfry does not enforce");
  pass("testDefaults");
}

async function testPluginSetsRestrictedWrites({ wsUrl }) {
  writeConfig(nip11Config({ relay: ['writePolicy { plugin = "./test/plugins/mybadplugin.js" }'] }), relayCfgPath);
  await delay(3_000); // config reload
  const info = await fetchNip11(wsUrl);
  expect(info.limitation.restricted_writes === true, "restricted_writes is true when relay.writePolicy.plugin is set");
  pass("testPluginSetsRestrictedWrites");
}

async function testPayToRelayConfig({ wsUrl }) {
  writeConfig(
    nip11Config({
      relay: ['writePolicy { plugin = "./test/plugins/mybadplugin.js" }'],
      info: [
        'paymentsUrl = "https://relay.test/pay"',
        `fees = ${JSON.stringify(JSON.stringify(fees))}`,
        'paymentRequired = "true"',
        'authRequired = "true"',
        'restrictedWrites = "false"',
      ],
    }),
    relayCfgPath,
  );
  await delay(3_000);
  const info = await fetchNip11(wsUrl);
  expect(info.payments_url === "https://relay.test/pay", "payments_url from relay.info.paymentsUrl");
  expect(isDeepStrictEqual(info.fees, fees), "fees from relay.info.fees");
  expect(info.limitation.payment_required === true, "payment_required override");
  expect(info.limitation.auth_required === true, "auth_required override");
  expect(info.limitation.restricted_writes === false, "restricted_writes override beats the plugin default");
  pass("testPayToRelayConfig");
}

async function testBadFeesOnReloadAreOmitted({ wsUrl }) {
  writeConfig(nip11Config({ info: ['fees = "{\\"tips\\": []}"'] }), relayCfgPath);
  await delay(3_000);
  const info = await fetchNip11(wsUrl);
  expect(!("fees" in info), "invalid fees after a reload are omitted, not served");
  pass("testBadFeesOnReloadAreOmitted");
}

function testBadFeesRejectedAtStartup() {
  for (const [bad, why] of [
    ['fees = "not json"', "Invalid relay.info.fees"],
    ['fees = "{\\"publication\\": [{\\"amount\\": -1, \\"unit\\": \\"msats\\"}]}"', "amount must be a non-negative integer"],
    ['paymentRequired = "yes"', "Invalid relay.info.paymentRequired"],
  ]) {
    writeConfig(nip11Config({ info: [bad] }).replace(`port = ${relayPort}`, `port = ${relayPort + 1}`), badCfgPath);
    const res = runStrfry(["--config", badCfgPath, "relay"], { timeout: 10_000 });
    const out = `${res.stdout}\n${res.stderr}`;
    expect(res.status !== 0 && res.signal === null, `relay should refuse to start with ${bad}`);
    expect(out.includes(why), `startup error should mention "${why}", got: ${out.slice(-300)}`);
  }
  pass("testBadFeesRejectedAtStartup");
}

async function main() {
  console.log("* NIP-11 relay information document tests");

  writeConfig(nip11Config(), relayCfgPath);

  await runRelaySuite({
    relayConfigPath: relayCfgPath,
    relayPort,
    relayDbPath: relayDbDir,
    tests: async ({ wsUrl }) => {
      await testDefaults({ wsUrl });
      await testPluginSetsRestrictedWrites({ wsUrl });
      await testPayToRelayConfig({ wsUrl });
      await testBadFeesOnReloadAreOmitted({ wsUrl });
    },
  });

  testBadFeesRejectedAtStartup();
  console.log("All NIP-11 tests passed!");
}

main().catch((err) => {
  console.error(err && err.stack ? err.stack : String(err));
  process.exit(1);
});
