// The .onairplugin check on upload. parseManifest is a line-by-line port of plugin::parseManifest (core/src/plugin/manifest.cpp), so a package
// the server takes is one OnAir's installer takes; the package checks follow Manager::installBytes and the listing policy of the 3.9
// registry (tools/marketplace/onair_registry.py): open licence, no patented codec builds, no encryption-cracking tools, no references to
// credential or system files without the "filesystem" permission. These are heuristics; nobody has reviewed an upload (it is listed as
// "unreviewed").
import { Zip, ZipError } from "./zip.js";

export const ABI_VERSION = 1;
export const MAX_MANIFEST_BYTES = 64 * 1024;
export const PERMISSIONS = ["network", "filesystem", "audio-out", "record", "spawn-process"] as const;
export const KINDS = ["mode", "action", "source", "panel-lite"] as const;
export const PLATFORMS = ["macos", "linux", "windows"] as const;

export interface ModeDecl { id: string; name: string; group: string }
export interface Manifest {
  id: string; name: string; version: string; author: string; license: string; homepage: string; description: string; kind: string;
  minOnAir: string; abi: number; permissions: string[];
  lib: { macos: string; linux: string; windows: string };
  modes: ModeDecl[];
}

export class ManifestError extends Error {}
const fail = (m: string): never => { throw new ManifestError(m); };

export function validId(id: string): boolean {
  if (!id || id.length > 64) return false;
  for (let i = 0; i < id.length; i++) {
    const c = id[i];
    const ok = (c >= "a" && c <= "z") || (c >= "0" && c <= "9") || (i > 0 && (c === "-" || c === "_" || c === "."));
    if (!ok) return false;
  }
  return !id.includes("..");
}
const validModeId = (id: string) => /^[a-z0-9_-]{1,32}$/.test(id);
export function validVersion(v: string): boolean {
  return !!v && v.length <= 32 && v[0] >= "0" && v[0] <= "9" && /^[0-9A-Za-z.+-]+$/.test(v);
}
const validFileName = (f: string) => !!f && f.length <= 100 && f[0] !== "." && !/[/\\:\x00-\x1f]/.test(f) && !f.includes("..");
function cleanText(s: string, maxLen: number): boolean {
  // the C++ side counts bytes
  if (new TextEncoder().encode(s).length > maxLen) return false;
  for (let i = 0; i < s.length; i++) { const c = s.charCodeAt(i); if (c < 32 && c !== 10 && c !== 9) return false; }
  return true;
}

// -1 / 0 / 1 like plugin::compareVersions: dotted numbers, a suffix after '-' or '+' ignored
export function compareVersions(a: string, b: string): number {
  const parts = (s: string) => {
    const core = s.split(/[-+]/)[0];
    return core.split(".").map((x) => { const m = /^\d+/.exec(x); return m ? Math.min(parseInt(m[0], 10), 1e8) : 0; });
  };
  const x = parts(a), y = parts(b);
  for (let i = 0; i < Math.max(x.length, y.length); i++) {
    const p = x[i] ?? 0, q = y[i] ?? 0;
    if (p !== q) return p < q ? -1 : 1;
  }
  return 0;
}

