import { createHash } from "node:crypto";
import { finalizeEvent } from "@nostr/tools";

export function sha256(message) {
  return createHash("sha256").update(message).digest("hex");
}

export function bytesToHex(bytes) {
  return Buffer.from(bytes).toString("hex");
}

export function hexToBytes(hex) {
  return Uint8Array.from(hex.match(/.{2}/g), (byte) => parseInt(byte, 16));
}

function serializeEvent(evt) {
  return JSON.stringify([
    0,
    evt.pubkey,
    evt.created_at,
    evt.kind,
    evt.tags,
    evt.content,
  ]);
}

function getEventId(evt) {
  const serialized = serializeEvent(evt);
  return sha256(serialized);
}

export function buildEvent({
  sec,
  pub,
  content = "",
  kind = 1,
  created_at = Math.floor(Date.now() / 1000),
  tags = [],
}) {
  const evt = {
    pubkey: pub,
    created_at,
    kind,
    tags,
    content,
  };

  const id = getEventId(evt);

  const sig = "0".repeat(128);

  return { ...evt, id, sig };
}

export function signEvent(authChallengeString, relayUrl, sec) {
  const secBytes = hexToBytes(sec);
  const authEvent = finalizeEvent(
    {
      kind: 22242,
      created_at: Math.floor(Date.now() / 1000),
      tags: [
        ["relay", relayUrl],
        ["challenge", authChallengeString],
      ],
      content: "",
    },
    secBytes,
  );

  return authEvent;
}
