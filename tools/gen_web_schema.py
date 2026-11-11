#!/usr/bin/env python3
# Writes the web panel schema of every mode (docs/WEB-SCHEMA-3.9.md). It reads the telemetry structs of core/include (every struct a mode's
# RxTelemetry member reaches: rows of its tables, nested blocks, enums), the standard -> member map of core/src/engine/engine_telemetry.cpp
# and the mode list of app/ui/mode_table.cpp, and generates
#   core/src/results/web_schema_gen.cpp   a JSON exporter for every reachable struct (tables, constellations, pictures, lists, nested blocks)
#                                         and the schema the browser renders from (field names, kinds, labels, units, simple / advanced tier)
# Nothing is written by hand for a mode: a new telemetry field appears in the browser the next time this runs. The quick gate runs it with
# --check so the generated file cannot go stale.
#   python3 tools/gen_web_schema.py           rewrite the generated file, print a coverage report
#   python3 tools/gen_web_schema.py --check   exit 1 when the generated file is out of date
import os, re, sys, json, collections

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
INC = os.path.join(ROOT, "core", "include")
OUT = os.path.join(ROOT, "core", "src", "results", "web_schema_gen.cpp")
TEL = os.path.join(ROOT, "core", "src", "engine", "engine_telemetry.cpp")
TABLE = os.path.join(ROOT, "app", "ui", "mode_table.cpp")

PRIM_NUM = {"int", "unsigned", "unsigned int", "long", "unsigned long", "long long", "unsigned long long", "size_t", "short", "unsigned short",
            "int8_t", "uint8_t", "int16_t", "uint16_t", "int32_t", "uint32_t", "int64_t", "uint64_t", "float", "double", "char", "unsigned char",
            "signed char", "std::size_t", "std::uint8_t", "std::uint16_t", "std::uint32_t", "std::uint64_t", "std::int32_t", "std::int64_t"}
U64 = {"uint64_t", "std::uint64_t", "unsigned long long", "size_t", "std::size_t", "unsigned long"}
I64 = {"int64_t", "std::int64_t", "long long", "long"}
CPLX = {"cf32", "std::complex<float>", "dect2::cf32", "std::complex<double>", "cf64"}
CAP_PRIM, CAP_ROWS, CAP_CPLX, CAP_TEXT = 2048, 200, 512, 4096
COMMON = ["seq", "state", "cfoHz", "snrDb", "dataValid", "blocksOk", "blocksBad"]
SIMPLE_OWN = 10   # the first own primitive fields of a mode are "simple"; the rest "advanced" (tables, pictures and constellations are always simple)
LAT = re.compile(r"^(lat|latitude|latDeg|latdeg|lat_deg)$", re.I)
LON = re.compile(r"^(lon|lng|longitude|lonDeg|londeg|lon_deg)$", re.I)
PIX = re.compile(r"^(px|pixels|pix|rgb|gray|grey|luma|image|picture|img|data|bytes)$", re.I)


def read(p):
    return open(p, encoding="utf-8", errors="replace").read()


# ---------------------------------------------------------------- the headers: structs, members, enums
class Member:
    __slots__ = ("type", "name", "dims", "comment", "line")

    def __init__(self, t, n, d, c, line):
        self.type, self.name, self.dims, self.comment, self.line = t, n, d, c, line


class Struct:
    def __init__(self, q, file, line):
        self.q, self.file, self.line = q, file, line   # qualified name, header (relative to core/include), line
        self.members = []
        self.enums = set()   # nested enum names (unqualified)
        self.access = "public"   # the access of the members being read (a class starts private)
        self.bases = []          # base classes: their public members come first


def split_top(s, sep):
    """split s at sep outside (), {}, <> and []"""
    out, d, cur = [], 0, []
    for ch in s:
        if ch in "({<[":
            d += 1
        elif ch in ")}>]":
            d -= 1
        if ch == sep and d == 0:
            out.append("".join(cur)); cur = []
        else:
            cur.append(ch)
    out.append("".join(cur))
    return out


