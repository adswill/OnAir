#!/usr/bin/env python3
"""Reads the desktop mode table (app/ui/mode_table.cpp kBuiltinModes + kGroupNames + kGroupCols) and writes Modes.kt and res/values/modes.xml.
A mode's index on Android is its position in tools/mode_order.txt: saved settings, favourites and hidden modes store that index, so it never
changes: new modes are appended, removed ones stay as hidden placeholders. The order on screen follows the desktop table (ModeDef.order).
The script runs from tools/build_native.sh, so a mode added to the desktop table reaches the app with no Android change.
Usage: gen_modes.py [<OnAir repo> <android project>] (default: this checkout and its android/ folder)"""
import re, sys, html, os
_android = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
repo, proj = (sys.argv[1], sys.argv[2]) if len(sys.argv) > 2 else (os.path.dirname(_android), _android)
src = open(os.path.join(repo, 'app/ui/mode_table.cpp'), encoding='utf-8').read()
start = src.index('kBuiltinModes[] = {')
end = src.index('\n};', start)
body = src[start:end]

def split_fields(line):
    # split the top-level commas of "{...}" while honouring strings and parentheses
    line = line.strip()
    assert line.startswith('{') and line.rstrip(',').endswith('}'), line[:60]
    inner = line.rstrip(',')[1:-1]
    out, cur, depth, instr, esc = [], '', 0, False, False
    for ch in inner:
        if instr:
            cur += ch
            if esc: esc = False
            elif ch == '\\': esc = True
            elif ch == '"': instr = False
            continue
        if ch == '"': instr = True; cur += ch; continue
        if ch in '([{': depth += 1
        if ch in ')]}': depth -= 1
        if ch == ',' and depth == 0: out.append(cur.strip()); cur = ''; continue
        cur += ch
    out.append(cur.strip())
    return out

def cstr(s):
    s = s.strip()
    m = re.fullmatch(r'(?:N_|tr)?\(?\s*"((?:[^"\\]|\\.)*)"\s*\)?', s)
    if m is None: return None
    return bytes(m.group(1), 'utf-8').decode('unicode_escape').encode('latin-1', 'ignore').decode('utf-8', 'ignore') if '\\' in m.group(1) else m.group(1)

def vec4(s):
    nums = re.findall(r'-?\d+\.?\d*f?', s)
    r, g, b = [max(0, min(255, round(float(x.rstrip('f')) * 255))) for x in nums[:3]]
    return 0xFF000000 | (r << 16) | (g << 8) | b

modes = []
buf = ''
for line in body.splitlines():
    if not buf and not re.match(r'\s*\{\d+,', line): continue
    buf += line + '\n'
    st = buf.strip()
    if st.endswith('},') and st.count('{') == st.count('}'):
        f = split_fields(buf)
        col = f[3]
        f += ['""'] * (8 - len(f))
        if f[7] in ('""', ''): f[7] = 'ImVec4(0.7f, 0.74f, 0.8f, 1)'
        modes.append(dict(family=int(f[0]), name=cstr(f[1]), group=int(f[2]), blurb=cstr(f[4]) or '', tip=cstr(f[5]) or '', sub=cstr(f[6]) or '', accent=vec4(f[7]),
                          subId=(cstr(f[8]) if len(f) > 8 and f[8] != 'nullptr' else None), keys=(cstr(f[9]) or '' if len(f) > 9 else '')))
        buf = ''
gm = re.search(r'kGroupNames\[kNumGroups\]\s*=\s*\{(.*?)\};', src, re.S)
groups = re.findall(r'"((?:[^"\\]|\\.)*)"', gm.group(1))
cm = re.search(r'kGroupCols\[kNumGroups\]\s*=\s*\{(.*?)\};', src, re.S)
cols = [(int(a) << 16) | (int(b) << 8) | int(c) | 0xFF000000 for a, b, c in re.findall(r'IM_COL32\((\d+),\s*(\d+),\s*(\d+),\s*255\)', cm.group(1))]
assert len(groups) == len(cols), (len(groups), len(cols))
print(len(modes), 'modes', len(groups), 'groups')

# Why a mode does less on a phone (ModeDef.note; the texts are R.string.mode_note_*, see ModeNotes.kt). Families of the desktop table:
NOTE_DESKTOP, NOTE_PICTURE, NOTE_SOUND, NOTE_VOICE, NOTE_IMAGE, NOTE_AUTO = 1, 2, 3, 4, 5, 6
FAMILY_NOTE = {999: NOTE_DESKTOP, 19999: NOTE_DESKTOP, 209: NOTE_DESKTOP}
for f in (0, 1, 3, 4, 6, 7, 40, 154, 8, 47, 101): FAMILY_NOTE[f] = NOTE_PICTURE       # TV: FFmpeg is not in the Android build, analogue pictures have no panel
for f in (2, 10, 21, 22): FAMILY_NOTE[f] = NOTE_SOUND                               # DAB+, DRM, HD Radio, CDR: their audio codecs are not in the build
for f in (9, 38, 50, 180, 45, 46, 79, 31, 33, 128): FAMILY_NOTE[f] = NOTE_VOICE      # digital voice: user-installed codec plug-ins (not available on Android)
for f in (15, 59, 60, 80, 230, 231, 232, 233, 234, 64, 235, 236, 206): FAMILY_NOTE[f] = NOTE_IMAGE   # pictures are decoded, the phone has no picture panel yet

