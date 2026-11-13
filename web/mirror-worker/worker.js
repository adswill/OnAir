// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Example update mirror for OnAir (docs/UPDATE-MIRROR.md). A Cloudflare Worker that forwards GET requests for the project's own release
// list and release files, and nothing else. Use it as  https://<your-domain>/{url}  (the project's own copy: https://mirror.on-air.dev/{url})
const RELEASES_API = /^https:\/\/api\.github\.com\/repos\/adswill\/OnAir\/releases(\?per_page=\d{1,3})?$/;
const RELEASE_FILE = /^https:\/\/github\.com\/adswill\/OnAir\/releases\/download\/[A-Za-z0-9._-]{1,64}\/[A-Za-z0-9._-]{1,128}$/;

export default {
  async fetch(request) {
    if (request.method !== 'GET') return new Response('method not allowed', { status: 405 });
    const u = new URL(request.url);
    const target = decodeURIComponent(u.pathname.slice(1)) + u.search; // /https://api.github.com/... (the query belongs to the target)
    if (!RELEASES_API.test(target) && !RELEASE_FILE.test(target)) return new Response('not allowed', { status: 403 });
    const r = await fetch(target, { headers: { 'User-Agent': 'OnAir-mirror', Accept: request.headers.get('Accept') || '*/*' }, redirect: 'follow' });
    const h = new Headers();
    for (const k of ['content-type', 'content-length', 'content-disposition']) if (r.headers.has(k)) h.set(k, r.headers.get(k));
    h.set('cache-control', RELEASES_API.test(target) ? 'public, max-age=300' : 'public, max-age=86400'); // the list is short-lived, a release file never changes
    return new Response(r.body, { status: r.status, headers: h });
  },
};