def scan_header(rel, text, structs, enums):
    """A brace-aware pass over one header: struct bodies with their namespace, enums, members with trailing comments."""
    # strip block comments but keep line structure; keep // comments for the labels
    text = re.sub(r"/\*.*?\*/", lambda m: re.sub(r"[^\n]", " ", m.group(0)), text, flags=re.S)
    lines = text.split("\n")
    ns = []            # namespace stack entries: ("ns", name) / ("struct", qualified) / ("other", "")
    pending = None     # a struct or enum opening seen but its "{" not yet
    stmt = ""          # the statement being collected inside a struct (members can span lines)
    stmt_comment = ""
    for ln, raw in enumerate(lines, 1):
        code, _, comment = raw.partition("//")
        comment = comment.strip()
        if ns and ns[-1][0] == "struct":
            def acc(mm):
                structs[ns[-1][1]].access = mm.group(1)
                return " "
            code = re.sub(r"(?<![:\w])(public|private|protected)\s*:(?!:)", acc, code)
        i = 0
        while i < len(code):
            ch = code[i]
            if ch == "{":
                # what opens here?
                head = (stmt + " " + code[:i]).strip() if pending is None else pending
                m = re.search(r"(?:^|[\s;}])(struct|class)\s+(?:alignas\(\d+\)\s+)?(\w+)\s*(?::\s*([^{]*))?$", head)
                me = re.search(r"(?:^|[\s;}])enum\s+(?:class\s+|struct\s+)?(\w+)\s*(?::\s*\w+)?\s*$", head)
                mn = re.search(r"(?:^|[\s;}])namespace\s+([\w:]+)\s*$", head)
                if mn:
                    for part in mn.group(1).split("::"):
                        ns.append(("ns", part))
                    ns.append(("nsopen", str(len(mn.group(1).split("::")))))
                elif me:
                    q = qualify(ns, me.group(1))
                    enums.add(q)
                    for e in ns:
                        if e[0] == "struct":
                            structs[e[1]].enums.add(me.group(1))
                    ns.append(("enum", q))
                elif m and not re.search(r"\b(struct|class)\s+\w+\s*;", head) and not head.rstrip().endswith("="):
                    q = qualify(ns, m.group(2))
                    structs[q] = Struct(q, rel, ln)
                    if m.group(3):
                        structs[q].bases = [re.sub(r"^(public|private|protected|virtual)\s+", "", b.strip()).strip() for b in split_top(m.group(3), ",")]
                    if m.group(1) == "class":
                        structs[q].access = "private"
                    ns.append(("struct", q))
                else:
                    ns.append(("other", ""))
                stmt = ""; stmt_comment = ""
                pending = None
                code = code[i + 1:]
                i = 0
                continue
            if ch == "}":
                if ns:
                    top = ns.pop()
                    if top[0] == "nsopen":
                        for _ in range(int(top[1])):
                            if ns and ns[-1][0] == "ns":
                                ns.pop()
                stmt = ""; stmt_comment = ""
                code = code[i + 1:]
                i = 0
                continue
            if ch == ";" and ns and ns[-1][0] == "struct" and depth_ok(stmt + code[:i]):
                s = (stmt + code[:i]).strip()
                if s:
                    add_members(structs[ns[-1][1]], s, (stmt_comment or comment), ln)
                stmt = ""; stmt_comment = ""
                code = code[i + 1:]
                i = 0
                continue
            i += 1
        if ns and ns[-1][0] == "struct":
            rest = code.strip()
            if rest:
                stmt += rest + " "
                if comment and not stmt_comment:
                    stmt_comment = comment
        else:
            stmt = (stmt + " " + code).strip()[-300:]   # the head of a struct / enum / namespace can span lines
            stmt_comment = ""


def depth_ok(s):
    d = 0
    for ch in s:
        if ch in "({<[":
            d += 1
        elif ch in ")}>]":
            d -= 1
    return d == 0


def qualify(ns, name):
    parts = []
    for kind, v in ns:
        if kind == "ns":
            parts.append(v)
        elif kind == "struct":
            parts = v.split("::")
    return "::".join(parts + [name])


