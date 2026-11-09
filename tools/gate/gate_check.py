#!/usr/bin/env python3
"""Source-only checks of the quick gate (tools/gate/run.sh). No compiler, no build: seconds, on every push.

Each check is a Python reading of a C++ test that needs the whole receiver library to link, or of a link error found late:
  modes        family and poll-slot clashes (tests/app/test_mode_numbers.cpp, sections 1 and 3)
  web_policy   every mode id of the registry is classified in server/web/web_policy.cpp (tests/server/test_web_safety.cpp, section 4)
  android      an FFmpeg, libusb or HackRF call new in the core has a stand-in in android/native/src/stubs_ext.c (the Android link); calls in code
               the link drops are listed in tools/gate/android_unreached.txt

The C++ tests stay the reference: they run on the CI shards and in the same way against every commit. The gate only fails earlier.

  tools/gate/gate_check.py [--root DIR] [modes web_policy android]
"""
import argparse, os, re, sys


def read(root, rel):
    try:
        with open(os.path.join(root, rel), encoding="utf8", errors="replace") as f:
            return f.read()
    except OSError:
        return ""


# ---- mode numbers ----------------------------------------------------------------------------------------------------------------

def scan_constants(text, const):
    for m in re.finditer(r"constexpr\s+int\s+(k[A-Za-z0-9_]+)\s*=\s*([^;]+);", text):
        const[m.group(1)] = m.group(2)


def eval_expr(e, const, depth=0):
    """the evalExpr of the test: a number, a named constant, a + b, a - b; None when it cannot be read"""
    if depth > 8:
        return None
    e = e.strip(" \t")
    if "::" in e:
        e = e[e.rfind("::") + 2:]
    op = max(e.rfind("+"), e.rfind("-"))
    if op > 0:
        a, b = eval_expr(e[:op], const, depth + 1), eval_expr(e[op + 1:], const, depth + 1)
        if a is None or b is None:
            return None
        return a + b if e[op] == "+" else a - b
    if e and e[0].isdigit():
        return int(re.match(r"\d+", e).group(0))
    return eval_expr(const[e], const, depth + 1) if e in const else None


def number_after(text, key):
    q = text.find(key)
    if q < 0:
        return -1
    m = re.match(r"\s*(\d+)", text[q + len(key):])
    return int(m.group(1)) if m else 0


def sub_id_of(table, brace):
    """the 9th field of the mode-table row that starts at table[brace] ('' when the row has none)"""
    depth = field = 0
    string = False
    cur = ""
    p = brace + 1
    while p < len(table):
        ch = table[p]
        if string:
            if ch == "\\":
                cur += ch + table[p + 1]
                p += 2
                continue
            if ch == '"':
                string = False
            cur += ch
            p += 1
            continue
        if ch == '"':
            string = True
            cur += ch
            p += 1
            continue
        if ch in "({":
            depth += 1
        if ch in ")}" and depth > 0:
            depth -= 1
            cur += ch
            p += 1
            continue
        if ch == "}" or (ch == "," and depth == 0):
            if field == 8:
                a, b = cur.find('"'), cur.rfind('"')
                return cur[a + 1:b] if a >= 0 and b > a else ""
            if ch == "}":
                return ""
            field += 1
            cur = ""
            p += 1
            continue
        cur += ch
        p += 1
    return ""


