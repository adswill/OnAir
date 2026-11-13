#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Which Ubuntu package provides which shared library, from the shlibs files of the .deb files a Linux sysroot was made from.
A package made with the SDK takes its Depends line from this: dpkg-shlibdeps asks the build machine's package database, and the sysroot has none.
  make_shlibs.py <sysroot lock file> <folder with the .deb files> > deb-shlibs.txt
One line per library:  soname package minimum-version"""
import os
import re
import subprocess
import sys

lock, cache = sys.argv[1:3]
out = {}
with open(lock) as h:
    for line in h:
        w = line.split()
        if len(w) < 4 or w[0] != "deb":
            continue
        version, path = w[2], os.path.join(cache, os.path.basename(w[3]))
        r = subprocess.run(["dpkg-deb", "-I", path, "shlibs"], capture_output=True, text=True)
        if r.returncode != 0:
            continue
        for s in r.stdout.splitlines():
            f = s.split()
            if len(f) < 3 or f[0].endswith(":"):
                continue
            soname = f"{f[0]}.so.{f[1]}" if re.match(r"^\d", f[1]) else f"{f[0]}-{f[1]}.so"
            out[soname] = (f[2], re.sub(r"-[^-]*$", "", version.split(":")[-1]))
if not out:
    sys.exit("error: no shlibs found: " + lock)
for soname in sorted(out):
    print(soname, *out[soname])
