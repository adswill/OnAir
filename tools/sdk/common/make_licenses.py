#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""make_licenses.py - the last step of make_sdk.sh: complete the SDK's licence texts and write LICENSES.txt, the list of
every part of the SDK with its version, its licence and the folder that holds its licence text.

  make_licenses.py <sdk folder> <target>[,<target>...]      targets: windows linux rpi5 macos-arm64 macos-x86_64

It reads the lock files in <sdk>/locks, common/licenses.map and, for MSYS2's licences and Ubuntu's source package names,
the downloaded packages in cache/. It adds <sdk>/licenses/common-licenses (the standard texts Ubuntu's copyright files
refer to), the AD9361 driver's licence (no-OS: only in its file headers) and the note for FTDI's FTD3XX.dll.
It stops with an error when a part has no licence text or no licence in licences.map: the SDK is then not ready to share."""
import os
import re
import shutil
import subprocess  # nosec B404
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CACHE = os.environ.get("ONAIR_SDK_CACHE") or os.path.join(REPO, "cache")   # (make_sdk.sh --cache)
TRIPLE = {"windows": "x86_64-w64-mingw32", "linux": "x86_64-linux-gnu", "rpi5": "aarch64-linux-gnu",
          "macos-arm64": "arm64-apple-darwin", "macos-x86_64": "x86_64-apple-darwin"}
TITLE = {"windows": "Windows x64", "linux": "Linux x86-64 (Ubuntu 26.04)", "rpi5": "Linux arm64 (Ubuntu 26.04, Raspberry Pi 5)",
         "macos-arm64": "macOS arm64", "macos-x86_64": "macOS x86_64"}
MSYS2_PFX = "mingw-w64-ucrt-x86_64-"
FTDI_URL = "https://ftdichip.com/driver-licence-terms-details/"

sdk, targets = os.path.abspath(sys.argv[1]), sys.argv[2].split(",")
errors = []


def lock(name):
    """the lines of a lock file in <sdk>/locks, split, without comments"""
    with open(os.path.join(sdk, "locks", name)) as h:
        return [ln.split() for ln in h if ln.strip() and not ln.startswith("#")]


def read_map():
    m = {}
    with open(os.path.join(REPO, "common", "licenses.map")) as h:
        for ln in h:
            if ln.strip() and not ln.startswith("#"):
                kind, name, lic = ln.split(None, 2)
                m[(kind, name)] = lic.strip()
    return m


MAP = read_map()


def mapped(kind, name):
    lic = MAP.get((kind, name))
    if lic is None:
        errors.append(f"{kind} {name}: no licence in common/licenses.map")
        return "?"
    return lic


def text_dir(rel):
    """rel (relative to the SDK) when it holds a licence text, else an error"""
    p = os.path.join(sdk, rel)
    if not (os.path.isfile(p) and os.path.getsize(p)) and not (os.path.isdir(p) and os.listdir(p)):
        errors.append(f"no licence text: {rel}")
    return rel


def run(*cmd):
    # the program by its full path, with this script's own arguments (archive and file names), no shell
    prog = shutil.which(cmd[0])
    if prog is None:
        sys.exit(f"error: {cmd[0]} is not on PATH")
    return subprocess.run([prog, *cmd[1:]], capture_output=True, text=True, check=True).stdout  # nosec B603  # nosemgrep


# --- licence texts that do not come with a package ---------------------------------------------------------------------
common = os.path.join(sdk, "licenses", "common-licenses")
shutil.rmtree(common, ignore_errors=True)
shutil.copytree(os.path.join(REPO, "common", "licenses", "common-licenses"), common)
COMMON = "licenses/common-licenses"


def lib_licenses(target):
    """the folder (relative to the SDK) where build_sources.sh put the libraries' licences"""
    t = TRIPLE[target]
    return f"sysroots/{t}/share/licenses" if target == "windows" else f"sysroots/{t}/usr/local/share/licenses"