def check_modes(root):
    errs = []
    const = {}
    for d, _, files in os.walk(os.path.join(root, "core/include/dect2")):
        for fn in files:
            if fn.endswith("_modes.h"):
                with open(os.path.join(d, fn), encoding="utf8", errors="replace") as f:
                    scan_constants(f.read(), const)
    num_fam = number_after(read(root, "app/app.h"), "constexpr int kNumFamilies = ")
    slots_max = number_after(read(root, "core/include/dect2/engine/engine.h"), "kPollSeqSlots = ")
    if num_fam <= 0 or slots_max <= 0:
        return [f"kNumFamilies ({num_fam}) or kPollSeqSlots ({slots_max}) not found"], "not read"
    table = read(root, "app/ui/mode_table.cpp")
    fam, subs = {}, {}
    for m in re.finditer(r'\n\s*\{\s*([A-Za-z0-9_:]+(?:\s*[+-]\s*[0-9]+)?)\s*,\s*"([^"]*)"', table):
        f = eval_expr(m.group(1), const)
        if f is None:
            errs.append(f'mode row "{m.group(2)}": cannot read the family "{m.group(1)}"')
            continue
        sub = sub_id_of(table, table.find("{", m.start()))
        if sub:
            subs.setdefault((f, sub), []).append(m.group(2))
        else:
            fam.setdefault(f, []).append(m.group(2))
    for k, names in subs.items():
        if len(names) > 1:
            errs.append(f"protocol entry {k[0]}/{k[1]} is used by {len(names)} mode rows")
    if len(fam) <= 100:
        errs.append(f"only {len(fam)} families read from the mode table")
    for f, names in fam.items():
        if len(names) > 1:
            errs.append(f"family {f} is used by {len(names)} mode rows: " + ", ".join(names))
        if f not in (999, 19999) and f < 20000 and not 0 <= f < num_fam:
            errs.append(f"family {f} ({names[0]}) is outside 0 .. kNumFamilies-1 ({num_fam})")
    slots = {}
    fn = ""
    for line in read(root, "core/src/engine/engine_telemetry.cpp").splitlines():
        m = re.search(r"^int\s+([A-Za-z0-9_:]+)\s*\(", line)
        if m:
            fn = m.group(1)
        m = re.search(r"case\s+([A-Za-z0-9_:+ -]+?)\s*:", line)
        owner = "case " + m.group(1) if m else fn
        for s in re.finditer(r"seqs\[([A-Za-z0-9_: +-]+)\]", line):
            v = eval_expr(s.group(1), const)
            if v is None:
                errs.append(f'cannot read the poll slot "{s.group(1)}"')
                continue
            slots.setdefault(v, set()).add(owner)
    if len(slots) <= 100:
        errs.append(f"only {len(slots)} poll slots read")
    for s, owners in slots.items():
        if len(owners) > 1:
            errs.append(f"poll slot {s} is used by {len(owners)} receivers: " + ", ".join(sorted(owners)))
        if not 0 <= s < slots_max:
            errs.append(f"poll slot {s} is outside 0 .. kPollSeqSlots-1 ({slots_max})")
    return errs, f"{len(fam)} families, {len(subs)} protocol entries, {len(slots)} poll slots"


# ---- web policy ------------------------------------------------------------------------------------------------------------------

def registry_mode_ids(root):
    """registryModeIds() of tests/server/test_web_safety.cpp"""
    ids = {"dvb", "t2", "t", "atsc", "atsc3", "dab", "isdbt", "fm"}
    s = read(root, "core/src/engine/modes.cpp")
    p = 0
    while True:
        p = s.find('\n    {"', p)
        if p < 0:
            break
        a = p + 7
        b = s.find('"', a)
        if b < 0:
            break
        i = s[a:b]
        c, nl = s.find(', "', b), s.find("\n", b)
        if c >= 0 and c < nl and i and len(i) < 20:
            ids.add(i)
        p = b
    for d, _, files in os.walk(os.path.join(root, "core/src")):
        for fn in files:
            if not fn.endswith(".cpp"):
                continue
            with open(os.path.join(d, fn), encoding="utf8", errors="replace") as f:
                s = f.read()
            p = 0
            while True:
                p = s.find("ModeTuning ", p)
                if p < 0:
                    break
                q = p + 11
                while q < len(s) and (s[q].isalnum() or s[q] == "_"):
                    q += 1
                semi = s.find(";", q)
                brace = s.find("{", q)
                is_def = s[q:q + 3] == "() " and s[q - 6:q] == "Tuning" and brace >= 0 and (semi < 0 or brace < semi)
                p = q
                if not is_def:
                    continue
                fin = s.find("\n}\n", q)
                body = s[q:q + min(1500, 1500 if fin < 0 else fin - q)]
                k = body.find(".id = ")
                if k < 0:
                    continue
                end = body.find(";", k)
                expr = body[k:end] if end >= 0 else body[k:]
                ids.update(re.findall(r'"([^"]*)"', expr))
    return ids


def function_body(text, name):
    """the text of the function `name`, up to the closing brace at the start of a line"""
    p = text.find(name + "() {")
    if p < 0:
        return ""
    e = text.find("\n}", p)
    return text[p:e if e >= 0 else len(text)]


