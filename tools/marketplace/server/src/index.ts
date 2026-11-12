// The Worker entry. Only the default export lives here: the Workers runtime treats every named export of the main module as an entrypoint.
// The handlers are in app.ts (the tests call them directly).
import type { Deps, Env } from "./types.js";
import { cleanup, handle } from "./app.js";

const realDeps: Deps = { fetch: (i, init) => fetch(i, init), now: () => Math.floor(Date.now() / 1000) };
export default {
  fetch(req: Request, env: Env): Promise<Response> { return handle(req, env, realDeps); },
  async scheduled(_ev: unknown, env: Env): Promise<void> { await cleanup(env, realDeps.now()); },
};