export function parseManifest(text: string): Manifest {
  if (new TextEncoder().encode(text).length > MAX_MANIFEST_BYTES) fail("manifest.json is larger than 64 KiB");
  let root: unknown;
  try { root = JSON.parse(text); } catch (e) { fail("manifest.json is not valid JSON (" + (e as Error).message.slice(0, 80) + ")"); }
  if (!root || typeof root !== "object" || Array.isArray(root)) fail("manifest.json must be a JSON object");
  const o = root as Record<string, unknown>;
  const text1 = (k: string, maxLen: number, required: boolean): string => {
    const v = o[k];
    if (v === undefined) { if (required) fail(`missing "${k}"`); return ""; }
    if (typeof v !== "string" || !cleanText(v, maxLen)) fail(`"${k}" must be text of at most ${maxLen} characters`);
    if (required && v === "") fail(`"${k}" is empty`);
    return v as string;
  };
  const m: Manifest = {
    id: text1("id", 64, true), name: text1("name", 80, true), version: text1("version", 32, true), author: text1("author", 120, true),
    license: text1("license", 80, true), homepage: text1("homepage", 200, false), description: text1("description", 2000, false),
    kind: text1("kind", 16, true), minOnAir: text1("minimum_onair", 32, false), abi: 0, permissions: [],
    lib: { macos: "", linux: "", windows: "" }, modes: [],
  };
  if (!validId(m.id)) fail('"id" must be 1-64 characters of a-z 0-9 - _ . (starting with a letter or digit)');
  if (!validVersion(m.version)) fail('"version" must look like 1.2.3');
  if (!(KINDS as readonly string[]).includes(m.kind)) fail('"kind" must be mode, action, source or panel-lite');
  if (m.homepage && !m.homepage.startsWith("https://") && !m.homepage.startsWith("http://")) fail('"homepage" must be an http(s) address');
  if (m.minOnAir && !validVersion(m.minOnAir)) fail('"minimum_onair" must look like 0.2.7');
  const abi = o["abi_version"];
  if (abi === undefined) fail('missing "abi_version"');
  if (typeof abi !== "number" || !isFinite(abi) || abi < 1 || abi > 1000000) fail('"abi_version" must be a number from 1 to 1000000');
  m.abi = Math.trunc(abi as number);
  if (m.abi !== ABI_VERSION) fail(`built for plugin ABI ${m.abi}, OnAir speaks ABI ${ABI_VERSION}`);
  const lib = o["library"];
  if (lib !== undefined) {
    if (typeof lib === "string") m.lib = { macos: lib, linux: lib, windows: lib };
    else if (lib && typeof lib === "object" && !Array.isArray(lib)) {
      const l = lib as Record<string, unknown>;
      const s = (k: string) => (typeof l[k] === "string" ? (l[k] as string) : "");
      m.lib = { macos: s("macos"), linux: s("linux"), windows: s("windows") };
    } else fail('"library" must be a file name or an object with macos, linux and windows names');
    for (const f of Object.values(m.lib)) if (f && !validFileName(f)) fail('"library" must be plain file names inside the plugin folder');
  }
  const perms = o["permissions"];
  if (perms !== undefined) {
    if (!Array.isArray(perms) || perms.length > 16) fail('"permissions" must be a list');
    for (const p of perms as unknown[]) {
      if (typeof p !== "string") fail('"permissions" must be a list of names');
      if (!(PERMISSIONS as readonly string[]).includes(p as string)) fail(`unknown permission "${(p as string).slice(0, 40)}" (network, filesystem, audio-out, record, spawn-process)`);
      if (!m.permissions.includes(p as string)) m.permissions.push(p as string);
    }
  }
  const modes = o["modes"];
  if (modes !== undefined) {
    if (!Array.isArray(modes) || modes.length > 16) fail('"modes" must be a list of at most 16 modes');
    for (const e of modes as unknown[]) {
      if (!e || typeof e !== "object" || Array.isArray(e)) fail("each mode must be an object");
      const x = e as Record<string, unknown>;
      const str = (k: string) => (typeof x[k] === "string" ? (x[k] as string) : "");
      const d: ModeDecl = { id: str("id"), name: str("name"), group: str("group") };
      if (!validModeId(d.id)) fail('a mode "id" must be 1-32 characters of a-z 0-9 - _');
      if (!d.name || !cleanText(d.name, 60) || !cleanText(d.group, 60)) fail(`mode "${d.id}" needs a "name" of at most 60 characters`);
      if (m.modes.some((q) => q.id === d.id)) fail(`mode id "${d.id}" is used twice`);
      for (const [k, lo, hi] of [["default_frequency_hz", 1, 1e12], ["bandwidth_hz", 1, 1e10]] as const) {
        const v = x[k];
        if (v !== undefined && (typeof v !== "number" || !isFinite(v) || v < lo || v > hi)) fail(`"${k}" must be a number from ${lo} to ${hi}`);
      }
      const sr = x["sample_rates"];
      if (sr !== undefined) {
        if (!Array.isArray(sr) || sr.length > 16) fail('"sample_rates" must be a short list');
        for (const r of sr as unknown[]) if (typeof r !== "number" || !isFinite(r) || r < 1000 || r > 1e9) fail('"sample_rates" must be numbers from 1000 to 1e9');
      }
      m.modes.push(d);
    }
  }
  if ((m.kind === "mode" || m.kind === "source") && m.modes.length === 0) fail('a plugin of this kind needs at least one entry in "modes"');
  const settings = o["settings"];
  if (settings !== undefined) {
    if (!Array.isArray(settings) || settings.length > 64) fail('"settings" must be a list of at most 64 controls');
    const keys = new Set<string>();
    for (const e of settings as unknown[]) {
      if (!e || typeof e !== "object" || Array.isArray(e)) fail("each setting must be an object");
      const x = e as Record<string, unknown>;
      const str = (k: string) => (typeof x[k] === "string" ? (x[k] as string) : "");
      const key = str("key"), label = str("label"), type = str("type");
      if (!validModeId(key)) fail('a setting "key" must be 1-32 characters of a-z 0-9 - _');
      if (keys.has(key)) fail(`setting key "${key}" is used twice`);
      keys.add(key);
      if (!label || !cleanText(label, 80)) fail(`setting "${key}" needs a "label"`);
      if (!["slider", "checkbox", "combo", "text"].includes(type)) fail(`setting "${key}": type must be slider, checkbox, combo or text`);
      let min = 0, max = 100;
      for (const k of ["min", "max", "step"]) {
        const v = x[k];
        if (v === undefined) continue;
        const lo = k === "step" ? 0 : -1e12;
        if (typeof v !== "number" || !isFinite(v) || v < lo || v > 1e12) fail(`"${k}" must be a number from ${lo} to 1000000000000`);
        if (k === "min") min = v as number;
        if (k === "max") max = v as number;
      }
      if (type === "slider" && !(min < max)) fail(`setting "${key}": min must be below max`);
      const df = x["default"];
      if (typeof df === "string" && !cleanText(df, 500)) fail("setting default too long");
      if (type === "combo") {
        const op = x["options"];
        if (!Array.isArray(op) || op.length === 0 || op.length > 64) fail(`setting "${key}": a combo needs 1-64 "options"`);
        for (const v of op as unknown[]) if (typeof v !== "string" || !cleanText(v, 80)) fail(`setting "${key}": options are short texts`);
      }
    }
  }
  return m;
}

