#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

"""Decode a library of real recordings with dect2cli and check what comes out (docs/REAL-SIGNALS.md, the hook for CI).

  signal_library.py --cli path/to/dect2cli [--manifest tests/real/signal_library.txt]

The manifest is read first; the library named by the environment variable ONAIR_SIGNAL_LIB adds its own manifest.txt (recordings too big or
not licensed for the repository). A file that is missing is reported and skipped, so a partial library still runs; a wrong result fails.
Exit code: 0 passed, 1 a decode did not give the expected output, 77 nothing to run (ctest SKIP_RETURN_CODE).
"""
import argparse, os, re, subprocess, sys, tempfile


def parse(text, base):
    entries, errors = [], []
    for num, raw in enumerate(text.splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        f = [x.strip() for x in line.split("|")]
        if len(f) != 6:
            errors.append(f"line {num}: expected 6 fields, got {len(f)}")
            continue
        path, fmt, rate, mode, secs, expect = f
        if fmt not in ("cs8", "cu8", "cf32"):
            errors.append(f"line {num}: unknown format {fmt}")
            continue
        try:
            rate = float(rate)
            secs = float(secs)
            re.compile(expect)
        except (ValueError, re.error) as e:
            errors.append(f"line {num}: {e}")
            continue
        entries.append({"file": os.path.join(base, path), "format": fmt, "rate": rate, "mode": mode, "secs": secs, "expect": expect})
    return entries, errors


def run(cli, e, timeout):
    env = dict(os.environ)
    with tempfile.TemporaryDirectory() as d:
        env["DECT2_DATA_DIR"] = d
        cmd = [cli, "--file", e["file"], "--rate", str(e["rate"]), "--format", e["format"], "--standard", e["mode"], "--secs", str(e["secs"])]
        try:
            p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=env, timeout=timeout)
        except subprocess.TimeoutExpired:
            return False, "timed out"
    out = p.stdout.decode("utf-8", "replace")
    if re.search(e["expect"], out):
        return True, ""
    return False, f"exit {p.returncode}, output has no match for /{e['expect']}/"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cli", required=True)
    ap.add_argument("--manifest", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tests", "real", "signal_library.txt"))
    ap.add_argument("--timeout", type=float, default=300)
    a = ap.parse_args()
    entries, errors = [], []
    if os.path.exists(a.manifest):
        with open(a.manifest, encoding="utf-8") as f:
            e, er = parse(f.read(), os.path.dirname(os.path.abspath(a.manifest)))
        entries += e
        errors += [f"{a.manifest}: {x}" for x in er]
    lib = os.environ.get("ONAIR_SIGNAL_LIB", "")
    if lib:
        m = os.path.join(lib, "manifest.txt")
        if os.path.exists(m):
            with open(m, encoding="utf-8") as f:
                e, er = parse(f.read(), lib)
            entries += e
            errors += [f"{m}: {x}" for x in er]
        else:
            print(f"ONAIR_SIGNAL_LIB is set but {m} does not exist")
    for x in errors:
        print("manifest error:", x)
    failed, ran, missing = 0, 0, 0
    for e in entries:
        name = os.path.basename(e["file"])
        if not os.path.exists(e["file"]):
            print(f"MISSING {name}")
            missing += 1
            continue
        ok, why = run(a.cli, e, a.timeout)
        ran += 1
        print(("ok      " if ok else "FAILED  ") + f"{name} ({e['mode']})" + ("" if ok else f": {why}"))
        failed += 0 if ok else 1
    print(f"signal library: {ran} run, {failed} failed, {missing} missing")
    if errors or failed:
        return 1
    return 0 if ran else 77


if __name__ == "__main__":
    sys.exit(main())
