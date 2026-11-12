// The upload check on its own: the manifest rules (a port of plugin::parseManifest) and the package rules (zip, libraries, policy).
import test from "node:test";
import assert from "node:assert/strict";
import { checkPackage, compareVersions, parseManifest, ManifestError } from "../src/manifest.js";
import { makeZip, manifest, pkg } from "./helpers.js";

const LIM = 64 << 20;
async function refused(bytes: Uint8Array, re: RegExp) {
  await assert.rejects(() => checkPackage(bytes, LIM), (e: unknown) => e instanceof ManifestError && re.test((e as Error).message));
}

test("a good package passes and names its platforms", async () => {
  const r = await checkPackage(pkg(), LIM);
  assert.equal(r.manifest.id, "tone-finder");
  assert.deepEqual(r.platforms, ["macos", "linux", "windows"]);
  assert.deepEqual(r.manifest.permissions, ["record"]);
});

test("the manifest may sit in one top folder", async () => {
  const m = manifest();
  const z = makeZip([["tone/manifest.json", JSON.stringify(m)], ["tone/libtone.dylib", "x"], ["tone/libtone.so", "x"], ["tone/tone.dll", "x"]]);
  assert.equal((await checkPackage(z, LIM)).files, 4);
});

test("only the platforms whose library is named", async () => {
  const r = await checkPackage(pkg({ library: { linux: "libtone.so" } }), LIM);
  assert.deepEqual(r.platforms, ["linux"]);
});

test("manifest rules match the client", () => {
  const ok = manifest();
  const bad: [Record<string, unknown>, RegExp][] = [
    [{ id: "" }, /"id" is empty/], [{ id: "Tone" }, /"id" must be/], [{ id: "-x" }, /"id" must be/], [{ id: "a..b" }, /"id" must be/],
    [{ version: "v1" }, /"version"/], [{ kind: "driver" }, /"kind"/], [{ abi_version: 2 }, /ABI 2/], [{ abi_version: "1" }, /abi_version/],
    [{ permissions: ["root"] }, /unknown permission/], [{ modes: [] }, /at least one entry/], [{ homepage: "ftp://x" }, /homepage/],
    [{ library: { macos: "../evil.dylib" } }, /plain file names/], [{ name: "x".repeat(81) }, /at most 80/], [{ minimum_onair: "new" }, /minimum_onair/],
    [{ modes: [{ id: "a", name: "A" }, { id: "a", name: "B" }] }, /used twice/], [{ settings: [{ key: "k", label: "L", type: "knob" }] }, /type must be/],
  ];
  assert.doesNotThrow(() => parseManifest(JSON.stringify(ok)));
  for (const [o, re] of bad) assert.throws(() => parseManifest(JSON.stringify({ ...ok, ...o })), re, JSON.stringify(o));
  const noAbi = { ...ok } as Record<string, unknown>;
  delete noAbi.abi_version;
  assert.throws(() => parseManifest(JSON.stringify(noAbi)), /missing "abi_version"/);
  assert.throws(() => parseManifest("[1]"), /JSON object/);
  assert.throws(() => parseManifest("{"), /not valid JSON/);
  assert.throws(() => parseManifest(" ".repeat(65 * 1024)), /64 KiB/);
});

test("versions compare like the client", () => {
  assert.equal(compareVersions("1.0.10", "1.0.9"), 1);
  assert.equal(compareVersions("1.0", "1.0.0"), 0);
  assert.equal(compareVersions("1.2.0-beta", "1.2.0"), 0);
  assert.equal(compareVersions("0.9", "1.0"), -1);
});

test("damaged and hostile packages are refused", async () => {
  await refused(new TextEncoder().encode("not a zip at all, not a zip at all"), /not a zip/);
  await refused(makeZip([["readme.txt", "hi"]]), /no manifest.json/);
  await refused(makeZip([["manifest.json", "{"]]), /not valid JSON/);
  await refused(makeZip([["manifest.json", JSON.stringify(manifest())], ["../escape.so", "x"]]), /unsafe file name/);
  await refused(makeZip([["manifest.json", JSON.stringify(manifest())], ["a/b/c/d/e.so", "x"]]), /unsafe file name/);
  await refused(makeZip([["manifest.json", JSON.stringify(manifest())]], { encrypted: true }), /encrypted/);
  await refused(makeZip([["manifest.json", JSON.stringify(manifest())]]), /libtone.dylib as the macos library, but it is not in the package/);
  await refused(pkg({ library: undefined }), /names no library/);
  const many: [string, string][] = [["manifest.json", JSON.stringify(manifest({ kind: "panel-lite", library: undefined, modes: undefined }))]];
  for (let i = 0; i < 64; i++) many.push(["f" + i, "x"]);
  await refused(makeZip(many), /more than 64 entries/);
  // a flipped byte in a stored entry: the CRC catches it
  const z = makeZip([["manifest.json", JSON.stringify(manifest({ library: { linux: "libtone.so" } }))], ["libtone.so", "binary libtone, stored"]]);
  const i = Buffer.from(z).indexOf("binary libtone");
  assert.ok(i > 0);
  z[i] ^= 1;
  await refused(z, /checksum error/);
});

test("a zip bomb stops at the size limit", async () => {
  const big = new Uint8Array(3 << 20);   // 3 MiB of zeros, deflates to a few KiB
  const z = makeZip([["manifest.json", JSON.stringify(manifest({ kind: "panel-lite", library: undefined, modes: undefined }))], ["zeros.bin", big, 8]]);
  assert.ok(z.length < 64 * 1024);
  await assert.rejects(() => checkPackage(z, 1 << 20), /size limit/);
  assert.equal((await checkPackage(z, LIM)).unpacked > 3e6, true);
  // the same entry claiming to be 100 bytes: inflating stops at 100 bytes instead of trusting the header
  const liar = z.slice();
  const dv = new DataView(liar.buffer);
  const eocd = liar.length - 22, cdOff = dv.getUint32(eocd + 16, true);
  let p = cdOff;
  for (let k = 0; k < 2; k++) {
    const nl = dv.getUint16(p + 28, true);
    if (k === 1) { dv.setUint32(p + 24, 100, true); dv.setUint32(dv.getUint32(p + 42, true) + 22, 100, true); }
    p += 46 + nl;
  }
  await assert.rejects(() => checkPackage(liar, LIM), /more than it says/);
});

test("listing policy: licence, codecs, cracking, sensitive paths", async () => {
  await refused(pkg({ license: "Proprietary" }), /not open/);
  await refused(pkg({ license: "CC-BY-NC-4.0" }), /not open/);
  await refused(pkg({ description: "Decodes voice with mbelib" }), /patented voice codec/);
  await refused(pkg({ name: "Key recovery tool" }), /encryption-cracking/);
  await refused(pkg({}, [["libambe.so", "x"]]), /patented voice codec build/);
  await refused(pkg({}, [["data.txt", "reads ~/.ssh/id_rsa"]]), /filesystem permission/);
  const ok = await checkPackage(pkg({ permissions: ["filesystem"] }, [["data.txt", "reads ~/.ssh/id_rsa"]]), LIM);
  assert.ok(ok.manifest.permissions.includes("filesystem"));
});
