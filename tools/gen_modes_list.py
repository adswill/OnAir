#!/usr/bin/env python3
# Writes MODES.md: every mode in the mode list (app/ui/mode_table.cpp), grouped as the app groups them, with its one-line summary.
# One place to answer "do we support X?" without searching the code. The quick gate runs it with --check, so the file can't go stale.
#   python3 tools/gen_modes_list.py           rewrite MODES.md
#   python3 tools/gen_modes_list.py --check   exit 1 if MODES.md is out of date
import os, re, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mode_families  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TABLE = os.path.join(ROOT, "app", "ui", "mode_table.cpp")
OUT = os.path.join(ROOT, "MODES.md")


def strings(s):
    """The C string literals in one table row, in order (handles \\" and adjacent pieces)."""
    out = []
    for m in re.finditer(r'"((?:[^"\\]|\\.)*)"', s):
        out.append(m.group(1).replace('\\"', '"').replace("\\n", " "))
    return out


def load():
    src = open(TABLE, encoding="utf-8").read()
    g = re.search(r"kGroupNames\[kNumGroups\]\s*=\s*\{(.*?)\};", src, re.S)
    groups = [x.strip() for x in strings(g.group(1))] if g else []
    rows = []
    for line in src.splitlines():
        m = mode_families.ROW_RE.match(line)
        if not m:
            continue
        st = strings(line)
        if len(st) < 2:
            continue
        gm = re.match(r"\s*\{\s*" + mode_families.TOKEN + r"\s*,\s*\"(?:[^\"\\]|\\.)*\",\s*(\d+)\s*,", line)
        grp = int(gm.group(1)) if gm else -1
        name = st[0]
        # row layout: name, long description, short summary, tag line, ...; prefer the short summary
        summary = st[2] if len(st) > 2 and st[2] else st[1]
        rows.append((grp, name, summary.strip(), mode_families.family(m.group(1))))
    return groups, rows


KEEP = {"TV": "TV", "HF": "HF", "IOT": "IoT", "&": "&", "-": "-"}


def nice(title):
    """'IoT & SENSORS' -> 'IoT & Sensors', 'HF UTILITY' -> 'HF Utility', keeping the abbreviations."""
    return " ".join(KEEP.get(w.upper(), w if any(c.islower() for c in w[1:]) else w.capitalize()) for w in title.split())


def render(groups, rows):
    lines = ["# OnAir modes", "",
             "Every mode in OnAir's mode list, grouped as in the app. Generated from `app/ui/mode_table.cpp` by "
             "`tools/gen_modes_list.py`; don't edit by hand. The quick gate fails when this file is out of date.", "",
             "**%d modes.**" % len(rows), ""]
    for gi in sorted({r[0] for r in rows}):
        title = groups[gi] if 0 <= gi < len(groups) else "Other"
        members = [r for r in rows if r[0] == gi]
        lines += ["## %s (%d)" % (nice(title), len(members)), ""]
        for _, name, summary, fam in members:
            lines.append("- **%s** - %s" % (name, summary) if summary else "- **%s**" % name)
        lines.append("")
    return "\n".join(lines)


def main():
    groups, rows = load()
    text = render(groups, rows)
    if "--check" in sys.argv:
        cur = open(OUT, encoding="utf-8").read() if os.path.exists(OUT) else ""
        if cur != text:
            print("MODES.md is out of date: run python3 tools/gen_modes_list.py")
            return 1
        print("MODES.md: %d modes, up to date" % len(rows))
        return 0
    open(OUT, "w", encoding="utf-8").write(text)
    print("wrote MODES.md: %d modes" % len(rows))
    return 0


if __name__ == "__main__":
    sys.exit(main())
