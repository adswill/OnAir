#!/usr/bin/env python3
# Writes the per-mode option controls of OnAir Web (docs/WEB-SCHEMA-3.9.md, "Mode options"). Like the panel schema (gen_web_schema.py) it is
# generated from the headers, not written per mode:
#   core/src/engine/engine_telemetry.cpp   standard -> the RxTelemetry member of the mode (the same map the panels use)
#   core/include/dect2/engine/engine.h     the member name -> Engine's accessor of that receiver (engine.dmr(), engine.adsb(), ...)
#   the receiver class' header             its public one-argument setters of a bool, a number or an enum: setX(T v); // label
# and generates
#   core/src/results/web_options_gen.cpp   per standard: the option schema (JSON) and the code that calls the setter on the running engine
# Setters the engine or the app drives themselves (tuning, offsets, sample rates, sound devices, volume, files, keys, transmit, callbacks)
# are left out by name. The first options of a mode (declaration order) are Simple, the rest Advanced, as with the panel fields.
#   python3 tools/gen_web_options.py           rewrite the generated file and print a coverage report
#   python3 tools/gen_web_options.py --check   exit 1 when the generated file is out of date
import os, re, sys, json, collections

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gen_web_schema as g   # noqa: E402

ROOT = g.ROOT
INC = g.INC
ENGINE_H = os.path.join(INC, "dect2", "engine", "engine.h")
OUT = os.path.join(ROOT, "core", "src", "results", "web_options_gen.cpp")
SIMPLE_OPTS = 4

NUM_T = {"int": "int", "unsigned": "int", "unsigned int": "int", "uint32_t": "int", "int32_t": "int", "uint16_t": "int", "int16_t": "int",
         "uint8_t": "int", "int8_t": "int", "size_t": "int", "long": "int", "float": "num", "double": "num", "bool": "bool"}
# setters the engine, the radio settings or the app own; and anything about files, keys, sound devices or transmitting
EXCLUDE = re.compile(r"(SignalOffset|Offset$|CenterHz|Centre|Center|SampleRate|Rate$|InputRate|Fs$|TimeBase|Clock|Now$|Time$|WallTime|Mute|Volume|"
                     r"Silent|Listen$|Debug|Log|Verbose|Path|File|Dir$|Folder|Key|Password|Passcode|Secret|Token|Tx|Transmit|Record|Callback|"
                     r"Sink|Tap$|Thread|Seed|Reference|Lat$|Lon$|Position|Location|Gain$|Freq|Frequency|Tune|Device|Output|Monitor|Session|"
                     r"Store|Plugin|Codec|Cache|Test|Sim|Synth|Inject|Hook|Handler|Observer|Wide|Narrow|Channeli[sz]er|Receiver$|Instance|"
                     r"Id$|Index$|Ptr$|Buffer|Queue|ComputeMode|Synchronous|Offline|TableScale|AudioInput|Threads)", re.I)


def comment_choices(comment):
    """'0 automatic, 1 DVB-S only, 2 DVB-S2 only' -> ('choice', [(0, 'automatic'), ...]); '1 POCSAG, 2 FLEX' -> ('mask', ...); else None"""
    head = re.split(r"[;(]", comment)[0]
    items = [x.strip() for x in re.split(r",\s*|\s+or\s+", head) if x.strip()]
    vals = []
    for it in items:
        m = re.match(r"^(-?\d+)\s*(?:=|:)?\s+([A-Za-z][^,]*)$", it)
        if not m:
            return None
        vals.append((int(m.group(1)), m.group(2).strip().rstrip(".")))
    if len(vals) < 2:
        return None
    v = [x[0] for x in vals]
    if v == list(range(v[0], v[0] + len(v))) and v[0] in (-1, 0, 1):
        return "choice", vals
    if all(x > 0 and (x & (x - 1)) == 0 for x in v) and len(set(v)) == len(v):
        return "mask", vals
    return None


def class_bodies(text):
    """(name, body text, start line) of every class / struct with a body, brace matched; comments kept"""
    out = []
    for m in re.finditer(r"(?:^|\n)\s*(?:class|struct)\s+(?:alignas\(\d+\)\s+)?(\w+)\s*(?:final\s*)?(?::[^{;]*)?\{", text):
        i = m.end()
        d = 1
        j = i
        while j < len(text) and d:
            c = text[j]
            if c == "{":
                d += 1
            elif c == "}":
                d -= 1
            elif c == "/" and text.startswith("//", j):
                k = text.find("\n", j)
                j = len(text) if k < 0 else k
                continue
            elif c == "/" and text.startswith("/*", j):
                k = text.find("*/", j)
                j = len(text) if k < 0 else k + 2
                continue
            j += 1
        out.append((m.group(1), text[i:j - 1], text.count("\n", 0, i) + 1, m.group(0).lstrip().startswith("class")))
    return out


