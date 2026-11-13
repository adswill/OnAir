#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

"""Token-based clone detector for core/ and app/ (C and C++).

Tokens are normalised (identifiers -> I, numbers -> N, strings -> S, characters -> C, keywords and punctuation kept) so renamed copies still match. Every window of
--window tokens (default 60) is hashed; windows that occur at two places are clone windows, overlapping clone windows are merged into blocks, and blocks that share a
window are grouped into clusters. Prints the duplicated line figure and the biggest clusters. No dependencies, nothing is downloaded.

  python3 tools/clones.py                      # core/ and app/, top 30 clusters
  python3 tools/clones.py --top 50 --window 80
  python3 tools/clones.py core/src/dab         # only that folder
  python3 tools/clones.py --json clones.json   # the full cluster list, machine readable

Generated and data files are skipped (see SKIP). The figure printed first is "duplicated lines": lines that belong to a clone block at two or more places, as a
share of all code lines scanned; "excess" counts only the copies after the first one in each cluster (what de-duplication could remove).
"""
import argparse, json, os, re, sys
from collections import defaultdict

SKIP_DIRS = {"third_party", "i18n", "build", "node_modules", ".git"}
SKIP_FILES = {"credits_data.h", "mesh_presets_data.cpp"}
EXT = (".cpp", ".cc", ".c", ".h", ".hpp", ".mm")
KEYWORDS = set("""alignas alignof and asm auto bool break case catch char class const constexpr const_cast continue decltype default delete do double dynamic_cast else enum explicit
extern false final float for friend goto if inline int long mutable namespace new noexcept nullptr operator override private protected public register reinterpret_cast return
short signed sizeof static static_assert static_cast struct switch template this thread_local throw true try typedef typeid typename union unsigned using virtual void volatile
while int8_t int16_t int32_t int64_t uint8_t uint16_t uint32_t uint64_t size_t std""".split())

TOKEN = re.compile(r"""
    (?P<ws>\s+)
  | (?P<lcom>//[^\n]*)
  | (?P<bcom>/\*.*?\*/)
  | (?P<pre>^[ \t]*\#[ \t]*include[^\n]*)
  | (?P<str>"(?:\\.|[^"\\\n])*")
  | (?P<chr>'(?:\\.|[^'\\\n])+')
  | (?P<num>0[xX][0-9a-fA-F']+[uUlL]*|\d[\d']*(?:\.\d*)?(?:[eE][+-]?\d+)?[fFuUlL]*|\.\d+(?:[eE][+-]?\d+)?[fF]?)
  | (?P<id>[A-Za-z_]\w*)
  | (?P<op>->\*?|\+\+|--|<<=?|>>=?|<=>|&&|\|\||::|[-+*/%&|^!<>=]=|\.\.\.|[{}()\[\];,.<>?:~!%^&*+=|/-])
""", re.X | re.S | re.M)


def tokenize(text):
    """Returns (tokens, lines): normalised token strings and the 1-based line of each."""
    toks, lines = [], []
    pos, line, n = 0, 1, len(text)
    while pos < n:
        m = TOKEN.match(text, pos)
        if not m:
            pos += 1
            continue
        kind = m.lastgroup
        t = m.group()
        if kind == "id":
            toks.append(t if t in KEYWORDS else "I"); lines.append(line)
        elif kind == "num":
            toks.append("N"); lines.append(line)
        elif kind == "str":
            toks.append("S"); lines.append(line)
        elif kind == "chr":
            toks.append("C"); lines.append(line)
        elif kind == "op":
            toks.append(t); lines.append(line)
        line += t.count("\n")
        pos = m.end()
    return toks, lines


