#!/usr/bin/env python3
# Which modes have what in the sidebar layout's lower row and right column (docs/LAYOUT-4.0.md): reads app/ui/mode_table.cpp and
# app/panels/modeui.cpp, finds each mode's ModeUi and classifies its panels() from the code. Usage: tools/layout_survey.py <repo> out.json
import re, os, sys, glob, json, collections
R = sys.argv[1]
mt = open(f"{R}/app/ui/mode_table.cpp").read()
rows = []
for m in re.finditer(r'^\s*\{(\d+), "([^"]+)", (\d+),', mt, re.M):
    rows.append((int(m.group(1)), m.group(2), int(m.group(3))))
mu = open(f"{R}/app/panels/modeui.cpp").read()
body = mu[mu.index("const ModeUi* modeUi(int family) {"):]
body = body[:body.index("\n}\n")]
fam2sym = {}
pending = []
for line in body.split("\n"):
    cases = [int(x) for x in re.findall(r'case (\d+):', line)]
    pending += cases
    r = re.search(r'return &(\w+)', line)
    if r:
        for c in pending: fam2sym[c] = r.group(1)
        pending = []
# find definitions
srcs = [p for p in glob.glob(f"{R}/app/**/*.cpp", recursive=True)]
text = {p: open(p, errors="ignore").read() for p in srcs}
def find_def(sym):
    for p, t in text.items():
        m = re.search(r'(?:^|\n)(?:static )?(?:const )?ModeUi ' + sym + r'\b(\[[^\]]*\])?\s*=', t)
        if m: return p, m.start()
    for p, t in text.items():   # a macro: NAME_UI(sym, ...) -> the #define's body
        m = re.search(r'\n(\w+)\(' + sym + r'\b', t)
        if m:
            d = re.search(r'#define ' + m.group(1) + r'\(', t)
            if d: return p, d.start()
    return None, None
def block(t, i):
    j = t.find("};", i)
    return t[i:j]

def func_body(t, name):
    m = re.search(r'\n(?:template ?<[^>]*>\s*)?(?:static )?void ' + re.escape(name) + r'\(App& ?a?\) ?\{', t)
    if not m: return None
    j = t.find("\n}\n", m.end())
    return t[m.end():j]
def classify(fb, t, depth=0):
    if fb is None: return "unknown"
    s = fb
    if re.search(r'BeginTable', s): return "wide (table)"
    if re.search(r'GetContentRegionAvail\(\)\.x|availW|av\.x|\bW\b ?[\*/-]|ImVec2\(-1|ImVec2\(0, ?[^0)]|panes\(|subNav\(', s): return "wide"
    if re.search(r'GetContentRegionAvail\(\)\.y', s) and re.search(r'scatter\(|ImVec2\(sz, ?sz\)|ImVec2\(side', s): return "small (square plot)"
    if re.search(r'scatter\(|ImVec2\(\d+ ?\* ?gUi', s): return "small (fixed)"
    calls = re.findall(r'^\s*(\w+)\(a\);', s, re.M)
    if calls and depth < 2:
        for c in calls:
            sub = func_body(t, c)
            if sub is not None:
                return classify(sub, t, depth + 1)
    if len(s.strip()) < 3: return "empty"
    return "unknown"
info = {}
for sym in set(fam2sym.values()):
    p, i = find_def(sym)
    if not p: info[sym] = dict(file=None, panels=None, list=None, cls="?"); continue
    t = text[p]
    b = block(t, i)
    pm = re.search(r'\.panels ?= ?&?(\w+)', b) or re.search(r'\.panels ?= ?&?(\w+)', t)
    lm = re.search(r'\.list ?= ?&?(\w+)', b) or re.search(r'\.list ?= ?&?(\w+)', t)
    if not re.search(r'\.panels', b) and re.search(r'ModeUi \w+ ?= ?\{\s*\.', b):   # designated initialiser without .panels: none
        pm = None
    cls = "none"
    if pm:
        cls = classify(func_body(t, pm.group(1)), t)
    if re.search(r'\.list', b) is None and re.search(r'ModeUi \w+ ?= ?\{\s*\.|const ModeUi var = \{', b): lm = None
    sm = re.search(r'\.stream ?= ?true', b) or (re.search(r'\.stream ?= ?true', t) if not re.search(r'const ModeUi var = \{|ModeUi \w+ ?= ?\{\s*\.', b) else None)
    if cls == "unknown" and pm and re.search(r'kvRow|TextDisabled', func_body(t, pm.group(1)) or ""): cls = "text lines"
    info[sym] = dict(file=os.path.relpath(p, R), panels=pm.group(1) if pm else None, list=lm.group(1) if lm else None, stream=bool(sm), cls=cls)
out = []
for fam, name, grp in rows:
    sym = fam2sym.get(fam)
    if fam <= 5: cls, sy, f = "classic (DVB/DAB/FM/ATSC built-in panels)", "-", "-"
    elif sym is None: cls, sy, f = "no ModeUi (generic)", "-", "-"
    else:
        d = info[sym]; cls, sy, f = d["cls"], sym, d["file"]
    I = info.get(sym) or {}
    out.append(dict(fam=fam, name=name, group=grp, sym=sy, file=f, cls=cls, panels=I.get("panels"), list=I.get("list"), stream=I.get("stream")))
json.dump(out, open(sys.argv[2], "w"), indent=0)
c = collections.Counter(o["cls"] for o in out)
print(len(out), "rows"); [print(f"{v:4d}  {k}") for k, v in c.most_common()]

nolist = [o for o in out if o['sym'] not in ('-',) and not o['list']]
print('no list:', len(nolist), 'of which stream:', sum(1 for o in nolist if o['stream']))
print(sorted(set((o['sym'], bool(o['stream'])) for o in nolist)))
