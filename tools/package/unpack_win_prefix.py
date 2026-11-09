#!/usr/bin/env python3
"""Unpacks the Windows dependency prefix (tools/release/upload_win_prefix.sh) and points its pkg-config and CMake files at the new folder.

  unpack_win_prefix.py <tarball> <destination, no spaces>

The prefix was built on the maintainer's Mac: its .pc, .cmake and .la files carry that path. The tarball holds one folder, onair-win-prefix/.
"""
import os, re, shutil, sys, tarfile

TEXT = (".pc", ".cmake", ".la", ".prl")


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    tar, dest = sys.argv[1], os.path.abspath(sys.argv[2])
    if " " in dest:
        sys.exit("the prefix path must have no spaces (the configure scripts of the dependencies cannot cope with them)")
    if os.path.exists(dest):
        shutil.rmtree(dest)
    tmp = dest + ".unpack"
    if os.path.exists(tmp):
        shutil.rmtree(tmp)
    os.makedirs(tmp)
    with tarfile.open(tar) as t:
        t.extractall(tmp, filter="tar")
    src = os.path.join(tmp, "onair-win-prefix")
    if not os.path.isdir(src):
        sys.exit("the tarball has no onair-win-prefix/ folder")
    os.rename(src, dest)
    shutil.rmtree(tmp)
    # the folder the prefix was built under, from any pkg-config file
    old = None
    pc = os.path.join(dest, "lib", "pkgconfig")
    for fn in sorted(os.listdir(pc)) if os.path.isdir(pc) else []:
        with open(os.path.join(pc, fn), encoding="utf8", errors="replace") as f:
            m = re.search(r"^prefix=(\S+)$", f.read(), re.M)
        if m and m.group(1).startswith("/"):
            old = m.group(1)
            break
    n = 0
    if old and old != dest:
        print(f"rewriting {old} -> {dest}")
        for d, _, files in os.walk(dest):
            for fn in files:
                if not fn.endswith(TEXT):
                    continue
                p = os.path.join(d, fn)
                with open(p, encoding="utf8", errors="surrogateescape") as f:
                    s = f.read()
                if old in s:
                    with open(p, "w", encoding="utf8", errors="surrogateescape") as f:
                        f.write(s.replace(old, dest))
                    n += 1
    print(f"prefix in {dest} ({n} files rewritten)")


if __name__ == "__main__":
    main()