def split_decl(d):
    """'name[3] = init' / 'name{1,2}' -> (name, dims) or None"""
    d = d.strip()
    m = re.match(r"^(\w+)\s*((?:\[[^\]]*\])*)\s*(?:[={].*)?$", d, re.S)
    return (m.group(1), m.group(2)) if m else None


def add_members(st, s, comment, line):
    s = s.strip()
    if not s or len(s) > 600 or st.access != "public":
        return
    if s.startswith(("static ", "using ", "typedef ", "friend ", "template", "constexpr ", "virtual ", "explicit ", "operator",
                     "public:", "private:", "protected:", "return ", "#", "enum ", "struct ", "class ", "if ", "for ", "while ")):
        return
    s = re.sub(r"\b(mutable|volatile|alignas\(\d+\))\s+", "", s)
    s = re.sub(r"^\s*(public|private|protected):\s*", "", s)
    if "std::function" in s or "::iterator" in s or "operator" in s:
        return
    head = split_top(s, "=")[0]
    if "(" in head and not re.search(r"\)\s*\[", head):
        return   # a method, a constructor, or a member with a parenthesised initializer we do not read
    decls = split_top(s, ",")
    first = decls[0].strip()
    # the type is everything before the last identifier (followed only by dims and an initializer)
    m = re.match(r"^(.*?[\w>])\s+(\w+)\s*((?:\[[^\]]*\])*)\s*(?:[={].*)?$", first, re.S)
    if not m:
        return
    typ = re.sub(r"\s+", " ", m.group(1)).strip()
    typ = re.sub(r"^(const|struct|class) ", "", typ)
    if not re.match(r"^[\w:]+(\s*<.*>)?$", typ):
        return
    names = [(m.group(2), m.group(3))]
    for d in decls[1:]:
        r = split_decl(d)
        if r:
            names.append(r)
    for n, dims in names:
        st.members.append(Member(typ, n, dims, comment.strip(), line))


def load_headers():
    structs, enums = {}, set()
    for base, _, files in os.walk(INC):
        for f in sorted(files):
            if f.endswith(".h"):
                p = os.path.join(base, f)
                scan_header(os.path.relpath(p, INC), read(p), structs, enums)
    return structs, enums


# ---------------------------------------------------------------- the type model
class T:
    """resolved member type: kind in num,u64,i64,bool,str,cplx,enum,struct,vec,arr,pair,ptr,map,opt,skip"""

    def __init__(self, kind, **kw):
        self.kind = kind
        self.__dict__.update(kw)


def resolve(typ, here, structs, enums, depth=0):
    typ = typ.strip()
    typ = re.sub(r"^((const|struct|class)\s+)+", "", typ)
    if typ in PRIM_NUM:
        if typ in U64:
            return T("u64")
        if typ in I64:
            return T("i64")
        return T("num")
    if typ == "bool":
        return T("bool")
    if typ in ("std::string", "string"):
        return T("str")
    if typ in CPLX:
        return T("cplx")
    if typ in ("std::string_view",):
        return T("skip", why=typ)
    m = re.match(r"^(std::vector|std::deque|std::list|std::array|std::set|std::unordered_set)\s*<(.*)>$", typ)
    if m:
        inner = split_top(m.group(2), ",")
        e = resolve(inner[0], here, structs, enums, depth + 1)
        if e.kind == "skip":
            return T("skip", why=typ)
        return T("vec", elem=e)
    m = re.match(r"^std::pair\s*<(.*)>$", typ)
    if m:
        a, b = split_top(m.group(1), ",")[:2]
        ra, rb = resolve(a, here, structs, enums, depth + 1), resolve(b, here, structs, enums, depth + 1)
        if ra.kind == "skip" or rb.kind == "skip":
            return T("skip", why=typ)
        return T("pair", a=ra, b=rb)
    m = re.match(r"^(std::shared_ptr|std::unique_ptr|std::optional)\s*<(.*)>$", typ)
    if m:
        e = resolve(m.group(2), here, structs, enums, depth + 1)
        if e.kind == "skip":
            return T("skip", why=typ)
        return T("ptr", elem=e)
    m = re.match(r"^(std::map|std::unordered_map)\s*<(.*)>$", typ)
    if m:
        k, v = split_top(m.group(2), ",")[:2]
        rk, rv = resolve(k, here, structs, enums, depth + 1), resolve(v, here, structs, enums, depth + 1)
        if rk.kind == "skip" or rv.kind == "skip":
            return T("skip", why=typ)
        return T("map", k=rk, v=rv)
    q = lookup(typ, here, structs)
    if q:
        return T("struct", q=q)
    qe = lookup(typ, here, {e: None for e in enums})
    if qe:
        return T("enum", q=qe)
    return T("skip", why=typ)


