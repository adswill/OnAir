# SPDX-License-Identifier: GPL-3.0-or-later
"""common/check_versions.py - Ubuntu 26.04's versions are the SDK's minimum versions: check the Windows and macOS sysroots.

    python3 -I check_versions.py <linux/sysroot.lock> <windows/sysroot.lock> [<macos/sysroot-ARCH.lock> | <linux/sysroot-ARCH.lock> ...]

For each downloaded library below (zlib; the libraries built from source have one version for all targets:
sources/sources.lock), the upstream version on Windows (the MSYS2 package), on macOS (the conda-forge package) and on
the other Ubuntu architectures (Raspberry Pi 5: linux/sysroot-arm64.lock's "minimum" lines) must be the same as or
newer than Ubuntu x86-64's (the "minimum" lines of linux/sysroot.lock). Newer is fine. Upstream versions are compared (no epoch, no Debian/MSYS2 package revision, "+dfsg"/"+really" handled), in
dpkg's order. Prints a table; exits 1 when one is older.
"""
import importlib.util
import os
import re
import sys

# library: (Ubuntu package with the minimum version, MSYS2 package name without mingw-w64-ucrt-x86_64-,
#           conda-forge package name)
LIBRARIES = {
    "zlib":     ("zlib1g-dev", "zlib", "libzlib"),
}
# OnAir's other libraries (FFmpeg, GLFW, libusb, OpenSSL, HackRF, SoapySDR ...) are built from source by
# sources/build_sources.sh at the same version for every target (sources/sources.lock), so there is nothing to compare.

here = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("ubuntu_index", os.path.join(here, "..", "linux", "ubuntu_index.py"))
ubuntu_index = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ubuntu_index)


def upstream_deb(v):    # "1:1.3.dfsg+really1.3.1-1ubuntu3.2" -> "1.3.1"; "7:8.0.1-3ubuntu2" -> "8.0.1"
    v = v.split(":", 1)[1] if ":" in v else v
    v = v.rsplit("-", 1)[0] if "-" in v else v
    if "really" in v:
        v = v.split("really", 1)[1]
    return re.split(r"[+~]", v)[0].replace(".dfsg", "")


def upstream_msys2(v):  # "3.6.5-1" -> "3.6.5"; "1~2.4-1" (epoch) -> "2.4"
    v = v.split("~", 1)[1] if "~" in v.split("-")[0] else v
    return v.rsplit("-", 1)[0]


def upstream_conda(v):  # "1!164.3095" -> "164.3095"
    return v.split("!", 1)[1] if "!" in v else v


def main():
    linux, windows = sys.argv[1:3]
    macs = []           # (column name, {package: upstream version}, which key of LIBRARIES' tuple)
    for path in sys.argv[3:]:
        if not os.path.exists(path):
            continue
        arch = re.sub(r"^.*sysroot-(.+)\.lock$", r"\1", path)
        rows = [l.split() for l in open(path)]
        if any(p[:1] == ["minimum"] for p in rows):        # another Ubuntu architecture
            macs.append((f"Ubuntu {arch}", {p[1]: upstream_deb(p[2]) for p in rows if p[:1] == ["minimum"]}, 0))
        else:
            macs.append((f"macOS {arch}", {p[1]: upstream_conda(p[2]) for p in rows if p[:1] == ["pkg"]}, 2))
    minimum, msys2 = {}, {}
    for line in open(linux):
        p = line.split()
        if p[:1] == ["minimum"]:
            minimum[p[1]] = p[2]
    for line in open(windows):
        p = line.split()
        if p[:1] == ["pkg"] and p[1].startswith("mingw-w64-ucrt-x86_64-"):
            msys2[p[1][len("mingw-w64-ucrt-x86_64-"):]] = p[2]
    bad = 0
    print(f"{'library':10} {'Ubuntu 26.04 (minimum)':24} {'Windows':16}" + "".join(f" {m[0]:16}" for m in macs) + " ok")
    for lib, (deb, win, conda) in LIBRARIES.items():
        if deb not in minimum:
            sys.exit(f"error: {linux} has no 'minimum {deb}' line: run linux/fetch_sysroot.sh --update")
        u = upstream_deb(minimum[deb])
        if win in msys2:
            w = upstream_msys2(msys2[win])
        else:
            sys.exit(f"error: {windows} has no package {win} (for {lib})")
        cols, ok = [w], ubuntu_index.vcmp(w, u) >= 0
        for name, pkgs, k in macs:
            key = (deb, win, conda)[k]
            if key not in pkgs:
                sys.exit(f"error: the {name} lock file has no package {key} (for {lib})")
            m = pkgs[key]
            cols.append(m)
            ok = ok and ubuntu_index.vcmp(m, u) >= 0
        bad += not ok
        print(f"{lib:10} {u:24}" + "".join(f" {c:16}" for c in cols) + f" {'yes' if ok else 'NO: older than Ubuntu'}")
    if bad:
        sys.exit(f"error: {bad} librar{'y is' if bad == 1 else 'ies are'} older than Ubuntu 26.04's on another target")


if __name__ == "__main__":
    main()
