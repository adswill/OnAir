// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Makes the marketplace signing key: an Ed25519 seed (the SIGNING_KEY secret) and its public key (kBuiltInMarketKeyHex in
// core/src/pstore/market.cpp). Prints both; writes nothing. Run it on your own computer and paste the seed straight into
// `wrangler secret put SIGNING_KEY`; keep a copy offline (a password manager is fine).
import { generateKeyPairSync } from "node:crypto";
const { publicKey, privateKey } = generateKeyPairSync("ed25519");
const hex = (b64u) => Buffer.from(b64u, "base64url").toString("hex");
console.log("SIGNING_KEY (secret seed, 64 hex digits):\n  " + hex(privateKey.export({ format: "jwk" }).d));
console.log("public key (kBuiltInMarketKeyHex):\n  " + hex(publicKey.export({ format: "jwk" }).x));
