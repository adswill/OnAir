#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

"""Pull dradec.c and dradec.h out of maliwen2015's ffmpeg-7.1.2_cavs_dra.patch (https://github.com/maliwen2015/ffmpeg_cavs_dra).

OnAir does not contain that source. Run this yourself if you want to build the optional DRA+ add-on (docs/CODECS.md, "DRA+ (CDR sound)").
  python3 extract_dra.py [path/to/ffmpeg-7.1.2_cavs_dra.patch] [output folder]
Without a patch path it downloads the patch from GitHub (about 1.4 MB). Output folder defaults to ./dra_src.
"""
import os, sys, urllib.request

URL = "https://raw.githubusercontent.com/maliwen2015/ffmpeg_cavs_dra/main/ffmpeg-7.1.2_cavs_dra.patch"
WANT = {"libavcodec/dradec.c": "dradec.c", "libavcodec/dradec.h": "dradec.h"}

def main():
    args = sys.argv[1:]
    patch = args[0] if args and not os.path.isdir(args[0]) and args[0].endswith(".patch") else None
    out = args[1] if len(args) > 1 else (args[0] if args and patch is None else "dra_src")
    data = open(patch, "rb").read() if patch else urllib.request.urlopen(URL, timeout=60).read()
    lines = data.decode("utf-8", "replace").split("\n")
    files, cur = {}, None
    for ln in lines:
        if ln.startswith("+++ "):
            path = ln[4:].split("\t")[0].strip()
            cur = next((v for k, v in WANT.items() if path.endswith(k)), None)
            if cur: files[cur] = []
        elif ln.startswith("--- ") or ln.startswith("diff "):
            if not ln.startswith("--- "): cur = None
        elif cur and ln.startswith("+"):
            files[cur].append(ln[1:])
        elif cur and ln.startswith(("@@", "\\")):
            continue
        elif cur and ln.startswith((" ", "-")):
            print("this is not a new-file patch for", cur, "(unexpected context line); refusing", file=sys.stderr); sys.exit(1)
    if set(files) != set(WANT.values()):
        print("dradec.c / dradec.h not found in the patch", file=sys.stderr); sys.exit(1)
    files = {n: [l.rstrip("\r") for l in b] for n, b in files.items()}     # the upstream file has Windows line ends
    # Our one change to the upstream decoder (the sound played at twice the speed with bandwidth-extension bits set): the frame info and the
    # channel count keep the header's rate and channels. Done as exact text replacements so a changed upstream refuses instead of misbuilding.
    src = "\n".join(files["dradec.c"]) + "\n"
    a1 = "\tif (pDRADecoder->stHeader.nAuxData)\n\t{\n\t\tpDRAFrameInfo->nChannels *= 2;\n\t\tpDRAFrameInfo->nSampleRate *= 2;\n\t}\n"
    a2 = "\t\t\tif (pDRADecoder->stHeader.nAuxData)\n\t\t\t{\n\t\t\t\tnChannels *= 2;\n\t\t\t}\n"
    if src.count(a1) != 1 or src.count(a2) != 1:
        print("the upstream decoder is not the version this script knows (aux-data rate doubling not found); not patched", file=sys.stderr); sys.exit(1)
    files["dradec.c"] = src.replace(a1, "").replace(a2, "").split("\n")[:-1]
    os.makedirs(out, exist_ok=True)
    for name, body in files.items():
        with open(os.path.join(out, name), "w", newline="") as f: f.write("\n".join(body) + "\n")
    print("wrote", ", ".join(os.path.join(out, n) for n in files))

main()