def lookup(name, here, table):
    """a type name as written inside struct `here` (qualified name) -> the qualified name in table"""
    name = name.strip()
    if name in table:
        return name
    parts = here.split("::")
    for i in range(len(parts), -1, -1):
        cand = "::".join(parts[:i] + [name])
        if cand in table:
            return cand
    if ("dect2::" + name) in table:
        return "dect2::" + name
    tail = "::" + name
    hits = [k for k in table if k.endswith(tail)]
    return hits[0] if len(hits) == 1 else None


# ---------------------------------------------------------------- the standard -> member map
def standard_map(rx_members):
    src = read(TEL)
    body = src[src.index("int Engine::pollMode("):]
    out = {}
    for line in body.split("\n"):
        cases = re.findall(r"case (\d+):", line)
        if not cases:
            continue
        m = re.search(r"&RxTelemetry::(\w+)\)", line)
        if m:
            for c in cases:
                out[int(c)] = m.group(1)
            continue
        m = re.search(r"return poll(\d+)\(", line)
        if m:
            fb = re.search(r"int poll%s\(Rx& rx, RxTelemetry& t, uint64_t\* seqs\) \{(.*?)\n\}" % m.group(1), src, re.S)
            if fb:
                used = [x for x in set(re.findall(r"t\.(\w+)", fb.group(1))) if x in rx_members and rx_members[x].type[0].isupper()]
                if len(used) == 1:
                    for c in cases:
                        out[int(c)] = used[0]
    return out


# ---------------------------------------------------------------- the mode table
def strings(s):
    out = []
    for m in re.finditer(r'"((?:[^"\\]|\\.)*)"', s):
        out.append(m.group(1).replace('\\"', '"').replace("\\n", " "))
    return out


def mode_rows():
    src = read(TABLE)
    g = re.search(r"kGroupNames\[kNumGroups\]\s*=\s*\{(.*?)\};", src, re.S)
    groups = [x.strip() for x in strings(g.group(1))] if g else []
    rows = []
    for line in src.splitlines():
        m = re.match(r"\s*\{(\d+),\s*\"", line)
        if not m:
            continue
        st = strings(line)
        if len(st) < 2:
            continue
        gm = re.match(r"\s*\{\d+,\s*\"(?:[^\"\\]|\\.)*\",\s*(\d+)\s*,", line)
        fam = int(m.group(1))
        # the sub-mode id is the last short string of the row (a lower-case token) when the row has one
        sub = ""
        for s in reversed(st):
            if re.match(r"^[a-z][a-z0-9_]*$", s) and s not in ("x",):
                sub = s
                break
        rows.append({"family": fam, "name": st[0], "group": int(gm.group(1)) if gm else -1,
                     "blurb": (st[2] if len(st) > 2 and st[2] else st[1]).strip(), "sub": sub})
    return groups, rows


# ---------------------------------------------------------------- emission
def cname(q):
    return re.sub(r"\W", "_", q)


def humanize(name):
    s = re.sub(r"([a-z0-9])([A-Z])", r"\1 \2", name).replace("_", " ")
    s = s[0].upper() + s[1:]
    s = re.sub(r"\bDb\b", "dB", s)
    s = re.sub(r"\bDbfs\b", "dBFS", s)
    s = re.sub(r"\bHz\b", "Hz", s)
    s = re.sub(r"\bK Hz\b", "kHz", s)
    s = re.sub(r"\bM Hz\b", "MHz", s)
    s = re.sub(r"\bPct\b", "%", s)
    s = re.sub(r"\bMs\b", "ms", s)
    s = re.sub(r"\bOk\b", "OK", s)
    s = re.sub(r"\bId\b", "ID", s)
    s = re.sub(r"\bSnr\b", "SNR", s)
    s = re.sub(r"\bCfo\b", "CFO", s)
    s = re.sub(r"\bFec\b", "FEC", s)
    s = re.sub(r"\bCrc\b", "CRC", s)
    s = re.sub(r"\bPer\b", "per", s)
    return s


