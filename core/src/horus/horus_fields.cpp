// Project Horus: the user-editable tables (horus_proto.h). The payload ID list ("id, callsign" lines) and the v2 custom field list
// in the JSON shape the horusdemodlib wiki documents ("struct" with the characters < > B b H h f x, "fields" with the
// post-processing names none, battery_5v_byte, divide_by_10, divide_by_100). Nothing is downloaded: the user pastes or edits them.
#include "dect2/horus/horus_proto.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace dect2::horus {

std::string defaultIdList() {
    return "# Horus payload IDs: one \"id, callsign\" per line (the format of horusdemodlib's payload_id_list.txt).\n"
           "# Paste the IDs of the payloads you track; nothing is downloaded.\n"
           "256, 4FSKTEST-V2\n";
}

std::string defaultCustomFields() {
    return "{\n"
           "  \"4FSKTEST-V2\": {\n"
           "    \"comment\": \"Default custom fields for RS41ng\",\n"
           "    \"struct\": \"<hhBHxx\",\n"
           "    \"fields\": [[\"ascent_rate\", \"divide_by_100\"], [\"ext_temperature\", \"divide_by_10\"], [\"ext_humidity\", \"none\"], [\"ext_pressure\", \"divide_by_10\"]]\n"
           "  }\n"
           "}\n";
}

int parseIdList(const std::string& text, std::map<int, std::string>& out) {
    int bad = 0;
    size_t s = 0;
    while (s <= text.size()) {
        size_t e = text.find('\n', s);
        if (e == std::string::npos) e = text.size();
        std::string line = text.substr(s, e - s);
        s = e + 1;
        if (const size_t h = line.find('#'); h != std::string::npos) line.resize(h);
        for (char& c : line) if (c == ',' || c == '\t' || c == '\r' || c == ';') c = ' ';
        char* end = nullptr;
        const char* p = line.c_str();
        while (*p == ' ') p++;
        if (!*p) { if (e == text.size()) break; continue; }
        const long id = std::strtol(p, &end, 10);
        while (end && *end == ' ') end++;
        std::string call = end ? end : "";
        while (!call.empty() && call.back() == ' ') call.pop_back();
        if (end == p || id < 0 || id > 65535 || call.empty() || call.find(' ') != std::string::npos) bad++;
        else out[(int)id] = call;
        if (e == text.size()) break;
    }
    return bad;
}

// ---------------------------------------------------------------- a small JSON reader for the custom field list
namespace {
struct JKV;   // an object member; not std::pair<std::string, J>, which clang rejects while J is incomplete
struct J {
    enum T { Null, Str, Num, Arr, Obj } t = Null;
    std::string s;
    double n = 0;
    std::vector<J> a;
    std::vector<JKV> o;
    const J* get(const char* k) const;
};
struct JKV { std::string first; J second; };
inline const J* J::get(const char* k) const { for (const auto& kv : o) if (kv.first == k) return &kv.second; return nullptr; }
struct Reader {
    const std::string& x; size_t i = 0; std::string err;
    explicit Reader(const std::string& s) : x(s) {}
    void ws() { while (i < x.size() && std::isspace((unsigned char)x[i])) i++; }
    bool fail(const char* m) { if (err.empty()) { char b[96]; snprintf(b, sizeof b, "%s at character %zu", m, i); err = b; } return false; }
    bool str(std::string& out) {
        if (i >= x.size() || x[i] != '"') return fail("expected a string");
        i++;
        while (i < x.size() && x[i] != '"') {
            if (x[i] == '\\' && i + 1 < x.size()) { i++; const char c = x[i]; out += c == 'n' ? '\n' : c == 't' ? '\t' : c; }
            else out += x[i];
            i++;
        }
        if (i >= x.size()) return fail("unterminated string");
        i++;
        return true;
    }
    bool val(J& v, int depth = 0) {
        if (depth > 16) return fail("nested too deeply");
        ws();
        if (i >= x.size()) return fail("unexpected end");
        const char c = x[i];
        if (c == '"') { v.t = J::Str; return str(v.s); }
        if (c == '{') {
            v.t = J::Obj; i++; ws();
            if (i < x.size() && x[i] == '}') { i++; return true; }
            for (;;) {
                ws(); std::string k; if (!str(k)) return false;
                ws(); if (i >= x.size() || x[i] != ':') return fail("expected ':'");
                i++; J sub; if (!val(sub, depth + 1)) return false;
                v.o.push_back({k, std::move(sub)});
                ws(); if (i < x.size() && x[i] == ',') { i++; continue; }
                if (i < x.size() && x[i] == '}') { i++; return true; }
                return fail("expected ',' or '}'");
            }
        }
        if (c == '[') {
            v.t = J::Arr; i++; ws();
            if (i < x.size() && x[i] == ']') { i++; return true; }
            for (;;) {
                J sub; if (!val(sub, depth + 1)) return false;
                v.a.push_back(std::move(sub));
                ws(); if (i < x.size() && x[i] == ',') { i++; continue; }
                if (i < x.size() && x[i] == ']') { i++; return true; }
                return fail("expected ',' or ']'");
            }
        }
        if (c == '-' || std::isdigit((unsigned char)c)) { char* e = nullptr; v.t = J::Num; v.n = std::strtod(x.c_str() + i, &e); i = size_t(e - x.c_str()); return true; }
        if (x.compare(i, 4, "null") == 0 || x.compare(i, 4, "true") == 0) { i += 4; return true; }
        if (x.compare(i, 5, "false") == 0) { i += 5; return true; }
        return fail("unexpected character");
    }
};

// Python's str() of a float, near enough: the shortest text that reads back the same, with ".0" on whole numbers
std::string pyFloat(double v) {
    char b[40];
    for (int p = 1; p <= 17; p++) { snprintf(b, sizeof b, "%.*g", p, v); if (std::strtod(b, nullptr) == v) break; }
    std::string s = b;
    if (s.find_first_of(".eEn") == std::string::npos) s += ".0";
    return s;
}

int structSize(char c) { switch (c) { case 'B': case 'b': case 'x': return 1; case 'H': case 'h': return 2; case 'f': return 4; default: return -1; } }
} // namespace