def collect(paths):
    out = []
    for base in paths:
        if os.path.isfile(base):
            out.append(base); continue
        for root, dirs, files in os.walk(base):
            dirs[:] = sorted(d for d in dirs if d not in SKIP_DIRS)
            for f in sorted(files):
                if f.endswith(EXT) and f not in SKIP_FILES:
                    out.append(os.path.join(root, f))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("paths", nargs="*", default=["core", "app"])
    ap.add_argument("--window", type=int, default=60, help="minimum clone length in tokens (default 60)")
    ap.add_argument("--top", type=int, default=30, help="clusters to print (default 30)")
    ap.add_argument("--json", help="write every cluster to this file")
    args = ap.parse_args()
    W = args.window

    files = collect(args.paths)
    toks, lns, code_lines = {}, {}, 0
    for f in files:
        try:
            text = open(f, errors="replace").read()
        except OSError:
            continue
        t, l = tokenize(text)
        toks[f], lns[f] = t, l
        code_lines += len(set(l))

    MASK = (1 << 61) - 1
    BASE = 1000003
    top_pow = pow(BASE, W - 1, MASK)
    ids = {}
    def tid(s):
        v = ids.get(s)
        if v is None:
            v = len(ids) + 1; ids[s] = v
        return v

    # pass 1: count every window hash; pass 2: keep the places of the hashes that occur twice or more (keeps the memory of the big dictionary small)
    def windows(f):
        t = toks.get(f)
        if not t or len(t) < W:
            return
        v = [tid(x) for x in t]
        h = 0
        for i in range(W):
            h = (h * BASE + v[i]) & MASK
        yield 0, h
        for i in range(1, len(v) - W + 1):
            h = ((h - v[i - 1] * top_pow) * BASE + v[i + W - 1]) & MASK
            yield i, h

    counts = {}
    for f in files:
        for _, h in windows(f):
            counts[h] = counts.get(h, 0) + 1
    twice = {h for h, c in counts.items() if c > 1}
    del counts
    groups = defaultdict(list)          # window hash -> [(file, start token)]
    for f in files:
        for i, h in windows(f):
            if h in twice:
                groups[h].append((f, i))
    del twice

    # clone windows: occurring at two or more places; a window that only repeats inside its own file at overlapping places (a run of the same token) is no copy
    rep = defaultdict(list)             # file -> [(start, hash)] of clone windows
    multi = {}
    for h, occ in groups.items():
        if len({f for f, _ in occ}) < 2:
            starts = sorted(i for _, i in occ)
            if not any(b - a >= W for a, b in zip(starts, starts[1:])):
                continue
        multi[h] = occ
        for f, i in occ:
            rep[f].append((i, h))
    del groups

    # blocks: maximal runs of consecutive clone windows per file
    blocks = []                         # (file, first token, last token inclusive, first hash)
    for f, lst in rep.items():
        lst.sort()
        run_start = prev = None
        first_h = None
        for i, h in lst:
            if run_start is None:
                run_start, prev, first_h = i, i, h
            elif i == prev + 1:
                prev = i
            else:
                blocks.append((f, run_start, prev + W - 1, first_h))
                run_start, prev, first_h = i, i, h
        if run_start is not None:
            blocks.append((f, run_start, prev + W - 1, first_h))

    # union blocks that share a window
    parent = list(range(len(blocks)))
    def find(x):
        while parent[x] != x:
            parent[x] = parent[parent[x]]; x = parent[x]
        return x
    by_file = defaultdict(list)
    for bi, (f, a, b, _) in enumerate(blocks):
        by_file[f].append((a, b, bi))
    for f in by_file:
        by_file[f].sort()
    def block_at(f, pos):
        lst = by_file[f]
        lo, hi = 0, len(lst) - 1
        while lo <= hi:
            mid = (lo + hi) // 2
            a, b, bi = lst[mid]
            if pos < a: hi = mid - 1
            elif pos > b: lo = mid + 1
            else: return bi
        return None
    for h, occ in multi.items():
        first = None
        for f, i in occ:
            bi = block_at(f, i)
            if bi is None:
                continue
            if first is None: first = bi
            else:
                ra, rb = find(first), find(bi)
                if ra != rb: parent[rb] = ra

    clusters = defaultdict(list)
    for bi in range(len(blocks)):
        clusters[find(bi)].append(blocks[bi])

    # duplicated lines (all copies) and excess lines (all but the largest copy of every cluster)
    dup_lines, excess_lines = set(), 0
    result = []
    for members in clusters.values():
        if len(members) < 2:
            continue
        spans = []
        for f, a, b, _ in members:
            l = lns[f]
            lo, hi = l[a], l[min(b, len(l) - 1)]
            spans.append((f, lo, hi, b - a + 1))
            for x in range(lo, hi + 1):
                dup_lines.add((f, x))
        spans.sort(key=lambda s: -(s[2] - s[1]))
        excess = sum(s[2] - s[1] + 1 for s in spans[1:])
        excess_lines += excess
        result.append({"copies": len(spans), "tokens": max(s[3] for s in spans), "lines": spans[0][2] - spans[0][1] + 1, "excess_lines": excess,
                       "where": [{"file": s[0], "from": s[1], "to": s[2]} for s in sorted(spans)]})
    result.sort(key=lambda c: -c["excess_lines"])

    pct = 100.0 * len(dup_lines) / max(1, code_lines)
    print("files scanned: %d, code lines: %d, window: %d tokens" % (len(files), code_lines, W))
    print("duplicated lines: %d (%.1f%%), excess over one copy: %d (%.1f%%), clusters: %d" % (len(dup_lines), pct, excess_lines, 100.0 * excess_lines / max(1, code_lines), len(result)))
    for k, c in enumerate(result[: args.top], 1):
        print("\n#%d  %d copies x ~%d lines (%d tokens), excess %d lines" % (k, c["copies"], c["lines"], c["tokens"], c["excess_lines"]))
        for w in c["where"][:8]:
            print("    %s:%d-%d" % (w["file"], w["from"], w["to"]))
        if len(c["where"]) > 8:
            print("    ... and %d more" % (len(c["where"]) - 8))
    if args.json:
        json.dump({"code_lines": code_lines, "duplicated_lines": len(dup_lines), "excess_lines": excess_lines, "clusters": result}, open(args.json, "w"), indent=1)
    return 0


if __name__ == "__main__":
    sys.exit(main())