def unit_of(name, comment):
    n = name
    for suf, u in (("Dbfs", "dBFS"), ("Dbm", "dBm"), ("Db", "dB"), ("Khz", "kHz"), ("Mhz", "MHz"), ("Ghz", "GHz"), ("Hz", "Hz"), ("Ppm", "ppm"),
                   ("Pct", "%"), ("Percent", "%"), ("Ms", "ms"), ("Us", "us"), ("Sec", "s"), ("Secs", "s"), ("Nm", "NM"), ("Km", "km"),
                   ("Kbps", "kbit/s"), ("Bps", "bit/s"), ("Baud", "Bd"), ("Deg", "deg"), ("Kts", "kt"), ("Ft", "ft")):
        if n.endswith(suf) and len(n) > len(suf):
            return u
    m = re.search(r"\b(dBFS|dBm|dB|kHz|MHz|GHz|Hz|ppm|ms|kbit/s|bit/s|baud|km|NM|knots|feet|degrees)\b", comment)
    if m:
        return {"knots": "kt", "feet": "ft", "degrees": "deg", "baud": "Bd"}.get(m.group(1), m.group(1))
    if "%" in comment and "percent" in comment.lower():
        return "%"
    return ""


def label_of(name, comment):
    c = comment.strip()
    if not c:
        return humanize(name)
    c = re.split(r"[;(]|\s-\s", c)[0].strip().rstrip(".,:")
    if len(c) > 90 or len(c) < 2 or c.lower().startswith(("valid when", "the part", "filled", "---")):
        return humanize(name)
    return c[0].upper() + c[1:]