def check_web_policy(root):
    src = read(root, "server/web/web_policy.cpp")
    known = set()
    for fn in ("webSafeKinds", "voiceKinds", "pagerKinds", "restrictedKnownKinds"):
        body = function_body(src, fn)
        if not body:
            return [f"{fn}() not found in server/web/web_policy.cpp"], "not read"
        known.update(re.findall(r'"([^"]+)"', re.sub(r"//[^\n]*", "", body)))
    cat = function_body(src, "decoderCatalogue")
    known.update(re.findall(r'\{"([^"]+)"\s*,\s*DecoderClass::', cat))
    ids = registry_mode_ids(root)
    errs = []
    if len(ids) < 100:
        errs.append(f"found only {len(ids)} ids in the mode registry")
    for i in sorted(ids - known):
        errs.append(f'mode "{i}" is not classified in server/web/web_policy.cpp (add it to safe, voice, pager or restricted)')
    return errs, f"{len(ids)} registry ids, {len(known)} classified kinds"


# ---- Android link ----------------------------------------------------------------------------------------------------------------

# inline functions and macros of the FFmpeg headers: nothing to link
HEADER_ONLY = {"av_q2d", "av_make_q", "av_cmp_q", "av_inv_q", "av_err2str", "av_ts2str", "av_ts2timestr", "av_x_if_null", "av_clip", "av_clipf",
               "av_clip_uint8", "av_clip_int16", "av_ceil_log2", "av_popcount", "av_rescale_q_rnd_inline"}
CALL = re.compile(r"\b((?:av|avcodec|avformat|avio|avsubtitle|avdevice|swr|sws|hackrf|libusb)_[a-z0-9_]+)\s*\(")
# what the core only names as a table entry or a dlsym string does not count; a comment does not count either
COMMENT = re.compile(r"//[^\n]*|/\*.*?\*/", re.S)
STRING = re.compile(r'"(?:\\.|[^"\\\n])*"')


def declared_locally(text, name):
    """a static helper or a member of the same name in the core itself (hackrf_rate(...), libusb_ok(...))"""
    return re.search(r"(?:static|inline|void|int|bool|double|float|auto)\s+[\w:*&<> ]*\b" + name + r"\s*\([^;{]*\)\s*(?:const\s*)?\{", text) is not None


def check_android(root):
    stubs = read(root, "android/native/src/stubs_ext.c")
    if not stubs:
        return ["android/native/src/stubs_ext.c not read"], "not read"
    have = set(re.findall(r"\bKEEP\b[^(;\n]*?([A-Za-z0-9_]+)\s*\(", stubs))
    used = {}
    for top in ("core/src", "core/include"):
        for d, _, files in os.walk(os.path.join(root, top)):
            for fn in files:
                if not fn.endswith((".c", ".cc", ".cpp", ".h", ".hpp", ".inc")):
                    continue
                path = os.path.join(d, fn)
                with open(path, encoding="utf8", errors="replace") as f:
                    raw = f.read()
                text = STRING.sub('""', COMMENT.sub(" ", raw))
                for name in set(CALL.findall(text)):
                    if name in HEADER_ONLY or declared_locally(text, name):
                        continue
                    used.setdefault(name, os.path.relpath(path, root))
    # the link keeps only what the JNI bridge reaches (--gc-sections), so a call in unreached code needs no stand-in: those known today are listed
    known = {l.strip() for l in read(root, "tools/gate/android_unreached.txt").splitlines() if l.strip() and not l.startswith("#")}
    errs = [f"{n} (used in {w}) has no stand-in in android/native/src/stubs_ext.c: the Android link will fail. Add a stand-in, or list it in "
            f"tools/gate/android_unreached.txt once a real link (android.yml) has shown it is not reached" for n, w in sorted(used.items()) if n not in have and n not in known]
    return errs, f"{len(used)} library functions used by the core, {len(have)} stand-ins, {len(known)} listed as unreached"


CHECKS = {"modes": check_modes, "web_policy": check_web_policy, "android": check_android}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
    ap.add_argument("only", nargs="*", choices=[[], *CHECKS], default=[])
    a = ap.parse_args()
    bad = 0
    for name in a.only or CHECKS:
        errs, info = CHECKS[name](a.root)
        print(f"{'FAIL' if errs else 'ok  '} {name}: {info}")
        for e in errs:
            print(f"  {e}")
        bad += len(errs)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