def no_os_licence(target):
    """the AD9361 driver of no-OS (built into libbladeRF) states its licence only in its file headers"""
    src = {r[1]: r for r in lock("sources.lock") if r[0] == "src"}["no-os"]
    archive = os.path.join(CACHE, "src", src[3])
    member = next(m for m in run("tar", "-tzf", archive).splitlines() if m.endswith("/ad9361/sw/ad9361.c"))
    lines = run("tar", "-xOzf", archive, member).splitlines(keepends=True)
    end = next(i for i, ln in enumerate(lines) if i > 0 and ln.rstrip().endswith("*/"))   # (the first line has a */ too)
    head = "".join(lines[:end + 1])
    d = os.path.join(sdk, lib_licenses(target), "no-os")
    os.makedirs(d, exist_ok=True)
    with open(os.path.join(d, "LICENSE-ad9361.txt"), "w") as h:
        h.write(f"The licence of no-OS's AD9361 driver (Analog Devices), built into libbladeRF, from the header of {member}:\n\n{head}")


def ftdi_note(target):
    """FTD3XX.dll (FTDI's D3XX library, copied from LimeSuite) comes without a licence file"""
    d = os.path.join(sdk, lib_licenses(target), "ftd3xx")
    os.makedirs(d, exist_ok=True)
    kept = os.path.join(REPO, "common", "licenses", "ftdi")
    if os.path.isdir(kept):
        for f in os.listdir(kept):
            shutil.copy(os.path.join(kept, f), d)
    with open(os.path.join(d, "README.txt"), "w") as h:
        h.write("FTD3XX.dll is FTDI's D3XX driver library (FT60x USB 3.0 chips, used by LimeSDR-USB). It is not open source:\n"
                "it comes as a binary with LimeSuite's source (src/ConnectionFTDI/FTD3XXLibrary), without a licence file.\n"
                f"FTDI's driver licence terms apply: {FTDI_URL}\n"
                "Read them before you share this SDK or a program that ships this DLL.\n")


# --- the rows ------------------------------------------------------------------------------------------------------------
sections = []   # (title, [(name, version, licence, text folder)])
native = "sysroots/x86_64-sdk-linux"
tc = lock("toolchain.lock")
rows = [("LLVM (clang, lld, tools)", next(r[2] for r in tc if r[0] == "llvm"), mapped("tool", "llvm"), text_dir(f"{native}/share/licenses/llvm")),
        ("Rust (rustc, cargo, std)", next(r[2] for r in tc if r[0] == "rust"), mapped("tool", "rust"), text_dir(f"{native}/share/licenses/rust"))]
if os.path.isdir(os.path.join(sdk, native, "share/licenses/binutils-mingw-w64")):
    rows.append(("GNU binutils for mingw-w64", next(r[2] for r in tc if r[0] == "deb"), mapped("tool", "binutils-mingw-w64"),
                 text_dir(f"{native}/share/licenses/binutils-mingw-w64")))
sections.append(("Tools that run on the build machine (Linux x86-64)", rows))

pk = os.path.join(sdk, native, "pkgtools/share/licenses")
if os.path.isdir(pk):
    ver = {r[1]: r[2] for r in lock("pkgtools.lock")}
    ver["bzip2"] = ver.get("libbz2-1.0", "?")
    ver["libdmg-hfsplus"] = ver.get("libdmg-hfsplus", "?")[:12]
    rows = [(n, ver.get(n, "?"), mapped("tool", n), text_dir(f"{native}/pkgtools/share/licenses/{n}")) for n in sorted(os.listdir(pk))]
    sections.append(("Packaging tools (makensis; for macOS: HFS+, dmg, rcodesign)", rows))

