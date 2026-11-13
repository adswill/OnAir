// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

const hex = (s) => {
  if (typeof s !== "string" || s.length % 2 || /[^0-9a-f]/i.test(s)) return null;
  const out = new Uint8Array(s.length / 2);
  for (let i = 0; i < out.length; i++) out[i] = parseInt(s.substr(i * 2, 2), 16);
  return out;
};

/** Verify Discord's Ed25519 request signature. Returns false on any problem. */
export async function verifySignature(publicKeyHex, signatureHex, timestamp, body) {
  try {
    const key = hex(publicKeyHex), sig = hex(signatureHex);
    if (!key || !sig || !timestamp) return false;
    const pub = await crypto.subtle.importKey("raw", key, { name: "Ed25519" }, false, ["verify"]);
    const msg = new TextEncoder().encode(timestamp + body);
    return await crypto.subtle.verify({ name: "Ed25519" }, pub, sig, msg);
  } catch {
    return false;
  }
}
