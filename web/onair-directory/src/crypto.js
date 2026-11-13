// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Ed25519 verification with WebCrypto (the same call works in Workers and in node 20+).
import { fromHex } from "./logic.js";

export async function verifySig(pubHex, sigHex, text) {
  const pub = fromHex(pubHex, 32), sig = fromHex(sigHex, 64);
  if (!pub || !sig) return false;
  try {
    const key = await crypto.subtle.importKey("raw", pub, { name: "Ed25519" }, false, ["verify"]);
    return await crypto.subtle.verify({ name: "Ed25519" }, key, sig, new TextEncoder().encode(text));
  } catch { return false; }
}

export async function sha256Hex(text) {
  const d = await crypto.subtle.digest("SHA-256", new TextEncoder().encode(text));
  return Array.from(new Uint8Array(d), (b) => b.toString(16).padStart(2, "0")).join("");
}
