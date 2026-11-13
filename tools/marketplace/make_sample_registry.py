#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

"""Regenerates tools/marketplace/sample-registry: the three example plugins of sdk/examples as STUB packages (the "library" in each package is a
few bytes of text, not a real plugin), a test index and a signature made with a published TEST key. It exists for the tests and as a template for a
real registry. Never put this registry's key into a real OnAir build. Usage: make_sample_registry.py (from anywhere)."""
import hashlib, io, json, os, sys, zipfile
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import onair_registry as R

TEST_SEED = bytes(range(1, 33))     # public on purpose: a test key
OUT = os.path.join(HERE, "sample-registry")
SDK = os.path.join(HERE, "..", "..", "sdk", "examples")
PLATFORMS = R.PLATFORMS
LIBKEY = {"windows": "windows", "macos": "macos", "linux": "linux"}
INFO = {
    "tone-detector": ("Finds the strongest tone in the tuned channel: offset in Hz, level and a spectrum plot.", ["analysis", "test-tools"], ["tone", "spectrum", "example"]),
    "burst-counter": ("Counts bursts above the noise floor with start time, length and peak; emits plugin.burst events.", ["analysis", "automation"], ["burst", "pulse", "example"]),
    "log-to-file": ("Writes every automation event to events.log in its own data folder.", ["automation"], ["log", "events", "example"]),
}

def zip_bytes(files):
    b = io.BytesIO()
    with zipfile.ZipFile(b, "w", zipfile.ZIP_STORED) as z:
        for name, data in files:
            zi = zipfile.ZipInfo(name, (1980, 1, 1, 0, 0, 0)); zi.compress_type = zipfile.ZIP_STORED; zi.external_attr = 0o644 << 16
            z.writestr(zi, data)
    return b.getvalue()

def main():
    for sub in ("plugins", "packages"): os.makedirs(os.path.join(OUT, sub), exist_ok=True)
    for pid, (summary, cats, tags) in INFO.items():
        man = json.load(open(os.path.join(SDK, pid, "manifest.json")))
        downloads = {}
        for plat in PLATFORMS:
            lib = man["library"][LIBKEY[plat.split("-")[0]]]
            pkg = zip_bytes([("manifest.json", (json.dumps(man, indent=2) + "\n").encode()), (lib, b"STUB: this is not a real plugin library. Test artefact of the sample registry.\n")])
            rel = "packages/%s/%s-%s-%s.onairplugin" % (pid, pid, man["version"], plat)
            os.makedirs(os.path.dirname(os.path.join(OUT, rel)), exist_ok=True)
            open(os.path.join(OUT, rel), "wb").write(pkg)
            downloads[plat] = {"url": rel, "size": len(pkg), "sha256": hashlib.sha256(pkg).hexdigest()}
        page = {"schema": 1, "id": pid, "name": man["name"], "summary": summary,
                "description": "STUB package of the OnAir example plugin '%s' for testing the plugin store. %s Its library is a text file, not code. Source: sdk/examples/%s in the OnAir repository." % (pid, summary, pid),
                "author": man["author"], "license": man["license"], "version": man["version"], "min_onair": man.get("minimum_onair", ""), "abi_version": man["abi_version"],
                "kind": man["kind"], "permissions": man.get("permissions", []), "categories": cats, "tags": tags, "review": "reviewed",
                "homepage": "https://github.com/adswill/OnAir", "source": "https://github.com/adswill/OnAir/tree/main/sdk/examples/" + pid, "screenshots": [], "downloads": downloads}
        os.makedirs(os.path.join(OUT, "plugins", pid), exist_ok=True)
        open(os.path.join(OUT, "plugins", pid, "plugin.json"), "w", newline="\n").write(json.dumps(page, indent=2) + "\n")
    open(os.path.join(OUT, "revoked.json"), "w", newline="\n").write("[]\n")
    class A: pass
    a = A(); a.registry = OUT; a.serial = 1; a.generated = "2026-11-11T00:00:00Z"; a.name = "OnAir sample registry (test only)"
    if R.cmd_index(a): sys.exit(1)
    s = A(); s.registry = OUT; s.keyfile = None
    data = open(os.path.join(OUT, "index.json"), "rb").read()
    open(os.path.join(OUT, "index.json.sig"), "w", newline="\n").write(R.ed_sign(TEST_SEED, data).hex() + "\n")
    open(os.path.join(OUT, "TEST-PUBLIC-KEY.txt"), "w", newline="\n").write(R.ed_public(TEST_SEED).hex() + "\n")
    print("sample registry written to", OUT, "public key", R.ed_public(TEST_SEED).hex())

if __name__ == "__main__":
    main()
