// The small part of the Cloudflare bindings this Worker uses. Written out here (instead of @cloudflare/workers-types) so the tests run in plain
// Node with a real SQLite (sql.js) standing in for D1 and a Map standing in for R2.

export interface D1Result<T = Record<string, unknown>> {
  results: T[];
  meta?: { changes?: number; last_row_id?: number };
}
export interface D1PreparedStatement {
  bind(...values: unknown[]): D1PreparedStatement;
  first<T = Record<string, unknown>>(): Promise<T | null>;
  all<T = Record<string, unknown>>(): Promise<D1Result<T>>;
  run(): Promise<D1Result>;
}
export interface D1Database {
  prepare(sql: string): D1PreparedStatement;
  batch(statements: D1PreparedStatement[]): Promise<D1Result[]>;
}

export interface R2ObjectBody {
  size: number;
  body: ReadableStream<Uint8Array>;
  arrayBuffer(): Promise<ArrayBuffer>;
  httpEtag?: string;
}
export interface R2Bucket {
  put(key: string, value: ArrayBuffer | Uint8Array, options?: { httpMetadata?: { contentType?: string }; customMetadata?: Record<string, string> }): Promise<unknown>;
  get(key: string): Promise<R2ObjectBody | null>;
  delete(key: string | string[]): Promise<void>;
}

export interface Env {
  DB: D1Database;
  FILES: R2Bucket;
  // vars (wrangler.jsonc)
  PUBLIC_URL?: string;          // https://plugins.on-air.dev (used for the download URLs in signed answers)
  FILES_BASE_URL?: string;      // optional public R2 domain; empty = files are served by this Worker at /files/<key>
  GITHUB_CLIENT_ID?: string;    // the OAuth app's client id (public; device flow needs no client secret)
  OWNER_LOGINS?: string;        // GitHub logins allowed to use ids starting with "onair" (comma separated)
  MAX_PACKAGE_BYTES?: string;   // upload size limit (default 25 MiB)
  // secrets (wrangler secret put ...; never in a file)
  SIGNING_KEY?: string;         // Ed25519 seed, 64 hex digits (scripts/keygen.mjs)
  SESSION_SECRET?: string;      // 32+ random bytes, any text: signs the app's session tokens
  HASH_SALT?: string;           // 32+ random bytes, any text: salts the install id and IP hashes
  ADMIN_TOKEN?: string;         // 32+ random characters: the moderation endpoints
}

// What the handler needs from the outside world, so the tests can replace it.
export interface Deps {
  fetch: typeof fetch;          // GitHub calls
  now(): number;                // unix seconds
}
