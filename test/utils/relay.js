import { spawn, spawnSync } from "node:child_process";
import { mkdirSync, rmSync, writeFileSync } from "node:fs";
import { setTimeout as delay } from "node:timers/promises";
import path from "node:path";
import { buildEvent } from "./events.js";
import { waitForRelay, openWebSocket, WsClient } from "./websocketClient.js";
import ids from "./dummyIds.json" with { type: "json" };

let ts = 1_700_000_000;

export function runStrfry(args, opts = {}) {
  const res = spawnSync("./strfry", args, {
    encoding: "utf-8",
    ...opts,
  });
  if (res.error) throw res.error;
  return res;
}

export function cleanDb(dir) {
  rmSync(dir, { recursive: true, force: true });
  mkdirSync(dir, { recursive: true });
}

export function writeConfig(config, configPath) {
  mkdirSync(path.dirname(configPath), { recursive: true });
  writeFileSync(configPath, config.trim() + "\n", "utf-8");
}

export function addEvent(configPath, evInput) {
  const event = buildEvent({
    sec: ids[evInput.from ?? 0].sec,
    pub: ids[evInput.from ?? 0].pub,
    content: evInput.content ?? "",
    kind: evInput.kind ?? 1,
    tags: evInput.tags ?? [],
    created_at: evInput.created_at ?? ts++,
  });

  const res = runStrfry(["--config", configPath, "import", "--no-verify"], {
    input: JSON.stringify(event) + "\n",
    stdio: ["pipe", "ignore", "pipe"],
  });

  if (res.status !== 0) {
    throw new Error(`import failed: ${res.stderr}`);
  }

  if (process.env.DUMP_EVENTS) {
    console.log(event);
  }

  return event;
}

export async function runRelaySuite({
  relayConfigPath,
  relayPort,
  relayDbPath,
  tests,
}) {
  const wsUrl = `ws://127.0.0.1:${relayPort}`;

  cleanDb(relayDbPath);
  addEvent(relayConfigPath, { kind: 1, from: 0, content: "seed-public" });
  addEvent(relayConfigPath, {
    kind: 4,
    from: 0,
    tags: [["p", ids[1].pub]],
    content: "seed-restricted",
  });

  const relayProc = spawn("./strfry", ["--config", relayConfigPath, "relay"], {
    stdio: ["ignore", "pipe", "pipe"],
  });

  let relayLogs = "";
  relayProc.stdout.on("data", (d) => {
    relayLogs += d.toString();
  });
  relayProc.stderr.on("data", (d) => {
    relayLogs += d.toString();
  });

  let client;
  try {
    await waitForRelay(wsUrl);
    const ws = await openWebSocket(wsUrl);
    client = new WsClient(ws);
    await tests({ wsUrl, client });
  } catch (e) {
    throw new Error(
      `${String(e && e.message ? e.message : e)}\n\nRelay logs:\n${relayLogs}`,
    );
  } finally {
    if (client) await client.close();
    relayProc.kill("SIGTERM");
    await Promise.race([
      new Promise((resolve) => relayProc.once("exit", resolve)),
      delay(2_000),
    ]);
    if (!relayProc.killed) relayProc.kill("SIGKILL");
  }
}