for target in targets:
    t = TRIPLE[target]
    rows = []
    if target == "windows":
        manifest = open(os.path.join(sdk, "sysroots", t, "SYSROOT-MANIFEST.txt")).read().split("closure of the ones asked for", 1)[1]
        unpacked = {ln.split()[0] for ln in manifest.splitlines()[1:] if ln.strip()}
        for r in lock("windows-sysroot.lock"):
            if r[0] != "pkg" or r[1] not in unpacked:
                continue
            name, short = r[1], r[1][len(MSYS2_PFX):]
            info = run("tar", "-I", "zstd", "-xOf", os.path.join(CACHE, "msys2", r[3]), ".PKGINFO")
            lic = [v for v in re.findall(r"^license = (.*)$", info, re.M) if not v.startswith("documentation:")]
            lic = " AND ".join(v.removeprefix("spdx:") for v in lic) or "?"
            d = f"sysroots/{t}/share/licenses/{short}"
            if not os.path.isdir(os.path.join(sdk, d)):
                alt = mapped("msys2-dir", short)
                d = "-" if alt == "-" else f"sysroots/{t}/share/licenses/{alt}"
            rows.append((short, r[2], lic, d if d == "-" else text_dir(d)))
        title = f"Target system {TITLE[target]}: MSYS2 UCRT64 packages ({MSYS2_PFX}<name>)"
    elif target in ("linux", "rpi5"):
        for r in lock("linux-sysroot.lock" if target == "linux" else "linux-sysroot-arm64.lock"):
            if r[0] != "deb":
                continue
            src = run("dpkg-deb", "-f", os.path.join(CACHE, "ubuntu", os.path.basename(r[3])), "Source").split()
            src = src[0] if src else r[1]
            rows.append((r[1], r[2], mapped("ubuntu", src), text_dir(f"sysroots/{t}/usr/share/licenses/{r[1]}/copyright")))
        title = f"Target system {TITLE[target]}: Ubuntu packages (standard texts they refer to: {COMMON})"
    else:
        lk = lock(f"macos-sysroot-{target.split('-')[1]}.lock")
        sdkline = next(r for r in lk if r[0] == "sdk")
        rows.append(("Apple macOS SDK", sdkline[1], "Apple SDK licence: not for redistribution", "-"))
        for r in lk:
            if r[0] == "pkg":
                rows.append((r[1], r[2], r[6].replace("_", " "), text_dir(f"sysroots/{t}/usr/local/share/licenses/{r[1]}")))
        title = f"Target system {TITLE[target]}: Apple's SDK and conda-forge packages"
    sections.append((title, rows))

    # the libraries built from source
    rows = []
    lib = lib_licenses(target)
    if os.path.isdir(os.path.join(sdk, lib, "bladerf")):
        no_os_licence(target)
    for r in lock("sources.lock"):
        if r[0] == "src":
            rows.append((r[1], r[2], mapped("src", r[1]), text_dir(f"{lib}/{r[1]}")))
    if target == "windows" and os.path.isfile(os.path.join(sdk, "sysroots", t, "bin", "FTD3XX.dll")):
        ftdi_note(target)
        rows.append(("ftd3xx (FTD3XX.dll)", "LimeSuite's copy", mapped("src", "ftd3xx"), text_dir(f"{lib}/ftd3xx")))
    sections.append((f"Libraries built from source for {TITLE[target]} (sources/sources.lock)", rows))

if errors:
    sys.exit("error: licences missing:\n  " + "\n  ".join(errors))

# --- LICENSES.txt --------------------------------------------------------------------------------------------------------
def split(d):
    """a licence text path: (the .../licenses folder, the rest), so a section can name the folder once"""
    if d == "-":
        return None, "-"
    head, _, rest = d.partition("/licenses/")
    return head + "/licenses/", rest


w = [max(len(r[0]) for _, rows in sections for r in rows), 24,
     max(len(split(r[3])[1]) for _, rows in sections for r in rows)]
out = ["Licences of this SDK (" + ", ".join(TITLE[t] for t in targets) + ")",
       "",
       "This SDK is a collection of separate programs and libraries; each keeps its own licence. The SDK's own scripts are",
       "GPL-3.0-or-later (LICENSE). The licence column is a summary (SPDX); the licence texts are what counts. Columns:",
       "name, version, licence text (in the folder named above each group, relative to this folder), licence.",
       f"Standard licence texts (GPL, LGPL, Apache, MPL ...): {COMMON}/",
       "Sharing this SDK: keep these texts with it and offer the source of the GPL, LGPL and MPL parts (docs/USAGE.md)."]
for title, rows in sections:
    bases = sorted({split(r[3])[0] for r in rows} - {None})
    out += ["", title, "  licence texts in: " + ", ".join(bases)]
    for n, v, lic, d in rows:
        v = v if len(v) <= w[1] else v[:w[1] - 1] + "~"
        out.append(f"  {n:<{w[0]}}  {v:<{w[1]}}  {split(d)[1]:<{w[2]}}  {lic}")
with open(os.path.join(sdk, "LICENSES.txt"), "w") as h:
    h.write("\n".join(out) + "\n")
print(f"==> LICENSES.txt: {sum(len(r) for _, r in sections)} parts, every one with its licence text")
