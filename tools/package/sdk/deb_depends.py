#!/usr/bin/env python3
"""The Depends line of a package made with the cross SDK. dpkg-shlibdeps cannot do this: it asks the build machine's package database,
and the sysroot has none. Reads NEEDED of every ELF file in the stage, leaves out what the package carries itself, and finds each
library's package in the SDK's deb-shlibs.txt. A library no package provides stops the build.
  deb_depends.py <stage> <SDK's deb-shlibs.txt> [extra dependency ...]"""
import os, re, subprocess, sys

stage, shlibs = sys.argv[1:3]
extra = sys.argv[3:]

def elf_files():
    for root, _, files in os.walk(stage):
        for f in files:
            p = os.path.join(root, f)
            if os.path.islink(p) or not os.path.isfile(p):
                continue
            with open(p, "rb") as h:
                if h.read(4) == b"\x7fELF":
                    yield p

carried, needed = set(), set()
for root, _, files in os.walk(stage):
    carried.update(files)
for p in elf_files():
    out = subprocess.run(["llvm-readobj", "--needed-libs", p], check=True, capture_output=True, text=True).stdout
    m = re.search(r"NeededLibraries \[(.*?)\]", out, re.S)
    if m:
        needed.update(l.strip() for l in m.group(1).splitlines() if l.strip())
needed -= carried
needed = {n for n in needed if not n.startswith("ld-linux")}   # (the loader comes with libc6)

provides = {}   # soname -> (package, minimum version)
with open(shlibs) as h:
    for line in h:
        soname, package, version = line.split()
        provides[soname] = (package, version)

missing = sorted(n for n in needed if n not in provides)
if missing:
    sys.exit("error: no package in the lock file provides: " + " ".join(missing))
deps = {f"{provides[n][0]} (>= {provides[n][1]})" for n in needed}
print(", ".join(sorted(deps | set(extra))))
