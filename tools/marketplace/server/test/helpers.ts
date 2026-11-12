// Test stand-ins: D1 backed by a real SQLite (sql.js, running schema.sql), R2 as a Map, GitHub as a scripted fetch, and a zip writer.
import { readFileSync } from "node:fs";
import { deflateRawSync } from "node:zlib";
import { createPublicKey, verify as nodeVerify, randomBytes } from "node:crypto";
import initSqlJs from "sql.js";
import type { D1Database, D1PreparedStatement, D1Result, Deps, Env, R2Bucket, R2ObjectBody } from "../src/types.js";
import { handle } from "../src/app.js";
import { crc32 } from "../src/zip.js";
import { makeSession, ed25519PublicHex } from "../src/crypto.js";

const here = new URL(".", import.meta.url).pathname;   // dist-test/test/
const schema = readFileSync(here + "../../schema.sql", "utf8");

type SqlDb = { prepare(sql: string): any; exec(sql: string): unknown; getRowsModified(): number; run(sql: string, p?: unknown[]): void };
let SQL: { Database: new () => SqlDb } | null = null;

export async function makeD1(): Promise<D1Database & { raw: SqlDb }> {
  if (!SQL) SQL = (await initSqlJs()) as any;
  const db = new SQL!.Database();
  db.exec(schema);
  const conv = (v: unknown) => (v === undefined ? null : typeof v === "boolean" ? (v ? 1 : 0) : v);
  const exec = (sql: string, args: unknown[]) => {
    const st = db.prepare(sql);
    st.bind(args.map(conv));
    const rows: Record<string, unknown>[] = [];
    while (st.step()) rows.push(st.getAsObject());
    st.free();
    return { rows, changes: db.getRowsModified() };
  };
  const make = (sql: string): D1PreparedStatement & { _run(): D1Result } => {
    let args: unknown[] = [];
    const s = {
      bind(...v: unknown[]) { args = v; return s; },
      async first<T>() { const r = exec(sql, args); return (r.rows[0] as T) ?? null; },
      async all<T>() { const r = exec(sql, args); return { results: r.rows as T[], meta: { changes: r.changes } }; },
      async run() { return s._run(); },
      _run(): D1Result { const r = exec(sql, args); return { results: r.rows, meta: { changes: r.changes } }; },
    };
    return s as any;
  };
  return {
    raw: db,
    prepare: make,
    async batch(stmts: D1PreparedStatement[]) {
      db.exec("BEGIN");
      try {
        const out = stmts.map((s) => (s as any)._run());
        db.exec("COMMIT");
        return out;
      } catch (e) { db.exec("ROLLBACK"); throw e; }
    },
  };
}

export function makeR2(): R2Bucket & { m: Map<string, Uint8Array> } {
  const m = new Map<string, Uint8Array>();
  const obj = (d: Uint8Array): R2ObjectBody => ({
    size: d.length,
    body: new Blob([d as BlobPart]).stream() as ReadableStream<Uint8Array>,
    async arrayBuffer() { return d.slice().buffer; },
  });
  return {
    m,
    async put(k, v) { m.set(k, new Uint8Array(v instanceof Uint8Array ? v : new Uint8Array(v)).slice()); return {}; },
    async get(k) { const d = m.get(k); return d ? obj(d) : null; },
    async delete(k) { for (const x of Array.isArray(k) ? k : [k]) m.delete(x); },
  };
}

// a zip like the SDK tools write; method 8 entries are deflated with node:zlib
export function makeZip(files: [string, string | Uint8Array, number?][], opts: { encrypted?: boolean } = {}): Uint8Array {
  const parts: Uint8Array[] = [], cd: Uint8Array[] = [];
  let off = 0;
  const u16 = (v: number) => [v & 255, (v >> 8) & 255];
  const u32 = (v: number) => [v & 255, (v >> 8) & 255, (v >> 16) & 255, (v >>> 24) & 255];
  for (const [name, content, method = 0] of files) {
    const data = typeof content === "string" ? new TextEncoder().encode(content) : content;
    const comp = method === 8 ? new Uint8Array(deflateRawSync(data)) : data;
    const nm = new TextEncoder().encode(name), crc = crc32(data), flags = opts.encrypted ? 1 : 0;
    const lh = new Uint8Array([...u32(0x04034b50), ...u16(20), ...u16(flags), ...u16(method), 0, 0, 0x21, 0, ...u32(crc), ...u32(comp.length), ...u32(data.length), ...u16(nm.length), 0, 0]);
    parts.push(lh, nm, comp);
    cd.push(new Uint8Array([...u32(0x02014b50), ...u16(20), ...u16(20), ...u16(flags), ...u16(method), 0, 0, 0x21, 0, ...u32(crc), ...u32(comp.length), ...u32(data.length), ...u16(nm.length), 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, ...u32(off)]), nm);
    off += lh.length + nm.length + comp.length;
  }
  const cdLen = cd.reduce((a, b) => a + b.length, 0);
  const eocd = new Uint8Array([...u32(0x06054b50), 0, 0, 0, 0, ...u16(files.length), ...u16(files.length), ...u32(cdLen), ...u32(off), 0, 0]);
  const all = [...parts, ...cd, eocd];
  const out = new Uint8Array(all.reduce((a, b) => a + b.length, 0));
  let p = 0;
  for (const a of all) { out.set(a, p); p += a.length; }
  return out;
}