def top_level_lines(body, is_class):
    """the lines of a class body at its own level (nested bodies skipped), with the access in force"""
    access = "private" if is_class else "public"
    d = 0
    for line in body.split("\n"):
        code = line.split("//")[0]
        if d == 0:
            m = re.match(r"\s*(public|private|protected)\s*:", code)
            if m:
                access = m.group(1)
                continue
            yield access, line
        d += code.count("{") - code.count("}")
        if d < 0:
            d = 0


def enum_values(text, name):
    """the enumerator names of enum `name` in text, when the values are 0, 1, 2 ... (explicit values that follow on are fine); else None"""
    m = re.search(r"enum\s+(?:class\s+|struct\s+)?%s\s*(?::\s*[\w:]+\s*)?\{([^}]*)\}" % re.escape(name), text)
    if not m:
        return None
    names, nxt = [], 0
    for part in re.sub(r"//[^\n]*", "", m.group(1)).split(","):
        part = part.strip()
        if not part:
            continue
        mm = re.match(r"(\w+)\s*(?:=\s*(-?\d+))?$", part)
        if not mm:
            return None
        v = int(mm.group(2)) if mm.group(2) is not None else nxt
        if v != nxt or v > 64:
            return None
        names.append(mm.group(1))
        nxt = v + 1
    return names or None


SETTER = re.compile(r"^\s*(?:virtual\s+)?void\s+(set[A-Z]\w*)\s*\(\s*(?:const\s+)?([\w:]+)\s*&?\s*(\w+)?\s*(?:=\s*[^,)]*)?\)\s*(const\s*)?(?:noexcept\s*)?(?:override\s*)?[;{]")


def setters_of(cls_name, file_text):
    for name, body, line, is_class in class_bodies(file_text):
        if name != cls_name:
            continue
        seen = collections.Counter()
        found = []
        for access, ln in top_level_lines(body, is_class):
            mm = re.match(r"\s*(?:virtual\s+)?(?:static\s+)?(?:inline\s+)?void\s+(set[A-Z]\w*)\s*\(", ln.split("//")[0])
            if mm:
                seen[mm.group(1)] += 1
            if access != "public" or "static" in ln.split("//")[0].split("(")[0]:
                continue
            m = SETTER.match(ln.split("//")[0] + (";" if not ln.split("//")[0].rstrip().endswith((";", "{", "}")) else ""))
            if not m:
                continue
            comment = ln.split("//", 1)[1].strip() if "//" in ln else ""
            found.append((m.group(1), m.group(2), comment))
        return [f for f in found if seen[f[0]] == 1], body
    return None, ""


