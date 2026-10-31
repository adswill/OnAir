#!/usr/bin/env python3
"""A fake OnAir directory Worker for tests (no network, no Cloudflare, no database).

It answers the same routes as web/onair-directory/src/index.js (GET /api/v1/health, GET /api/v1/servers, POST /api/v1/heartbeat) and validates a heartbeat by the
same rules as web/onair-directory/src/logic.js: allowed keys, field shapes, the clock window, the normal form of the url, the Ed25519 signature over the
canonical text, the id derived from the key, ts newer than the last one. On top of that it can misbehave on request, which is what the tests need:

  POST /__mode  {"mode": "ok" | "http500" | "http429" | "http401" | "html502" | "slow" | "hang" | "resolving" | "clock" | "badjson", "n": 2, "delay": 3}
      ok         normal
      http500    answer 500 with an HTML body
      http429    answer 429 (rate limit)
      http401    answer 401 (signature does not verify)
      html502    answer 502 with an HTML body
      slow       answer correctly after `delay` seconds
      hang       accept the request and never answer
      resolving  answer 422 (url host does not resolve) for the first `n` heartbeats, then normal
      clock      answer 400 "ts is too far" and a Date header one hour ahead
      badjson    answer 200 with a body that is not JSON
  GET /__log     the heartbeats received so far (JSON)

usage: fake_directory.py PORT [--tls CERT KEY]  (prints "ready" when it listens)
"""
import hashlib, json, re, ssl, sys, threading, time
from email.utils import formatdate
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

