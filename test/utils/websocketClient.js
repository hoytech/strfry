import { setTimeout as delay } from "node:timers/promises";
import WebSocket from 'ws';

export async function openWebSocket(url, timeoutMs = 4_000) {
  return await new Promise((resolve, reject) => {
    const ws = new WebSocket(url);
    const timer = setTimeout(() => {
      ws.close();
      reject(new Error("websocket open timeout"));
    }, timeoutMs);

    ws.addEventListener("open", () => {
      clearTimeout(timer);
      resolve(ws);
    });

    ws.addEventListener("error", () => {
      clearTimeout(timer);
      reject(new Error("websocket connection failed"));
    });
  });
}

export class WsClient {
  constructor(ws) {
    this.ws = ws;
    this.queue = [];
    this.waiters = [];

    this.ws.addEventListener("message", (ev) => {
      const msg = JSON.parse(String(ev.data));
      if (this.waiters.length > 0) {
        const waiter = this.waiters.shift();
        clearTimeout(waiter.timer);
        waiter.resolve(msg);
      } else {
        this.queue.push(msg);
      }
    });
  }

  send(msg) {
    this.ws.send(JSON.stringify(msg));
  }

  async nextMessage(timeoutMs = 3_000) {
    if (this.queue.length > 0) return this.queue.shift();

    return await new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        this.waiters = this.waiters.filter((w) => w.resolve !== resolve);
        reject(new Error("message timeout"));
      }, timeoutMs);

      this.waiters.push({ resolve, reject, timer });
    });
  }

  async waitFor(predicate, timeoutMs = 3_000) {
    const deadline = Date.now() + timeoutMs;
    while (Date.now() < deadline) {
      const remaining = Math.max(1, deadline - Date.now());
      const msg = await this.nextMessage(remaining);
      if (predicate(msg)) return msg;
    }
    throw new Error("waitFor timeout");
  }

  async collectUntil(predicate, timeoutMs = 3_000) {
    const out = [];
    while (true) {
      const msg = await this.nextMessage(timeoutMs);
      out.push(msg);
      if (predicate(msg)) return out;
    }
  }

  async close() {
    if (this.ws.readyState === WebSocket.CLOSED) return;
    await new Promise((resolve) => {
      this.ws.addEventListener("close", () => resolve(), { once: true });
      this.ws.close();
    });
  }
}

export async function waitForRelay(wsUrl) {
  const deadline = Date.now() + 12_000;
  let lastErr = "";

  while (Date.now() < deadline) {
    try {
      const ws = await openWebSocket(wsUrl, 500);
      ws.close();
      return;
    } catch (e) {
      lastErr = String(e.message || e);
      await delay(100);
    }
  }

  throw new Error(`relay did not start in time: ${lastErr}`);
}