// ---- listing policy (the same heuristics as onair_registry.py lint)
const NOT_FREE = /proprietary|all rights reserved|-NC|-ND|non-?commercial|no[- ]?derivatives|commercial use/i;
export const CODEC_RE = /\b(ambe(\+2)?|imbe|mbelib|dvsi|acelp|g\.?729|evs codec|amr-?wb|hvxc|tetra[- ]?codec|vocoder build)\b/i;
export const CRACK_RE = /\b(crack(er|ing)?|brute[- ]?force|key[- ]?recovery|break(ing)? encryption|a5\/[123] (crack|attack)|rainbow table|decrypt(s|ing)? without (the )?key)\b/i;
const CODEC_FILE_RE = /(ambe|imbe|mbelib|dvsi|acelp|g729)/i;
const SENSITIVE = [".ssh/", "/etc/passwd", "/etc/shadow", "Login Data", "wallet.dat", "Cookies.sqlite", "\\Microsoft\\Credentials", "keychain-db"].map((s) => new TextEncoder().encode(s));

export function policyText(...texts: string[]): string {
  const blob = texts.join(" ");
  const c = CODEC_RE.exec(blob);
  if (c) return `mentions a patented voice codec (${c[0]}): codecs are installed by each user, a plugin must not ship or build them`;
  const k = CRACK_RE.exec(blob);
  if (k) return `looks like an encryption-cracking tool (${k[0]}): not accepted`;
  return "";
}
export function licenceProblem(lic: string): string {
  if (!lic.trim()) return "the manifest has no licence (an SPDX id such as MIT or GPL-3.0-or-later)";
  if (NOT_FREE.test(lic)) return `licence "${lic}" is not open or freely redistributable`;
  return "";
}
function contains(hay: Uint8Array, needle: Uint8Array): boolean {
  outer: for (let i = 0; i + needle.length <= hay.length; i++) {
    for (let j = 0; j < needle.length; j++) if (hay[i + j] !== needle[j]) continue outer;
    return true;
  }
  return false;
}

