import { createHash } from "node:crypto";

export function sha256(message) {
  return createHash("sha256").update(message).digest("hex");
}

export function bytesToHex(bytes) {
  return Buffer.from(bytes).toString("hex");
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
