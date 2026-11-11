#!/usr/bin/env python3
"""OnAir plugin registry tool: lint, build the index, sign, verify. Python 3.8+, standard library only.

  onair_registry.py keygen KEYFILE            make the signing key (offline; never commit it)
  onair_registry.py lint REGISTRY [--fetch]   check every plugin page, its artefacts and the policy rules
  onair_registry.py index REGISTRY --serial N build index.json from plugins/*/plugin.json (+ revoked.json)
  onair_registry.py sign REGISTRY KEYFILE     write index.json.sig (Ed25519 over the exact bytes of index.json)
  onair_registry.py verify REGISTRY PUBKEY    check index.json.sig and the plugin pages against the index
  onair_registry.py artefact FILE             print size and sha256 for a plugin.json "downloads" entry

Registry layout (docs/PLUGIN-MARKETPLACE-3.9.md):
  index.json  index.json.sig  revoked.json (optional)  plugins/<id>/plugin.json  packages/... (or https URLs)

The Ed25519 code is the reference implementation of RFC 8032 section 6 (public domain, by S. Josefsson and I. Liusvaara, with
D. J. Bernstein's algorithms), rewritten here with the same maths. It is not constant time: it signs offline on the maintainer's machine only.
"""
import argparse, hashlib, io, json, os, re, stat, sys, zipfile