# ---- Ed25519 verify (RFC 8032, reference implementation; slow but enough for a test)
P = 2**255 - 19
Q = 2**252 + 27742317777372353535851937790883648493
D = -121665 * pow(121666, P - 2, P) % P
I = pow(2, (P - 1) // 4, P)
def _inv(x): return pow(x, P - 2, P)
def _xrec(y):
    xx = (y * y - 1) * _inv(D * y * y + 1)
    x = pow(xx, (P + 3) // 8, P)
    if (x * x - xx) % P != 0: x = x * I % P
    if (x * x - xx) % P != 0: raise ValueError
    return P - x if x & 1 else x
BY = 4 * _inv(5) % P
B = (_xrec(BY), BY, 1, _xrec(BY) * BY % P)
def _add(a, b):
    A = (a[1] - a[0]) * (b[1] - b[0]) % P; Bb = (a[1] + a[0]) * (b[1] + b[0]) % P
    C = 2 * a[3] * b[3] * D % P; Dd = 2 * a[2] * b[2] % P
    E, F, G, H = Bb - A, Dd - C, Dd + C, Bb + A
    return (E * F % P, G * H % P, F * G % P, E * H % P)
def _mul(s, pt):
    r = (0, 1, 1, 0)
    while s:
        if s & 1: r = _add(r, pt)
        pt = _add(pt, pt); s >>= 1
    return r
def _dec(s):
    y = int.from_bytes(s, "little"); sign = y >> 255; y &= (1 << 255) - 1
    x = _xrec(y)
    if x & 1 != sign: x = P - x
    return (x, y, 1, x * y % P)
def _eq(a, b): return (a[0] * b[2] - b[0] * a[2]) % P == 0 and (a[1] * b[2] - b[1] * a[2]) % P == 0
def ed25519_verify(pub, msg, sig):
    try:
        if len(pub) != 32 or len(sig) != 64: return False
        A = _dec(pub); R = _dec(sig[:32]); S = int.from_bytes(sig[32:], "little")
        if S >= Q: return False
        h = int.from_bytes(hashlib.sha512(sig[:32] + pub + msg).digest(), "little") % Q
        return _eq(_mul(S, B), _add(R, _mul(h, A)))
    except Exception:
        return False

# ---- the rules of web/onair-directory/src/logic.js
HEADER = "OnAir-Directory-v1"
KEYS = ["action", "id", "pub", "ts", "name", "url", "loc", "freq", "radio", "cap", "users", "interval", "ver", "contact", "desc", "reach"]
ALLOWED = set(KEYS) | {"sig", "v"}
def canonical(rec):
    keys = ["action", "id", "pub", "ts"] if rec.get("action") == "delete" else KEYS
    s = HEADER + "\n"
    for k in keys:
        v = rec.get(k)
        if k == "freq": v = ";".join("%d-%d" % (a, b) for a, b in (v or []))
        if v is None: v = ""
        s += "%s=%s\n" % (k, v)
    return s
def has_control(s): return re.search(r"[\x00-\x1f\x7f-\x9f\u2028\u2029]", s) is not None
def validate_public_url(t):
    m = re.match(r"^(https?)://([^/:?#@\s]+)(:(\d+))?(/[A-Za-z0-9._~\-/]*)?$", t or "")
    if not m or len(t) > 200: return None
    host = m.group(2).lower()
    if "." not in host or host == "localhost" or any(host.endswith(x) for x in (".local", ".internal", ".lan", ".home", ".test", ".example", ".invalid", ".onion", ".arpa")): return None
    port = int(m.group(4)) if m.group(4) else (443 if m.group(1) == "https" else 80)
    if port < 1024 and port not in (80, 443): return None
    path = m.group(5) or "/"
    if not path.endswith("/"): path += "/"
    return "%s://%s%s%s" % (m.group(1), host, (":" + m.group(4)) if m.group(4) else "", path)
def validate(body, now):
    errs = []
    if not isinstance(body, dict): return ["body must be a JSON object"], None
    for k in body:
        if k not in ALLOWED: errs.append("unknown field " + k)
    if body.get("v") != 1: errs.append("v must be 1")
    if body.get("action") not in ("heartbeat", "delete"): errs.append("action must be heartbeat or delete")
    if not re.match(r"^[a-z0-9][a-z0-9-]{7,31}$", str(body.get("id", ""))): errs.append("id must be 8 to 32 characters of a-z 0-9 -")
    if not re.match(r"^[0-9a-f]{64}$", str(body.get("pub", ""))): errs.append("pub must be 64 hex characters")
    if not re.match(r"^[0-9a-f]{128}$", str(body.get("sig", ""))): errs.append("sig must be 128 hex characters")
    ts = body.get("ts")
    if not isinstance(ts, int) or ts < 1: errs.append("ts must be an integer from 1 to 4102444800")
    elif abs(ts - now) > 300: errs.append("ts is too far from the directory's clock")
    if body.get("action") == "heartbeat":
        name = body.get("name", "")
        if not isinstance(name, str) or not name.strip() or len(name.encode()) > 48 or has_control(name) or name != name.strip(): errs.append("name is required (1 to 48 bytes, no spaces at the ends)")
        u = validate_public_url(body.get("url"))
        if not u: errs.append("url is not a public https address")
        elif u != body.get("url"): errs.append("url must be written in its normal form: " + u)
        loc = body.get("loc", "")
        if loc and not re.match(r"^(-?\d{1,2}(\.\d{1,5})?,-?\d{1,3}(\.\d{1,5})?|[A-Ra-r]{2}\d{2}([A-Xa-x]{2}(\d{2})?)?)$", loc): errs.append("loc must be \"lat,lon\" or a Maidenhead locator")
        fr = body.get("freq", [])
        if not isinstance(fr, list) or len(fr) > 8 or any((not isinstance(r, list) or len(r) != 2 or not all(isinstance(x, int) for x in r) or r[0] < 0 or r[1] < r[0]) for r in fr): errs.append("freq ranges are [low_hz, high_hz]")
        for k, lim in (("cap", 100000), ("users", 100000)):
            if not isinstance(body.get(k), int) or body[k] < 0 or body[k] > lim: errs.append("%s must be an integer from 0 to %d" % (k, lim))
        if isinstance(body.get("users"), int) and isinstance(body.get("cap"), int) and body["cap"] > 0 and body["users"] > body["cap"]: errs.append("users is more than cap")
        iv = body.get("interval", 300)
        if not isinstance(iv, int) or iv < 60 or iv > 3600: errs.append("interval must be an integer from 60 to 3600")
        for k, mx in (("radio", 48), ("ver", 24), ("contact", 80), ("desc", 280)):
            v = body.get(k, "")
            if not isinstance(v, str) or len(v.encode()) > mx or has_control(v) or v != v.strip(): errs.append("%s is not valid" % k)
    return errs, body

class State:
    def __init__(self):
        self.mode = "ok"; self.n = 0; self.delay = 3.0; self.log = []; self.last_ts = {}; self.keys = {}; self.hb = 0; self.lock = threading.Lock()
S = State()

class H(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    def log_message(self, *a): pass
    def _send(self, code, body, ctype="application/json", extra=None):
        data = body if isinstance(body, bytes) else body.encode()
        self.send_response(code)
        self.send_header("Content-Type", ctype); self.send_header("Content-Length", str(len(data))); self.send_header("Connection", "close")
        for k, v in (extra or {}).items(): self.send_header(k, v)
        self.end_headers(); self.wfile.write(data)
    def do_GET(self):
        if self.path == "/api/v1/health": return self._send(200, '{"ok":true,"proto":1}')
        if self.path.startswith("/api/v1/servers"): return self._send(200, json.dumps({"ok": True, "count": len(S.keys), "servers": []}))
        if self.path == "/__log":
            with S.lock: return self._send(200, json.dumps(S.log))
        if self.path == "/web/v1/info": return self._send(200, '{"proto":1,"name":"fake","users":0,"max_users":8}')
        self._send(404, '{"ok":false,"error":"not found"}')
    def do_POST(self):
        n = int(self.headers.get("Content-Length", "0"))
        raw = self.rfile.read(n)
        if self.path == "/__mode":
            j = json.loads(raw)
            with S.lock:
                S.mode = j.get("mode", "ok"); S.n = int(j.get("n", 0)); S.delay = float(j.get("delay", 3)); S.hb = 0
            return self._send(200, '{"ok":true}')
        if self.path != "/api/v1/heartbeat": return self._send(404, '{"ok":false,"error":"not found"}')
        with S.lock:
            mode, S.hb = S.mode, S.hb + 1
            hb = S.hb
        if mode == "hang": time.sleep(120); return
        if mode == "slow": time.sleep(S.delay)
        if mode == "http500": return self._send(500, "<html><body>Internal Server Error</body></html>", "text/html")
        if mode == "html502": return self._send(502, "<html><body>Bad gateway</body></html>", "text/html")
        if mode == "http429": return self._send(429, '{"ok":false,"error":"too many requests from this address"}')
        if mode == "http401": return self._send(401, '{"ok":false,"error":"signature does not verify"}')
        if mode == "badjson": return self._send(200, "this is not json")
        if mode == "resolving" and hb <= S.n: return self._send(422, '{"ok":false,"error":"url host does not resolve to public addresses only"}')
        if mode == "clock": return self._send(400, '{"ok":false,"error":"invalid","details":["ts is too far from the directory\'s clock"]}', extra={"Date": formatdate(time.time() + 3600, usegmt=True)})
        if not self.headers.get("Content-Type", "").lower().startswith("application/json"): return self._send(415, '{"ok":false,"error":"content-type must be application/json"}')
        if n > 4096: return self._send(413, '{"ok":false,"error":"body too large"}')
        try: body = json.loads(raw)
        except Exception: return self._send(400, '{"ok":false,"error":"body is not JSON"}')
        errs, rec = validate(body, int(time.time()))
        if errs: return self._send(400, json.dumps({"ok": False, "error": "invalid", "details": errs[:8]}))
        if not ed25519_verify(bytes.fromhex(rec["pub"]), canonical(rec).encode(), bytes.fromhex(rec["sig"])): return self._send(401, '{"ok":false,"error":"signature does not verify"}')
        if rec["id"] != "onair-" + hashlib.sha256(bytes.fromhex(rec["pub"])).hexdigest()[:16]: return self._send(400, '{"ok":false,"error":"invalid","details":["id does not belong to the key"]}')
        with S.lock:
            if rec["id"] in S.keys and S.keys[rec["id"]] != rec["pub"]: return self._send(403, '{"ok":false,"error":"id is registered with another key"}')
            if rec["ts"] <= S.last_ts.get(rec["id"], 0): return self._send(409, '{"ok":false,"error":"ts is not newer than the last accepted request"}')
            new = rec["id"] not in S.keys
            S.keys[rec["id"]] = rec["pub"]; S.last_ts[rec["id"]] = rec["ts"]
            S.log.append({"action": rec["action"], "name": rec.get("name"), "url": rec.get("url"), "freq": rec.get("freq"), "users": rec.get("users"), "cap": rec.get("cap"), "radio": rec.get("radio"), "id": rec["id"], "loc": rec.get("loc")})
        return self._send(200, json.dumps({"ok": True, "new": new, "next_sec": rec.get("interval", 300)}))

def main():
    port = int(sys.argv[1])
    srv = ThreadingHTTPServer(("127.0.0.1", port), H)
    srv.daemon_threads = True
    if "--tls" in sys.argv:
        i = sys.argv.index("--tls")
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER); ctx.load_cert_chain(sys.argv[i + 1], sys.argv[i + 2])
        srv.socket = ctx.wrap_socket(srv.socket, server_side=True)
    print("ready", flush=True)
    srv.serve_forever()

if __name__ == "__main__":
    main()