export interface CheckedPackage { manifest: Manifest; platforms: string[]; files: number; unpacked: number }

// The whole package check. Throws ManifestError (or ZipError) with a sentence for the uploader.
export async function checkPackage(bytes: Uint8Array, maxUnpacked: number): Promise<CheckedPackage> {
  let z: Zip;
  try { z = new Zip(bytes, maxUnpacked, maxUnpacked); } catch (e) { throw new ManifestError((e as Error).message); }
  // the manifest sits at the top, or inside a single top folder (as Manager::installBytes looks for it)
  let mf = z.entries.find((e) => e.name === "manifest.json"), prefix = "";
  if (!mf) {
    mf = z.entries.find((e) => { const s = e.name.indexOf("/"); return s > 0 && e.name.slice(s) === "/manifest.json"; });
    if (mf) prefix = mf.name.slice(0, mf.name.indexOf("/") + 1);
  }
  if (!mf) fail("the package has no manifest.json");
  let manifest: Manifest;
  try { manifest = parseManifest(new TextDecoder("utf-8", { fatal: true }).decode(await z.read(mf!))); } catch (e) {
    if (e instanceof ManifestError) throw e;
    throw new ManifestError(e instanceof ZipError ? e.message : "manifest.json is not UTF-8 text");
  }
  const lic = licenceProblem(manifest.license);
  if (lic) fail(lic);
  const pol = policyText(manifest.name, manifest.description, manifest.id, JSON.stringify(manifest.modes));
  if (pol) fail(pol);
  const names = new Set(z.entries.filter((e) => !e.dir).map((e) => e.name));
  const platforms: string[] = [];
  for (const p of PLATFORMS) {
    const lib = manifest.lib[p];
    if (!lib) continue;
    if (!names.has(prefix + lib)) fail(`the manifest names ${lib} as the ${p} library, but it is not in the package`);
    platforms.push(p);
  }
  if (platforms.length === 0) {
    if (manifest.kind !== "panel-lite") fail('the manifest names no library ("library": {"macos": ..., "linux": ..., "windows": ...})');
    platforms.push(...PLATFORMS);
  }
  const fsPerm = manifest.permissions.includes("filesystem");
  let unpacked = 0;
  for (const e of z.entries) {
    if (e.dir) continue;
    if (prefix && !e.name.startsWith(prefix)) fail("every file must be inside the plugin's folder " + prefix);
    if (CODEC_FILE_RE.test(e.name)) fail(`file ${e.name} looks like a patented voice codec build: not accepted`);
    let data: Uint8Array;
    try { data = await z.read(e); } catch (x) { throw new ManifestError((x as Error).message); }
    unpacked += data.length;
    if (!fsPerm) for (const s of SENSITIVE) if (contains(data, s)) fail(`${e.name} refers to files outside the plugin's folder (credentials, wallets, system files) but the manifest does not ask for the filesystem permission`);
  }
  return { manifest, platforms, files: names.size, unpacked };
}