def note_of(m):
    if m['subId'] and m['subId'].startswith('auto:'): return NOTE_AUTO     # desktop-only detector screen: on Android it is that family's receiver
    if m['subId'] == 'sstv': return NOTE_IMAGE
    return FAMILY_NOTE.get(m['family'], 0)

# ---- the stable index: tools/mode_order.txt, one "family|subId" per line
key_of = lambda m: '%d|%s' % (m['family'], m['subId'] or '')
order_path = os.path.join(proj, 'tools/mode_order.txt')
order = [l.strip() for l in open(order_path, encoding='utf-8') if l.strip()] if os.path.exists(order_path) else []
assert len(set(order)) == len(order), 'duplicate key in mode_order.txt'
known = set(order)
for m in modes:
    if key_of(m) not in known:
        order.append(key_of(m)); known.add(key_of(m))
with open(order_path, 'w', encoding='utf-8') as o: o.write('\n'.join(order) + '\n')
by_key = {}
for pos, m in enumerate(modes):
    m['order'] = pos
    assert key_of(m) not in by_key, 'two rows of the table have the key ' + key_of(m)
    by_key[key_of(m)] = m
removed = dict(family=-1, name='', group=0, blurb='', tip='', sub='', accent=0xFF808080, subId=None, keys='', order=-1)
rows = [by_key.get(k, removed) for k in order]
print(len(modes), 'modes,', len(rows), 'rows on Android', len(groups), 'groups')

def xml(s): return html.escape(s or '', quote=False).replace("'", "\\'").replace('"', '\\"').replace('@', '\\@').replace('?', '\\?')
with open(os.path.join(proj, 'app/src/main/res/values/modes.xml'), 'w', encoding='utf-8') as o:
    o.write('<?xml version="1.0" encoding="utf-8"?>\n<!-- Generated by tools/gen_modes.py from the desktop mode table (app/ui/mode_table.cpp). Do not edit. -->\n<resources>\n')
    o.write('    <string-array name="mode_groups">\n' + ''.join(f'        <item>{xml(g)}</item>\n' for g in groups) + '    </string-array>\n')
    o.write('    <string-array name="mode_names">\n' + ''.join(f'        <item>{xml(m["name"])}</item>\n' for m in rows) + '    </string-array>\n')
    o.write('    <string-array name="mode_blurbs">\n' + ''.join(f'        <item>{xml(m["blurb"])}</item>\n' for m in rows) + '    </string-array>\n')
    o.write('    <string-array name="mode_subs">\n' + ''.join(f'        <item>{xml(m["sub"])}</item>\n' for m in rows) + '    </string-array>\n')
    o.write('    <string-array name="mode_keys">\n' + ''.join(f'        <item>{xml(m["keys"])}</item>\n' for m in rows) + '    </string-array>\n')
    o.write('</resources>\n')
def kstr(v): return 'null' if v is None else '"' + v.replace('\\', '\\\\').replace('"', '\\"').replace('$', '\\$') + '"'
with open(os.path.join(proj, 'app/src/main/java/io/github/adswill/onair/Modes.kt'), 'w', encoding='utf-8') as o:
    o.write('// Generated by tools/gen_modes.py from the desktop mode table (app/ui/mode_table.cpp). Do not edit.\npackage io.github.adswill.onair\n\n')
    o.write('/**\n * One row of the desktop table. [index] is stable (tools/mode_order.txt) and is what the settings store; [order] is the place in the list\n')
    o.write(' * (-1: a mode that no longer exists, never shown). [family] is the number the desktop stores; [subId] picks a protocol of a shared receiver;\n')
    o.write(' * [note]: why the mode does less on a phone (ModeNotes.kt), 0 = nothing to say.\n */\n')
    o.write('class ModeDef(val index: Int, val family: Int, val group: Int, val accent: Long, val subId: String?, val order: Int, val note: Int) {\n    val removed: Boolean get() = order < 0\n}\n\n')
    o.write('object Modes {\n    val groupColors = longArrayOf(' + ', '.join(f'0x{c:08X}' for c in cols) + ')\n    const val GROUPS = %d\n' % len(groups))
    o.write('    /** Every mode, by [ModeDef.index]. Lists shown to the user go through [shown]. */\n')
    o.write('    val all: List<ModeDef> = listOf(\n')
    for i, m in enumerate(rows):
        o.write(f'        ModeDef({i}, {m["family"]}, {m["group"]}, 0x{m["accent"]:08X}, {kstr(m["subId"])}, {m["order"]}, {note_of(m) if m["order"] >= 0 else 0}),\n')
    o.write('    )\n')
    o.write('    /** The modes that exist, in the order of the desktop table. */\n    val shown: List<ModeDef> = all.filter { !it.removed }.sortedBy { it.order }\n}\n')