def main():
    check = "--check" in sys.argv
    structs, enums = g.load_headers()
    rx = structs.get("dect2::RxTelemetry")
    rx_members = {m.name: m for m in rx.members}
    smap = g.standard_map(rx_members)
    eng = g.read(ENGINE_H)
    acc = {}
    for m in re.finditer(r"^\s*(?:dect2::)?([\w:]+)&\s+(\w+)\(\)\s*\{\s*return", eng, re.M):
        acc.setdefault(m.group(2), m.group(1))
    # where every class lives
    where = {}
    for base, _, files in os.walk(INC):
        for f in sorted(files):
            if not f.endswith(".h"):
                continue
            p = os.path.join(base, f)
            t = g.read(p)
            for m in re.finditer(r"(?:^|\n)\s*class\s+(\w+)\s*(?:final\s*)?(?::[^{;]*)?\{", t):
                where.setdefault(m.group(1), os.path.relpath(p, INC))
    per_std = collections.OrderedDict()
    report = collections.Counter()
    skipped = collections.Counter()
    for sd in sorted(smap):
        mem = smap[sd]
        if mem not in acc:
            report["no accessor"] += 1
            continue
        cls = acc[mem].split("::")[-1]
        hdr = where.get(cls)
        if not hdr:
            report["no class header"] += 1
            continue
        text = g.read(os.path.join(INC, hdr))
        sets, body = setters_of(cls, text)
        if not sets:
            report["no setters"] += 1
            continue
        opts = []
        for name, typ, comment in sets:
            stem = name[3:]
            if EXCLUDE.search(stem):
                skipped[stem] += 1
                continue
            t = typ.replace("std::", "")
            o = {"n": name}
            cc = comment_choices(comment) if NUM_T.get(t) == "int" else None
            if cc:
                o["k"] = cc[0]
                o["c"] = [{"v": v, "l": l} for v, l in cc[1]]
            elif t in NUM_T:
                o["k"] = NUM_T[t]
            else:
                ev = enum_values(body, t.split("::")[-1]) or enum_values(text, t.split("::")[-1])
                if not ev:
                    skipped["type " + t] += 1
                    continue
                o["k"] = "choice"
                o["c"] = [{"v": i, "l": g.humanize(x)} for i, x in enumerate(ev)]
            o["l"] = g.humanize(stem)
            if comment:
                o["h"] = comment[:200]
            u = g.unit_of(stem, comment)
            if u:
                o["u"] = u
            opts.append(o)
        if not opts:
            report["only excluded setters"] += 1
            continue
        for i, o in enumerate(opts):
            o["t"] = "s" if i < SIMPLE_OPTS else "a"
        per_std[sd] = (mem, hdr, opts)
        report["with options"] += 1
    o = []
    o.append("// GENERATED by tools/gen_web_options.py from the receivers' public setters (core/include), Engine's accessors (engine.h) and the\n"
             "// standard -> member map of core/src/engine/engine_telemetry.cpp. Do not edit: run the script. docs/WEB-SCHEMA-3.9.md explains it.\n"
             "// clang-format off\n#include \"dect2/results/web_options.h\"\n#include \"dect2/engine/engine.h\"\n#include <cmath>\n#include <type_traits>")
    for hdr in sorted({h for _, h, _ in per_std.values()}):
        o.append('#include "%s"' % hdr)
    o.append("""namespace dect2::webschema {
namespace {
// calls a setter of one argument with a value from the page: bool, integer, enum or floating point
template <class A> A convert(double v) {
    if constexpr (std::is_same_v<A, bool>) return v != 0;
    else if constexpr (std::is_enum_v<A>) return (A)(int)std::lround(v);
    else if constexpr (std::is_integral_v<A>) return (A)std::llround(v);
    else return (A)v;
}
template <class Obj, class R, class T> void call(Obj& o, void (R::*f)(T), double v) { (o.*f)(convert<std::decay_t<T>>(v)); }
template <class Obj, class R, class T> void call(Obj& o, void (R::*f)(T) noexcept, double v) { (o.*f)(convert<std::decay_t<T>>(v)); }
template <class Obj> using Rx = std::remove_reference_t<Obj>;""")
    for sd, (mem, hdr, opts) in per_std.items():
        o.append('static const char kOpt_%d[] = R"JSON(%s)JSON";' % (sd, json.dumps(opts, separators=(",", ":"))))
        o.append("static bool set_%d(Engine& e, const std::string& n, double v) {" % sd)
        o.append("    auto& r = e.%s();" % mem)
        for x in opts:
            o.append('    if (n == "%s") { call(r, &Rx<decltype(r)>::%s, v); return true; }' % (x["n"], x["n"]))
        o.append("    return false;\n}")
    o.append("struct OptEntry { int standard; const char* schema; bool (*set)(Engine&, const std::string&, double); };")
    o.append("const OptEntry kOpt[] = {")
    for sd in per_std:
        o.append("    {%d, kOpt_%d, set_%d}," % (sd, sd, sd))
    o.append("};\n} // namespace")
    o.append("""
const char* optionsJson(int standard) {
    for (const OptEntry& x : kOpt) if (x.standard == standard) return x.schema;
    return nullptr;
}
std::vector<int> optionStandards() {
    std::vector<int> v;
    for (const OptEntry& x : kOpt) v.push_back(x.standard);
    return v;
}
bool applyOptionUnchecked(Engine& e, int standard, const std::string& name, double value) {
    for (const OptEntry& x : kOpt) if (x.standard == standard) return x.set(e, name, value);
    return false;
}
} // namespace dect2::webschema
""")
    text = "\n".join(o) + "\n"
    n_opts = sum(len(v[2]) for v in per_std.values())
    if check:
        cur = g.read(OUT) if os.path.exists(OUT) else ""
        if cur != text:
            print("core/src/results/web_options_gen.cpp is out of date: run python3 tools/gen_web_options.py")
            return 1
        print("web options: current (%d standards, %d options)" % (len(per_std), n_opts))
        return 0
    with open(OUT, "w", encoding="utf-8") as f:
        f.write(text)
    print("standards with options: %d, options: %d" % (len(per_std), n_opts))
    print("standards:", dict(report))
    print("left out by name (most common):", skipped.most_common(25))
    return 0


if __name__ == "__main__":
    sys.exit(main())