# ------------------------------------------------------------------ Ed25519 (RFC 8032, pure Python)
_p = 2 ** 255 - 19
_q = 2 ** 252 + 27742317777372353535851937790883648493
def _inv(x): return pow(x, _p - 2, _p)
_d = -121665 * _inv(121666) % _p
_sqrt_m1 = pow(2, (_p - 1) // 4, _p)
def _sha512(s): return hashlib.sha512(s).digest()
def _add(P, Q):
    A = (P[1] - P[0]) * (Q[1] - Q[0]) % _p
    B = (P[1] + P[0]) * (Q[1] + Q[0]) % _p
    C = 2 * P[3] * Q[3] * _d % _p
    D = 2 * P[2] * Q[2] % _p
    E, F, G, H = B - A, D - C, D + C, B + A
    return (E * F % _p, G * H % _p, F * G % _p, E * H % _p)
def _mul(s, P):
    Q = (0, 1, 1, 0)
    while s > 0:
        if s & 1: Q = _add(Q, P)
        P = _add(P, P); s >>= 1
    return Q
def _eq(P, Q):
    return (P[0] * Q[2] - Q[0] * P[2]) % _p == 0 and (P[1] * Q[2] - Q[1] * P[2]) % _p == 0
def _recover_x(y, sign):
    if y >= _p: return None
    x2 = (y * y - 1) * _inv(_d * y * y + 1)
    if x2 == 0: return None if sign else 0
    x = pow(x2, (_p + 3) // 8, _p)
    if (x * x - x2) % _p != 0: x = x * _sqrt_m1 % _p
    if (x * x - x2) % _p != 0: return None
    if (x & 1) != sign: x = _p - x
    return x
_gy = 4 * _inv(5) % _p
_gx = _recover_x(_gy, 0)
_G = (_gx, _gy, 1, _gx * _gy % _p)
def _compress(P):
    zi = _inv(P[2]); x = P[0] * zi % _p; y = P[1] * zi % _p
    return int.to_bytes(y | ((x & 1) << 255), 32, "little")
def _decompress(s):
    if len(s) != 32: return None
    y = int.from_bytes(s, "little"); sign = y >> 255; y &= (1 << 255) - 1
    x = _recover_x(y, sign)
    return None if x is None else (x, y, 1, x * y % _p)
def _expand(seed):
    h = _sha512(seed); a = int.from_bytes(h[:32], "little"); a &= (1 << 254) - 8; a |= 1 << 254
    return a, h[32:]
def _hq(s): return int.from_bytes(_sha512(s), "little") % _q
def ed_public(seed): return _compress(_mul(_expand(seed)[0], _G))
def ed_sign(seed, msg):
    a, prefix = _expand(seed); A = _compress(_mul(a, _G))
    r = _hq(prefix + msg); R = _compress(_mul(r, _G)); h = _hq(R + A + msg)
    return R + int.to_bytes((r + h * a) % _q, 32, "little")
def ed_verify(pub, msg, sig):
    if len(pub) != 32 or len(sig) != 64: return False
    A = _decompress(pub)
    if A is None: return False
    R = _decompress(sig[:32])
    if R is None: return False
    s = int.from_bytes(sig[32:], "little")
    if s >= _q: return False
    h = _hq(sig[:32] + pub + msg)
    return _eq(_mul(s, _G), _add(R, _mul(h, A)))
def _selftest():   # RFC 8032 section 7.1, test 1 (empty message)
    seed = bytes.fromhex("9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60")
    pub = bytes.fromhex("d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a")
    sig = bytes.fromhex("e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b")
    return ed_public(seed) == pub and ed_sign(seed, b"") == sig and ed_verify(pub, b"", sig) and not ed_verify(pub, b"x", sig)

# ------------------------------------------------------------------ rules
PLATFORMS = ("windows-x64", "macos-arm64", "macos-x86_64", "linux-x86_64", "linux-aarch64")
KINDS = ("mode", "action", "source", "panel-lite")
PERMS = ("network", "filesystem", "audio-out", "record", "spawn-process")
REVIEWS = ("listed", "reviewed", "verified")
ABI = 1
MAX_PACKAGE = 128 << 20
ID_RE = re.compile(r"^[a-z0-9][a-z0-9_.-]{0,63}$")
VER_RE = re.compile(r"^\d+(\.\d+){0,3}([-+][0-9A-Za-z.-]+)?$")
# SPDX ids that are open or freely redistributable (not the full list: a plugin with another free licence is added here by a maintainer)
SPDX_OK = {"MIT", "MIT-0", "ISC", "BSD-2-Clause", "BSD-3-Clause", "0BSD", "Apache-2.0", "Zlib", "Unlicense", "CC0-1.0", "BSL-1.0", "MPL-2.0",
           "GPL-2.0-only", "GPL-2.0-or-later", "GPL-3.0-only", "GPL-3.0-or-later", "LGPL-2.1-only", "LGPL-2.1-or-later", "LGPL-3.0-only",
           "LGPL-3.0-or-later", "AGPL-3.0-only", "AGPL-3.0-or-later", "CC-BY-4.0", "CC-BY-SA-4.0", "EUPL-1.2", "Artistic-2.0", "WTFPL"}
NOT_FREE = re.compile(r"proprietary|all rights reserved|-NC|-ND|non-?commercial|no[- ]?derivatives|commercial use", re.I)
# policy heuristics: a person still decides. Patented voice codecs (users install those themselves), tools that crack encryption.
CODEC_RE = re.compile(r"\b(ambe(\+2)?|imbe|mbelib|dvsi|acelp|g\.?729|evs codec|amr-?wb|hvxc|tetra[- ]?codec|vocoder build)\b", re.I)
CRACK_RE = re.compile(r"\b(crack(er|ing)?|brute[- ]?force|key[- ]?recovery|break(ing)? encryption|a5/[123] (crack|attack)|rainbow table|decrypt(s|ing)? without (the )?key)\b", re.I)
SENSITIVE_PATHS = (b".ssh/", b"/etc/passwd", b"/etc/shadow", b"Login Data", b"wallet.dat", b"Cookies.sqlite", b"\\Microsoft\\Credentials", b"keychain-db")
CODEC_FILE_RE = re.compile(r"(ambe|imbe|mbelib|dvsi|acelp|g729)", re.I)

class Report:
    def __init__(self): self.errors = []; self.warnings = []
    def err(self, where, msg): self.errors.append("%s: %s" % (where, msg))
    def warn(self, where, msg): self.warnings.append("%s: %s" % (where, msg))

def read_json(path, rep, where):
    try:
        with open(path, "rb") as f: raw = f.read()
        if len(raw) > 1 << 20: rep.err(where, "file larger than 1 MiB"); return None
        return json.loads(raw.decode("utf-8"))
    except Exception as e:
        rep.err(where, "cannot read JSON: %s" % e); return None

def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""): h.update(b)
    return h.hexdigest()

def safe_rel(r):
    if not isinstance(r, str) or not r or r.startswith("/") or "\\" in r or ":" in r or len(r) > 300: return False
    return all(p not in ("", ".", "..") for p in r.split("/"))

def check_license(lic, rep, where):
    if not isinstance(lic, str) or not lic.strip(): rep.err(where, "license is missing (an SPDX id such as MIT or GPL-3.0-or-later)"); return
    if NOT_FREE.search(lic): rep.err(where, "license %r is not open or freely redistributable (listing policy)" % lic); return
    parts = [x for x in re.split(r"\s+(?:AND|OR|WITH)\s+|[()]", lic) if x.strip()]
    for x in parts:
        x = x.strip()
        if x not in SPDX_OK and not x.startswith("LicenseRef-") and not re.match(r"^[A-Za-z0-9.+-]+$", x):
            rep.err(where, "license %r is not an SPDX identifier" % lic); return
        if x not in SPDX_OK and not x.startswith("LicenseRef-"):
            rep.warn(where, "license %r is not in the list of known open licences: a maintainer checks it" % x)

def policy_text(where, rep, *texts):
    blob = " ".join(t for t in texts if isinstance(t, str))
    if CODEC_RE.search(blob): rep.err(where, "mentions a patented voice codec (%s): codecs are installed by the user, a plugin must not ship or build them" % CODEC_RE.search(blob).group(0))
    if CRACK_RE.search(blob): rep.err(where, "looks like an encryption-cracking tool (%s): not listed" % CRACK_RE.search(blob).group(0))

def inspect_zip(data, page, platform, rep, where):
    try: z = zipfile.ZipFile(io.BytesIO(data))
    except Exception as e: rep.err(where, "not a zip file: %s" % e); return
    names = z.namelist()
    if len(names) > 64: rep.err(where, "more than 64 entries")
    for n in names:
        if n.startswith("/") or ".." in n.split("/") or "\\" in n or ":" in n: rep.err(where, "unsafe name %r in the package" % n)
        if CODEC_FILE_RE.search(n): rep.err(where, "file %r looks like a patented voice codec build: not listed" % n)
    mf = next((n for n in names if n == "manifest.json" or (n.count("/") == 1 and n.endswith("/manifest.json"))), None)
    if not mf: rep.err(where, "package has no manifest.json"); return
    prefix = mf[:-len("manifest.json")]
    try: m = json.loads(z.read(mf).decode("utf-8"))
    except Exception as e: rep.err(where, "manifest.json in the package is not valid: %s" % e); return
    for k in ("id", "version"):
        if m.get(k) != page.get(k): rep.err(where, "package manifest %s %r differs from the page %r" % (k, m.get(k), page.get(k)))
    if m.get("abi_version") != ABI: rep.err(where, "package abi_version is %r, OnAir speaks %d" % (m.get("abi_version"), ABI))
    if m.get("kind") != page.get("kind"): rep.err(where, "package kind %r differs from the page" % m.get("kind"))
    extra = set(m.get("permissions", [])) - set(page.get("permissions", []))
    if extra: rep.err(where, "package asks for permissions the page does not list: %s" % ", ".join(sorted(extra)))
    check_license(m.get("license"), rep, where + " (package manifest)")
    lib = m.get("library")
    key = {"windows": "windows", "macos": "macos", "linux": "linux"}[platform.split("-")[0]]
    libname = lib.get(key) if isinstance(lib, dict) else lib
    if m.get("kind") != "panel-lite":
        if not libname: rep.err(where, "the manifest names no library for %s" % key)
        elif prefix + libname not in names: rep.err(where, "library %s for %s is not in the package" % (libname, platform))
    declared_fs = "filesystem" in page.get("permissions", [])
    for n in names:
        if n.endswith("/"): continue
        blob = z.read(n)
        if not declared_fs and any(s in blob for s in SENSITIVE_PATHS):
            rep.err(where, "%s refers to files outside the plugin's folder (credentials, wallets, system files) without the filesystem permission" % n)
    policy_text(where + " (package manifest)", rep, json.dumps(m))

def lint_page(reg, pid, rep, fetch):
    where = "plugins/%s/plugin.json" % pid
    path = os.path.join(reg, "plugins", pid, "plugin.json")
    page = read_json(path, rep, where)
    if not isinstance(page, dict): return None
    if page.get("schema") != 1: rep.err(where, "schema must be 1")
    if page.get("id") != pid or not ID_RE.match(str(page.get("id", ""))): rep.err(where, "id must equal the folder name and match [a-z0-9][a-z0-9_.-]*")
    for k in ("name", "summary", "version", "author", "kind", "license"):
        if not isinstance(page.get(k), str) or not page[k].strip(): rep.err(where, "%s is required" % k)
    if len(str(page.get("summary", ""))) > 300: rep.err(where, "summary is longer than 300 characters")
    if len(str(page.get("description", ""))) > 4000: rep.err(where, "description is longer than 4000 characters")
    if not VER_RE.match(str(page.get("version", ""))): rep.err(where, "version must be dotted numbers with an optional suffix")
    if page.get("min_onair") and not VER_RE.match(str(page["min_onair"])): rep.err(where, "min_onair must be a version")
    if page.get("kind") not in KINDS: rep.err(where, "kind must be one of %s" % ", ".join(KINDS))
    if page.get("abi_version") != ABI: rep.err(where, "abi_version must be %d" % ABI)
    if page.get("review", "listed") not in REVIEWS: rep.err(where, "review must be one of %s" % ", ".join(REVIEWS))
    for p in page.get("permissions", []):
        if p not in PERMS: rep.err(where, "unknown permission %r" % p)
    for k in ("homepage", "source"):
        v = page.get(k, "")
        if v and not v.startswith("https://"): rep.err(where, "%s must be an https:// address" % k)
    if not page.get("source"): rep.warn(where, "no source URL: 'reviewed' needs readable source")
    for s in page.get("screenshots", []):
        if not (s.startswith("https://") or safe_rel(s)): rep.err(where, "screenshot %r must be https:// or a relative path" % s)
    check_license(page.get("license"), rep, where)
    policy_text(where, rep, page.get("name"), page.get("summary"), page.get("description"), " ".join(page.get("tags", [])))
    dl = page.get("downloads")
    if not isinstance(dl, dict) or not dl: rep.err(where, "downloads is empty"); return page
    for plat, d in dl.items():
        w = "%s [%s]" % (where, plat)
        if plat not in PLATFORMS: rep.err(w, "unknown platform (use %s)" % ", ".join(PLATFORMS)); continue
        url, size, sha = d.get("url"), d.get("size"), str(d.get("sha256", "")).lower()
        if not re.match(r"^[0-9a-f]{64}$", sha): rep.err(w, "sha256 must be 64 hex digits"); continue
        if not isinstance(size, int) or size <= 0 or size > MAX_PACKAGE: rep.err(w, "size must be 1..%d" % MAX_PACKAGE); continue
        data = None
        if isinstance(url, str) and url.startswith("https://"):
            if fetch:
                try:
                    import urllib.request
                    req = urllib.request.Request(url, headers={"User-Agent": "onair-registry-lint"})
                    with urllib.request.urlopen(req, timeout=60) as r: data = r.read(MAX_PACKAGE + 1)
                except Exception as e: rep.err(w, "cannot download %s: %s" % (url, e)); continue
            else: rep.warn(w, "https artefact not downloaded (run lint with --fetch in CI)")
        elif safe_rel(url):
            fp = os.path.join(reg, url)
            if not os.path.isfile(fp): rep.err(w, "file %s does not exist" % url); continue
            with open(fp, "rb") as f: data = f.read()
        else: rep.err(w, "url must be https:// or a relative path"); continue
        if data is not None:
            if len(data) != size: rep.err(w, "size is %d, the page says %d" % (len(data), size))
            if hashlib.sha256(data).hexdigest() != sha: rep.err(w, "sha256 of the artefact does not match the page")
            inspect_zip(data, page, plat, rep, w)
    return page

def cmd_lint(a):
    rep = Report(); reg = a.registry
    pdir = os.path.join(reg, "plugins")
    ids = sorted(d for d in os.listdir(pdir) if os.path.isdir(os.path.join(pdir, d))) if os.path.isdir(pdir) else []
    if not ids: rep.warn("plugins/", "no plugins")
    for pid in ids: lint_page(reg, pid, rep, a.fetch)
    rv = os.path.join(reg, "revoked.json")
    if os.path.exists(rv):
        r = read_json(rv, rep, "revoked.json")
        if isinstance(r, list):
            for x in r:
                if not isinstance(x, dict) or not ID_RE.match(str(x.get("id", ""))) or not x.get("reason"): rep.err("revoked.json", "every entry needs an id and a reason: %r" % x)
        elif r is not None: rep.err("revoked.json", "must be a list")
    for w in rep.warnings: print("warning:", w)
    for e in rep.errors: print("ERROR:", e)
    print("lint: %d plugin(s), %d error(s), %d warning(s)" % (len(ids), len(rep.errors), len(rep.warnings)))
    return 1 if rep.errors else 0

def cmd_index(a):
    rep = Report(); reg = a.registry
    plugins = []
    pdir = os.path.join(reg, "plugins")
    for pid in sorted(os.listdir(pdir)):
        pp = os.path.join(pdir, pid, "plugin.json")
        if not os.path.isfile(pp): continue
        page = read_json(pp, rep, pid)
        if not page: continue
        plugins.append({"id": page["id"], "name": page["name"], "summary": page["summary"], "version": page["version"], "author": page["author"],
                        "license": page["license"], "kind": page["kind"], "min_onair": page.get("min_onair", ""), "abi_version": page["abi_version"],
                        "review": page.get("review", "listed"), "permissions": page.get("permissions", []), "categories": page.get("categories", []),
                        "tags": page.get("tags", []), "platforms": sorted(page["downloads"].keys()),
                        "manifest": {"path": "plugins/%s/plugin.json" % pid, "sha256": sha256_file(pp)}})
    revoked = []
    rv = os.path.join(reg, "revoked.json")
    if os.path.exists(rv): revoked = read_json(rv, rep, "revoked.json") or []
    if rep.errors: print("\n".join(rep.errors)); return 1
    from datetime import datetime, timezone
    gen = a.generated or datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    idx = {"schema": 1, "serial": a.serial, "generated": gen, "registry": a.name, "plugins": plugins, "revoked": revoked}
    text = json.dumps(idx, indent=2, sort_keys=False) + "\n"
    if len(text.encode()) > 1 << 20: print("index.json would exceed 1 MiB"); return 1
    with open(os.path.join(reg, "index.json"), "w", newline="\n") as f: f.write(text)
    print("index.json: serial %d, %d plugin(s), %d revocation(s)" % (a.serial, len(plugins), len(revoked)))
    return 0

def load_seed(path):
    with open(path) as f: s = f.read().strip()
    if not re.match(r"^[0-9a-f]{64}$", s): raise SystemExit("key file must hold 64 hex digits (the 32 byte seed)")
    return bytes.fromhex(s)

def cmd_keygen(a):
    here = os.path.abspath(os.path.dirname(a.keyfile) or ".")
    d = here
    while True:
        if os.path.exists(os.path.join(d, ".git")) and not a.force:
            raise SystemExit("refusing to write the signing key inside a git work tree (%s). Put it on offline media, or pass --force for a throw-away test key." % d)
        n = os.path.dirname(d)
        if n == d: break
        d = n
    if os.path.exists(a.keyfile): raise SystemExit("%s exists" % a.keyfile)
    seed = os.urandom(32)
    fd = os.open(a.keyfile, os.O_WRONLY | os.O_CREAT | os.O_EXCL, stat.S_IRUSR | stat.S_IWUSR)
    with os.fdopen(fd, "w") as f: f.write(seed.hex() + "\n")
    print("private key written to %s (keep it offline; back it up; never commit it)" % a.keyfile)
    print("public key (put it in OnAir's kBuiltInRegistryKeyHex, or in Settings > Plugin store > registry key):")
    print(ed_public(seed).hex())
    return 0

def cmd_sign(a):
    p = os.path.join(a.registry, "index.json")
    with open(p, "rb") as f: data = f.read()
    seed = load_seed(a.keyfile)
    sig = ed_sign(seed, data)
    with open(p + ".sig", "w", newline="\n") as f: f.write(sig.hex() + "\n")
    print("index.json.sig written (public key %s)" % ed_public(seed).hex())
    return 0

def cmd_verify(a):
    p = os.path.join(a.registry, "index.json")
    with open(p, "rb") as f: data = f.read()
    with open(p + ".sig") as f: sig = bytes.fromhex(f.read().strip())
    if not ed_verify(bytes.fromhex(a.pubkey), data, sig): print("signature: BAD"); return 1
    print("signature: ok")
    idx = json.loads(data.decode()); bad = 0
    for e in idx.get("plugins", []):
        fp = os.path.join(a.registry, e["manifest"]["path"])
        if not os.path.isfile(fp) or sha256_file(fp) != e["manifest"]["sha256"]: print("page changed or missing after indexing: %s" % e["id"]); bad += 1
    print("pages: %s" % ("ok" if not bad else "%d mismatch(es): run index and sign again" % bad))
    return 1 if bad else 0

def cmd_artefact(a):
    print(json.dumps({"url": "https://...", "size": os.path.getsize(a.file), "sha256": sha256_file(a.file)}, indent=2))
    return 0

def main():
    if not _selftest(): sys.exit("internal error: the Ed25519 self-test (RFC 8032 test 1) failed")
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0]); sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("keygen"); s.add_argument("keyfile"); s.add_argument("--force", action="store_true"); s.set_defaults(f=cmd_keygen)
    s = sub.add_parser("lint"); s.add_argument("registry"); s.add_argument("--fetch", action="store_true", help="download https artefacts and check them"); s.set_defaults(f=cmd_lint)
    s = sub.add_parser("index"); s.add_argument("registry"); s.add_argument("--serial", type=int, required=True, help="must be higher than the last published index"); s.add_argument("--generated"); s.add_argument("--name", default="OnAir plugins"); s.set_defaults(f=cmd_index)
    s = sub.add_parser("sign"); s.add_argument("registry"); s.add_argument("keyfile"); s.set_defaults(f=cmd_sign)
    s = sub.add_parser("verify"); s.add_argument("registry"); s.add_argument("pubkey"); s.set_defaults(f=cmd_verify)
    s = sub.add_parser("artefact"); s.add_argument("file"); s.set_defaults(f=cmd_artefact)
    a = ap.parse_args(); sys.exit(a.f(a))

if __name__ == "__main__":
    main()