bool parseCustomFields(const std::string& json, std::map<std::string, CustomFormat>& out, std::string* error) {
    Reader r(json);
    J root;
    if (!r.val(root)) { if (error) *error = r.err; return false; }
    if (root.t != J::Obj) { if (error) *error = "the list must be a JSON object"; return false; }
    std::map<std::string, CustomFormat> m;
    for (const auto& kv : root.o) {
        if (kv.second.t != J::Obj) continue;
        CustomFormat f;
        if (const J* c = kv.second.get("comment"); c && c->t == J::Str) f.comment = c->s;
        if (const J* s = kv.second.get("struct"); s && s->t == J::Str) f.structFmt = s->s;
        if (const J* fl = kv.second.get("fields"); fl && fl->t == J::Arr)
            for (const J& e : fl->a) if (e.t == J::Arr && e.a.size() >= 2 && e.a[0].t == J::Str && e.a[1].t == J::Str) f.fields.push_back({e.a[0].s, e.a[1].s});
        if (const J* se = kv.second.get("sentence"); se && se->t == J::Arr)
            for (const J& e : se->a) if (e.t == J::Str) f.sentence.push_back(e.s);
        m[kv.first] = std::move(f);
    }
    out = std::move(m);
    return true;
}

std::vector<std::pair<std::string, std::string>> applyFormat(const CustomFormat& f, const uint8_t* p, int n) {
    std::vector<std::pair<std::string, std::string>> out;
    bool big = false;
    int pos = 0;
    size_t field = 0;
    std::string fmt;   // repeat counts written out ("2B" -> "BB")
    for (size_t i = 0; i < f.structFmt.size(); i++) {
        int count = 0;
        while (i < f.structFmt.size() && std::isdigit((unsigned char)f.structFmt[i])) count = count * 10 + (f.structFmt[i++] - '0');
        if (i >= f.structFmt.size() || count > 64) return {};
        fmt.append((size_t)std::max(1, count), f.structFmt[i]);
    }
    for (char c : fmt) {
        if (c == ' ') continue;
        if (c == '<' || c == '=' ) { big = false; continue; }
        if (c == '>' || c == '!') { big = true; continue; }
        const int sz = structSize(c);
        if (sz < 0 || pos + sz > n) return {};
        uint32_t u = 0;
        for (int k = 0; k < sz; k++) u |= uint32_t(p[pos + (big ? sz - 1 - k : k)]) << (8 * k);
        pos += sz;
        if (c == 'x') continue;
        double v;
        bool isFloat = false;
        if (c == 'b') v = (int8_t)u; else if (c == 'h') v = (int16_t)u; else if (c == 'f') { float fl; std::memcpy(&fl, &u, 4); v = fl; isFloat = true; } else v = u;
        const std::string name = field < f.fields.size() ? f.fields[field].first : "field " + std::to_string(field + 1);
        const std::string fn = field < f.fields.size() ? f.fields[field].second : "none";
        field++;
        char b[48];
        std::string txt;
        if (fn == "battery_5v_byte") { snprintf(b, sizeof b, "%.2f", v * 5.0 / 255.0); txt = b; }
        else if (fn == "divide_by_10") txt = pyFloat(v / 10.0);
        else if (fn == "divide_by_100") txt = pyFloat(v / 100.0);
        else if (isFloat) { snprintf(b, sizeof b, "%.6f", v); txt = b; }
        else txt = std::to_string((long long)v);
        out.push_back({name, txt});
    }
    return out;
}

} // namespace dect2::horus
