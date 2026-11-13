# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

"""Family numbers of app/ui/mode_table.cpp rows.

A row's family is a number ({146, "KLW", ...}) or a constant from a core/include/dect2/*/*_modes.h header
({dect2::railway::kKlwFamily, "KLW", ...}). The generators share this module so every row counts.
"""
import os
import re

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# family token at the start of a row: a number, or a (namespaced) constant, optionally +/- a number
TOKEN = r"(?:\d+|[A-Za-z_][\w:]*(?:\s*[+-]\s*\d+)?)"
ROW_RE = re.compile(r"\s*\{\s*(" + TOKEN + r")\s*,\s*\"")

_consts = None


def constants(root=ROOT):
    """{name: value} for 'constexpr int NAME = N' (or NAME + N) in the *_modes.h headers, by full and short name."""
    global _consts
    if _consts is not None:
        return _consts
    out, short = {}, {}
    base = os.path.join(root, "core", "include", "dect2")
    for d, _, files in os.walk(base):
        for fn in sorted(files):
            if not fn.endswith("_modes.h"):
                continue
            text = open(os.path.join(d, fn), encoding="utf-8", errors="replace").read()
            ns = re.search(r"namespace\s+([\w:]+)\s*\{", text)
            ns = ns.group(1) if ns else ""
            for m in re.finditer(r"constexpr\s+int\s+(\w+)\s*=\s*([^;]+);", text):
                v = _eval(m.group(2), out, ns)
                if v is None:
                    continue
                out[(ns + "::" if ns else "") + m.group(1)] = v
                short.setdefault(m.group(1), []).append(v)
    for k, vs in short.items():
        if len(set(vs)) == 1:
            out.setdefault(k, vs[0])
    _consts = out
    return out


def _eval(expr, known, ns=""):
    m = re.fullmatch(r"\s*([\w:]+)\s*(?:([+-])\s*(\d+))?\s*", expr)
    if not m:
        return None
    a = m.group(1)
    if a.isdigit():
        v = int(a)
    else:
        v = known.get(a, known.get((ns + "::" if ns else "") + a))
        if v is None:
            return None
    if m.group(2):
        v = v + int(m.group(3)) if m.group(2) == "+" else v - int(m.group(3))
    return v


def family(token, root=ROOT):
    """Number of a row's family token; raises ValueError when it cannot be read."""
    v = _eval(token, constants(root))
    if v is None:
        raise ValueError("cannot read the family %r of a mode_table.cpp row" % token)
    return v