class Emitter:
    def __init__(self, structs, enums):
        self.structs, self.enums = structs, enums
        self.funcs = collections.OrderedDict()   # qualified struct -> C++ text
        self.schemas = collections.OrderedDict()  # qualified struct -> schema dict
        self.skipped = collections.Counter()
        self.stack = []
        self.counts = collections.Counter()

    def ensure(self, q, depth=0):
        if q in self.funcs or q in self.stack:
            return
        if depth > 6:
            return
        self.stack.append(q)
        st = self.structs[q]
        fields, lines = [], []
        body = []
        members = []
        for b in st.bases:
            bq = lookup(b, q, self.structs)
            if bq:
                members += self.all_members(bq, 0)
        members += st.members
        for mem in members:
            t = resolve(mem.type, q, self.structs, self.enums)
            if mem.dims:
                dims = re.findall(r"\[([^\]]*)\]", mem.dims)
                if len(dims) != 1 or not dims[0].strip():
                    self.skipped[mem.type + mem.dims] += 1
                    continue
                t = T("vec", elem=t, carray=dims[0].strip())
                if t.elem.kind == "skip":
                    self.skipped[mem.type] += 1
                    continue
            if t.kind == "skip":
                self.skipped[t.why] += 1
                continue
            v = self.value_code("v." + mem.name, t, q, depth)
            if v is None:
                self.skipped[mem.type] += 1
                continue
            body.append('    o.key("%s"); %s' % (mem.name, v))
            fields.append(self.field_schema(mem, t, q))
            self.counts[t.kind] += 1
        self.stack.pop()
        self.funcs[q] = "static void ev_%s(std::string& s, const %s& v) {\n    detail::J o(s);\n%s\n    o.end();\n}\n" % (
            cname(q), q, "\n".join(body))
        self.schemas[q] = fields

    def all_members(self, q, depth):
        st = self.structs[q]
        out = []
        if depth < 4:
            for b in st.bases:
                bq = lookup(b, q, self.structs)
                if bq:
                    out += self.all_members(bq, depth + 1)
        return out + st.members

    def value_code(self, expr, t, here, depth):
        k = t.kind
        if k == "num":
            return "detail::vnum(s, (double)(%s));" % expr
        if k == "u64":
            return "detail::vu64(s, (unsigned long long)(%s));" % expr
        if k == "i64":
            return "detail::vi64(s, (long long)(%s));" % expr
        if k == "bool":
            return "detail::vbool(s, %s);" % expr
        if k == "str":
            return "detail::vstr(s, %s, %d);" % (expr, CAP_TEXT)
        if k == "cplx":
            return "detail::vcplx(s, (float)(%s).real(), (float)(%s).imag());" % (expr, expr)
        if k == "enum":
            return "detail::vnum(s, (double)(int)(%s));" % expr
        if k == "struct":
            self.ensure(t.q, depth + 1)
            if t.q not in self.funcs:
                return None
            return "ev_%s(s, %s);" % (cname(t.q), expr)
        if k == "ptr":
            inner = self.value_code("(*(%s))" % expr, t.elem, here, depth)
            if inner is None:
                return None
            return "if (%s) { %s } else s += \"null\";" % (expr, inner)
        if k == "pair":
            a = self.value_code("(%s).first" % expr, t.a, here, depth)
            b = self.value_code("(%s).second" % expr, t.b, here, depth)
            if a is None or b is None:
                return None
            return "s += '['; { %s } s += ','; { %s } s += ']';" % (a, b)
        if k == "map":
            vv = self.value_code("kv.second", t.v, here, depth)
            if vv is None:
                return None
            if t.k.kind == "str":
                return "{ detail::J m(s); size_t n = 0; for (const auto& kv : %s) { if (n++ >= %d) break; m.key(kv.first.c_str()); %s } m.end(); }" % (expr, CAP_ROWS, vv)
            kk = self.value_code("kv.first", t.k, here, depth)
            if kk is None:
                return None
            return "{ detail::Arr a(s); size_t n = 0; for (const auto& kv : %s) { if (n++ >= %d) break; a.next(); s += '['; { %s } s += ','; { %s } s += ']'; } a.end(); }" % (expr, CAP_ROWS, kk, vv)
        if k == "vec":
            e = t.elem
            if e.kind == "cplx":
                return "detail::vconst(s, (const float*)%s, (size_t)%s, %d);" % (
                    ("(%s).data()" % expr if not getattr(t, "carray", None) else expr),
                    ("(%s).size()" % expr if not getattr(t, "carray", None) else t.carray), CAP_CPLX)
            cap = CAP_ROWS if e.kind in ("struct", "pair", "map", "ptr", "vec") else CAP_PRIM
            inner = self.value_code("(%s)[i]" % expr, e, here, depth)
            if inner is None:
                return None
            n = ("(size_t)(%s)" % t.carray) if getattr(t, "carray", None) else "(%s).size()" % expr
            return "{ detail::Arr a(s); const size_t n = %s; for (size_t i = 0; i < n && i < %d; i++) { a.next(); %s } a.end(); }" % (n, cap, inner)
        return None

    def field_schema(self, mem, t, here):
        f = {"n": mem.name, "l": label_of(mem.name, mem.comment)}
        u = unit_of(mem.name, mem.comment)
        if u:
            f["u"] = u
        k = t.kind
        if k in ("num", "u64", "i64", "enum"):
            f["k"] = "num"
        elif k == "bool":
            f["k"] = "bool"
        elif k == "str":
            f["k"] = "text"
        elif k == "cplx":
            f["k"] = "num"; f["l"] += " (I/Q)"
        elif k == "struct":
            f["k"] = "obj"; f["s"] = t.q
            if t.q in self.schemas:
                f["f"] = self.schemas[t.q]
        elif k == "ptr":
            inner = self.field_schema(mem, t.elem, here)
            inner["l"] = f["l"]
            return inner
        elif k == "pair":
            f["k"] = "list"
        elif k == "map":
            f["k"] = "obj"
        elif k == "vec":
            e = t.elem
            if e.kind == "cplx":
                f["k"] = "const"
            elif e.kind == "struct":
                f["k"] = "table"; f["s"] = e.q
                cols = self.schemas.get(e.q, [])
                if e.q in self.schemas:
                    f["f"] = self.schemas[e.q]
                lat = [c["n"] for c in cols if LAT.match(c["n"])]
                lon = [c["n"] for c in cols if LON.match(c["n"])]
                if lat and lon:
                    lab = [c["n"] for c in cols if c["n"].lower() in ("callsign", "name", "icao", "id", "mmsi", "call", "label", "ident", "serial")]
                    f["map"] = {"lat": lat[0], "lon": lon[0], "label": lab[0] if lab else cols[0]["n"] if cols else ""}
            elif e.kind == "str":
                f["k"] = "lines"
            else:
                f["k"] = "list"
        else:
            f["k"] = "text"
        return f