export function manifest(over: Record<string, unknown> = {}): Record<string, unknown> {
  return {
    id: "tone-finder", name: "Tone finder", version: "1.0.0", author: "Test Author", license: "MIT", kind: "mode", abi_version: 1,
    library: { macos: "libtone.dylib", linux: "libtone.so", windows: "tone.dll" }, permissions: ["record"],
    modes: [{ id: "tone", name: "Tone", default_frequency_hz: 144e6, bandwidth_hz: 12500 }], ...over,
  };
}
export function pkg(over: Record<string, unknown> = {}, extra: [string, string | Uint8Array, number?][] = []): Uint8Array {
  const m = manifest(over);
  const lib = m.library as Record<string, string> | undefined;
  const files: [string, string | Uint8Array, number?][] = [["manifest.json", JSON.stringify(m), 8]];
  if (lib && typeof lib === "object") for (const f of Object.values(lib)) files.push([f, "binary " + f + " ".repeat(200), 8]);
  return makeZip([...files, ...extra]);
}

export const SEED = randomBytes(32).toString("hex");
export const ADMIN = "admin-token-for-tests-0123456789";
export interface Ctx { env: Env; deps: Deps & { clock: number; gh: GitHubFake }; db: D1Database & { raw: SqlDb }; r2: ReturnType<typeof makeR2>; publicHex: string }
export interface GitHubFake { pending: number; user: { id: number; login: string }; calls: string[]; tokensIssued: string[] }

export async function setup(envOver: Partial<Env> = {}): Promise<Ctx> {
  const db = await makeD1(), r2 = makeR2();
  const gh: GitHubFake = { pending: 1, user: { id: 4242, login: "alice" }, calls: [], tokensIssued: [] };
  const deps = {
    clock: 1_800_000_000, gh,
    now() { return deps.clock; },
    async fetch(input: RequestInfo | URL, init?: RequestInit) {
      const url = String(input);
      gh.calls.push(url);
      if (url === "https://github.com/login/device/code") return Response.json({ device_code: "dev-code-123456", user_code: "ABCD-1234", verification_uri: "https://github.com/login/device", expires_in: 900, interval: 5 });
      if (url === "https://github.com/login/oauth/access_token") {
        if (gh.pending > 0) { gh.pending--; return Response.json({ error: "authorization_pending" }); }
        const t = "gho_" + randomBytes(12).toString("hex");
        gh.tokensIssued.push(t);
        return Response.json({ access_token: t, token_type: "bearer", scope: "" });
      }
      if (url === "https://api.github.com/user") {
        const auth = new Headers(init?.headers).get("authorization") || "";
        if (!gh.tokensIssued.some((t) => auth === "Bearer " + t)) return new Response("{}", { status: 401 });
        return Response.json(gh.user);
      }
      return new Response("not found", { status: 404 });
    },
  };
  const env: Env = {
    DB: db, FILES: r2, PUBLIC_URL: "https://plugins.example", GITHUB_CLIENT_ID: "Iv1.testclient", OWNER_LOGINS: "owner",
    SIGNING_KEY: SEED, SESSION_SECRET: "session-secret-for-tests-0123456789", HASH_SALT: "hash-salt-for-tests-0123456789", ADMIN_TOKEN: ADMIN, ...envOver,
  };
  return { env, deps, db, r2, publicHex: await ed25519PublicHex(SEED) };
}

export async function token(c: Ctx, uid = 4242, login = "alice", expIn = 86400): Promise<string> {
  return makeSession(c.env.SESSION_SECRET!, { uid, login, iat: c.deps.clock, exp: c.deps.clock + expIn });
}

export async function call(c: Ctx, method: string, path: string, opts: { body?: unknown; form?: FormData; auth?: string; ip?: string; headers?: Record<string, string> } = {}) {
  const headers: Record<string, string> = { "cf-connecting-ip": opts.ip || "198.51.100.7", ...(opts.headers || {}) };
  if (opts.auth) headers.authorization = "Bearer " + opts.auth;
  let body: BodyInit | undefined;
  if (opts.form) body = opts.form;
  else if (opts.body !== undefined) { body = JSON.stringify(opts.body); headers["content-type"] = "application/json"; }
  const req = new Request("https://plugins.example" + path, { method, headers, body });
  const res = await handle(req, c.env, c.deps);
  const text = res.status === 302 ? "" : await res.text();
  let j: any = null;
  try { j = text ? JSON.parse(text) : null; } catch { j = null; }
  return { res, status: res.status, text, j };
}

// what OnAir does with a signed answer: verify the Ed25519 signature over the exact text with the built-in public key
export function verifySigned(publicHex: string, j: { signed: string; sig: string }): boolean {
  const key = createPublicKey({ key: { kty: "OKP", crv: "Ed25519", x: Buffer.from(publicHex, "hex").toString("base64url") }, format: "jwk" });
  return nodeVerify(null, Buffer.from(j.signed, "utf8"), key, Buffer.from(j.sig, "hex"));
}
export function opened(c: Ctx, r: { j: any }): any {
  if (!r.j || typeof r.j.signed !== "string") throw new Error("not a signed answer: " + JSON.stringify(r.j));
  if (!verifySigned(c.publicHex, r.j)) throw new Error("bad signature");
  return JSON.parse(r.j.signed);
}

export function uploadForm(bytes: Uint8Array, fields: Record<string, string> = { title: "Tone finder", description: "Finds steady tones in the band and logs them." }): FormData {
  const fd = new FormData();
  fd.append("package", new Blob([bytes as BlobPart]), "plugin.onairplugin");
  for (const [k, v] of Object.entries(fields)) fd.append(k, v);
  return fd;
}
