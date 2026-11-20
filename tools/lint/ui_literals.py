#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

"""A ratchet for literal colours in the mode panels.

app/ui/kit/theme_tokens.h says panels use the kit or the tokens, never a literal colour. Most per-mode panels still carry ImVec4(...) and
IM_COL32(...) constants, which ignore the palette, Daylight and High contrast. This check counts them per file (app/**/*_ui.cpp and
app/ui/mode_table.cpp) and fails when a file has more than the baseline in tools/lint/ui_literals_baseline.txt, so the number only goes down.

  python3 tools/lint/ui_literals.py            check against the baseline
  python3 tools/lint/ui_literals.py --update   write the baseline (after removing literals; it refuses to raise any count)
  python3 tools/lint/ui_literals.py --force-update   write the baseline as it is (a new panel that needs a few literals)
"""
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
BASE = ROOT / "tools" / "lint" / "ui_literals_baseline.txt"
PATTERN = re.compile(r"\b(?:ImVec4|IM_COL32)\s*\(")


def counts():
    out = {}
    files = sorted(list((ROOT / "app").rglob("*_ui.cpp")) + [ROOT / "app" / "ui" / "mode_table.cpp"])
    for f in files:
        if not f.exists():
            continue
        n = len(PATTERN.findall(f.read_text(encoding="utf-8", errors="replace")))
        if n:
            out[f.relative_to(ROOT).as_posix()] = n
    return out


def read_base():
    out = {}
    if BASE.exists():
        for line in BASE.read_text().splitlines():
            if line.strip() and not line.startswith("#"):
                n, name = line.split(None, 1)
                out[name.strip()] = int(n)
    return out


def write_base(c):
    lines = ["# literal colour constructors per file (tools/lint/ui_literals.py); lower numbers only"]
    lines += ["%d %s" % (n, name) for name, n in sorted(c.items())]
    BASE.write_text("\n".join(lines) + "\n")


def main(argv):
    now, base = counts(), read_base()
    if "--update" in argv or "--force-update" in argv:
        if "--update" in argv:
            worse = [k for k, n in now.items() if n > base.get(k, 0)]
            if worse:
                print("refusing to raise the baseline for: " + ", ".join(worse) + " (--force-update to accept)")
                return 1
        write_base(now)
        print("baseline written: %d files, %d literals" % (len(now), sum(now.values())))
        return 0
    over = [(k, n, base.get(k, 0)) for k, n in now.items() if n > base.get(k, 0)]
    for k, n, b in over:
        print("%s: %d literal colours (baseline %d): use kit::tokens() roles or pal::* instead of ImVec4()/IM_COL32()" % (k, n, b))
    print("literal colours in panels: %d (baseline %d)" % (sum(now.values()), sum(base.values())))
    return 1 if over else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