def is_image(st):
    """a struct with a width, a height and one byte vector: rendered as a picture"""
    names = {m.name: m for m in st.members}
    w = next((n for n in ("w", "width", "cols", "columns") if n in names), None)
    h = next((n for n in ("h", "height", "rows", "lines") if n in names), None)
    px = [m for m in st.members if re.match(r"std::vector\s*<\s*(uint8_t|unsigned char|std::uint8_t)\s*>", m.type) and PIX.match(m.name)]
    if w and h and len(px) == 1:
        return w, h, px[0].name
    return None


def main():
    check = "--check" in sys.argv
    structs, enums = load_headers()
    rx = structs.get("dect2::RxTelemetry")
    if not rx:
        print("RxTelemetry not found"); return 1
    rx_members = {m.name: m for m in rx.members}
    smap = standard_map(rx_members)
    em = Emitter(structs, enums)
    # the common block is one struct of its own: the fields every mode has (RxTelemetry's scalars)
    # per standard: the member struct
    per_std = collections.OrderedDict()
    for sd in sorted(smap):
        mem = rx_members.get(smap[sd])
        if not mem:
            continue
        t = resolve(mem.type, "dect2::RxTelemetry", structs, enums)
        if t.kind != "struct":
            continue
        em.ensure(t.q)
        if t.q in em.funcs:
            per_std[sd] = (mem.name, t.q)
    # pictures: structs with w / h / bytes get an extra "image" field (the exporter adds a PNG data URL)
    images = {}
    for q in list(em.funcs):
        im = is_image(structs[q])
        if im:
            images[q] = im
            em.schemas[q].append({"n": "image", "k": "image", "l": "Picture"})
    # the mode rows
    groups, rows = mode_rows()
    tv_std = {0: 0, 1: 2, 2: 3, 3: 4, 4: 5, 5: 6}   # family -> telemetry standard of the TV / broadcast families (the common block + rx_json details)
    tv_engine = {0: 0, 1: 3, 2: 4, 3: 5, 4: 6, 5: 7}   # family -> the headless server's mode number (ServerHost::buildModes: 0 dvb, 3 atsc, 4 dab, 5 atsc3, 6 isdbt, 7 fm)
    covered, uncovered = 0, []
    for r in rows:
        if r["family"] >= 6:
            r["std"] = r["family"] + 1
            r["engine"] = r["family"] + 2
        else:
            r["std"] = tv_std.get(r["family"], -1)
            r["engine"] = tv_engine.get(r["family"], r["family"])
        r["member"] = per_std[r["std"]][0] if r["std"] in per_std else ""
        if r["member"] or r["family"] < 6:
            covered += 1
        else:
            uncovered.append(r)
    # ---- the file
    o = []
    o.append("// GENERATED by tools/gen_web_schema.py from core/include (the telemetry structs), core/src/engine/engine_telemetry.cpp (standard -> member)\n"
             "// and app/ui/mode_table.cpp (the mode rows). Do not edit: run the script. docs/WEB-SCHEMA-3.9.md explains the schema.\n"
             "// clang-format off\n#include \"dect2/results/web_schema.h\"\n#include \"dect2/t2/t2rx.h\"\n")
    files = sorted({structs[q].file for q in em.funcs} | {structs[q].file for q in em.funcs})
    for f in files:
        if f != "dect2/t2/t2rx.h":
            o.append('#include "%s"' % f)
    o.append("namespace dect2::webschema {\nnamespace {")
    # forward declarations (tables refer to rows that may be emitted later)
    for q in em.funcs:
        o.append("static void ev_%s(std::string& s, const %s& v);" % (cname(q), q))
    for q, code in em.funcs.items():
        if q in images:
            w, h, px = images[q]
            code = code.replace("    o.end();\n}", '    o.key("image"); detail::vimage(s, v.%s.data(), v.%s.size(), (int)v.%s, (int)v.%s);\n    o.end();\n}' % (px, px, w, h))
        o.append(code)
    # schemas as JSON text per struct
    for q, fields in em.schemas.items():
        fl = fields
        o.append('static const char kSchema_%s[] = R"JSON(%s)JSON";' % (cname(q), json.dumps(fl, separators=(",", ":"))))
    o.append("struct StdEntry { int standard; const char* member; const char* structName; const char* schema; bool (*put)(const RxTelemetry&, std::string&); };")
    for sd, (mem, q) in per_std.items():
        o.append("static bool put_%d(const RxTelemetry& rx, std::string& s) { ev_%s(s, rx.%s); return true; }" % (sd, cname(q), mem))
    o.append("static const StdEntry kStd[] = {")
    for sd, (mem, q) in per_std.items():
        o.append('    {%d, "%s", "%s", kSchema_%s, put_%d},' % (sd, mem, q, cname(q), sd))
    o.append("};")
    o.append("static const char* const kGroups[] = {%s};" % ", ".join('"%s"' % g.replace('"', '\\"') for g in groups))
    o.append("static const WebModeRow kRows[] = {")
    for r in rows:
        o.append('    {%d, "%s", %d, "%s", "%s", %d, %d, "%s"},' % (r["family"], r["name"].replace('"', '\\"'), r["group"], r["blurb"].replace('"', '\\"'),
                                                                 r["sub"], r["engine"], r["std"], r["member"]))
    o.append("};\n} // namespace")
    o.append("""
bool putModeFields(const RxTelemetry& rx, int standard, std::string& out) {
    for (const StdEntry& e : kStd) if (e.standard == standard) return e.put(rx, out);
    return false;
}
const char* schemaJson(int standard) {
    for (const StdEntry& e : kStd) if (e.standard == standard) return e.schema;
    return nullptr;
}
const char* schemaStruct(int standard) {
    for (const StdEntry& e : kStd) if (e.standard == standard) return e.structName;
    return nullptr;
}
std::vector<int> schemaStandards() {
    std::vector<int> v;
    for (const StdEntry& e : kStd) v.push_back(e.standard);
    return v;
}
const WebModeRow* webModeRows(int& n) { n = (int)(sizeof kRows / sizeof *kRows); return kRows; }
const char* const* webModeGroups(int& n) { n = (int)(sizeof kGroups / sizeof *kGroups); return kGroups; }
} // namespace dect2::webschema
""")
    text = "\n".join(o) + "\n"
    if check:
        cur = read(OUT) if os.path.exists(OUT) else ""
        if cur != text:
            print("core/src/results/web_schema_gen.cpp is out of date: run python3 tools/gen_web_schema.py")
            return 1
        print("web schema: current (%d standards, %d structs, %d mode rows)" % (len(per_std), len(em.funcs), len(rows)))
        return 0
    with open(OUT, "w", encoding="utf-8") as f:
        f.write(text)
    print("structs emitted: %d; standards: %d; mode rows: %d (covered %d; %d rows with no telemetry member)" % (len(em.funcs), len(per_std), len(rows), covered, len(uncovered)))
    print("field kinds:", dict(em.counts))
    print("pictures:", len(images), [q.split("::")[-1] for q in images][:12])
    print("skipped member types:", em.skipped.most_common(20))
    for r in uncovered[:40]:
        print("  no member: family %d %s (standard %d)" % (r["family"], r["name"], r["std"]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
