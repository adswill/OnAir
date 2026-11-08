// Reference datasets: manifest, versions, validation, layering and the user's override files (see datasets.h). No network code here.
#include "dect2/packs/datasets.h"
#include "dect2/channels/freq_db.h"
#include "dect2/common/u8file.h"
#include "dect2/engine/updater.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

namespace fs = std::filesystem;

namespace dect2::datasets {

// ---------------------------------------------------------------- versions
namespace {
struct Ver { std::vector<unsigned long long> n; std::string pre; };
Ver parseVer(const std::string& s) {
    Ver v;
    size_t i = 0;
    if (i < s.size() && (s[i] == 'v' || s[i] == 'V')) i++;
    unsigned long long cur = 0;
    bool have = false;
    for (; i < s.size(); i++) {
        const char c = s[i];
        if (std::isdigit((unsigned char)c)) { cur = cur * 10 + (c - '0'); have = true; }
        else if ((c == '.' || c == '-') && have && i + 1 < s.size() && std::isdigit((unsigned char)s[i + 1])) { v.n.push_back(cur); cur = 0; have = false; }
        else break;
    }
    if (have) v.n.push_back(cur);
    if (i < s.size()) { v.pre = s.substr(i); if (!v.pre.empty() && (v.pre[0] == '-' || v.pre[0] == '.' || v.pre[0] == '+')) v.pre.erase(0, 1); }
    // 20261001 == 2026.10.01 only when both sides are dates: a single 8-digit number is split
    if (v.n.size() == 1 && v.n[0] >= 10000101ULL && v.n[0] <= 99991231ULL) { const auto d = v.n[0]; v.n = {d / 10000, (d / 100) % 100, d % 100}; }
    return v;
}
}  // namespace

int compareVersions(const std::string& a, const std::string& b) {
    const Ver x = parseVer(a), y = parseVer(b);
    const size_t n = std::max(x.n.size(), y.n.size());
    for (size_t i = 0; i < n; i++) {
        const unsigned long long p = i < x.n.size() ? x.n[i] : 0, q = i < y.n.size() ? y.n[i] : 0;
        if (p != q) return p < q ? -1 : 1;
    }
    if (x.pre == y.pre) return 0;
    if (x.pre.empty()) return 1;      // 1.0 is newer than 1.0-rc1
    if (y.pre.empty()) return -1;
    return x.pre < y.pre ? -1 : 1;
}

// ---------------------------------------------------------------- tiny JSON (objects, arrays, strings, numbers, true/false/null)
namespace {
struct JKV;   // an object member; not std::pair<std::string, J>, which clang rejects while J is incomplete
struct J {
    enum K { Null, Num, Str, Bool, Arr, Obj } k = Null;
    double num = 0;
    std::string str;
    std::vector<J> arr;
    std::vector<JKV> obj;
    const J* get(const std::string& key) const;
};
struct JKV { std::string first; J second; };
inline const J* J::get(const std::string& key) const { for (const auto& p : obj) if (p.first == key) return &p.second; return nullptr; }
struct JP {
    const std::string& s;
    size_t p = 0;
    int depth = 0;
    std::string err;
    explicit JP(const std::string& t) : s(t) {}
    void ws() { while (p < s.size() && std::isspace((unsigned char)s[p])) p++; }
    bool fail(const char* m) { if (err.empty()) err = std::string(m) + " at byte " + std::to_string(p); return false; }
    bool str_(std::string& o) {
        if (p >= s.size() || s[p] != '"') return fail("string expected");
        p++;
        while (p < s.size() && s[p] != '"') {
            char c = s[p++];
            if (c == '\\') {
                if (p >= s.size()) return fail("bad escape");
                c = s[p++];
                switch (c) {
                    case 'n': o += '\n'; break; case 't': o += '\t'; break; case 'r': o += '\r'; break; case 'b': o += '\b'; break; case 'f': o += '\f'; break;
                    case '/': case '\\': case '"': o += c; break;
                    case 'u': {
                        if (p + 4 > s.size()) return fail("bad \\u");
                        const unsigned cp = (unsigned)std::strtoul(s.substr(p, 4).c_str(), nullptr, 16);
                        p += 4;
                        if (cp < 0x80) o += (char)cp;
                        else if (cp < 0x800) { o += (char)(0xC0 | (cp >> 6)); o += (char)(0x80 | (cp & 63)); }
                        else { o += (char)(0xE0 | (cp >> 12)); o += (char)(0x80 | ((cp >> 6) & 63)); o += (char)(0x80 | (cp & 63)); }
                        break;
                    }
                    default: return fail("bad escape");
                }
            } else o += c;
        }
        if (p >= s.size()) return fail("unterminated string");
        p++;
        return true;
    }
    bool val(J& v) {
        ws();
        if (p >= s.size()) return fail("value expected");
        if (++depth > 16) return fail("nested too deep");
        const char c = s[p];
        bool ok = true;
        if (c == '"') { v.k = J::Str; ok = str_(v.str); }
        else if (c == '{') {
            v.k = J::Obj; p++; ws();
            if (p < s.size() && s[p] == '}') p++;
            else for (;;) {
                ws();
                std::string key;
                if (!str_(key)) { ok = false; break; }
                ws();
                if (p >= s.size() || s[p++] != ':') { ok = fail("':' expected"); break; }
                J x;
                if (!val(x)) { ok = false; break; }
                v.obj.emplace_back(std::move(key), std::move(x));
                ws();
                if (p < s.size() && s[p] == ',') { p++; continue; }
                if (p < s.size() && s[p] == '}') { p++; break; }
                ok = fail("',' or '}' expected");
                break;
            }
        } else if (c == '[') {
            v.k = J::Arr; p++; ws();
            if (p < s.size() && s[p] == ']') p++;
            else for (;;) {
                J x;
                if (!val(x)) { ok = false; break; }
                v.arr.push_back(std::move(x));
                ws();
                if (p < s.size() && s[p] == ',') { p++; continue; }
                if (p < s.size() && s[p] == ']') { p++; break; }
                ok = fail("',' or ']' expected");
                break;
            }
        } else if (s.compare(p, 4, "true") == 0) { v.k = J::Bool; v.num = 1; p += 4; }
        else if (s.compare(p, 5, "false") == 0) { v.k = J::Bool; p += 5; }
        else if (s.compare(p, 4, "null") == 0) { p += 4; }
        else if (c == '-' || std::isdigit((unsigned char)c)) {
            char* end = nullptr;
            v.num = std::strtod(s.c_str() + p, &end);
            if (end == s.c_str() + p) ok = fail("number expected");
            else { v.k = J::Num; p = (size_t)(end - s.c_str()); }
        } else ok = fail("unexpected character");
        depth--;
        return ok;
    }
    bool doc(J& v) { if (!val(v)) return false; ws(); if (p != s.size()) return fail("text after the document"); return true; }
};

bool isHex64(const std::string& h) {
    if (h.size() != 64) return false;
    for (char c : h) if (!std::isxdigit((unsigned char)c)) return false;
    return true;
}
std::string lower(std::string s) { for (char& c : s) c = (char)std::tolower((unsigned char)c); return s; }
}  // namespace

// ---------------------------------------------------------------- manifest
bool validDatasetId(const std::string& id) {
    if (id.empty() || id.size() > 48) return false;
    for (char c : id) if (!(std::islower((unsigned char)c) || std::isdigit((unsigned char)c) || c == '.' || c == '_' || c == '-')) return false;
    return id[0] != '.' && id.find("..") == std::string::npos;
}

bool validAllowListEntry(const std::string& src) {
    if (src.compare(0, 8, "https://") != 0 || src.size() <= 8) return false;
    for (char c : src) if (std::isspace((unsigned char)c) || (unsigned char)c < 0x21 || c == '\\') return false;
    const size_t slash = src.find('/', 8);
    const std::string host = src.substr(8, slash == std::string::npos ? std::string::npos : slash - 8);
    if (host.empty() || host.find('@') != std::string::npos) return false;
    return src.find('?') == std::string::npos && src.find('#') == std::string::npos;
}

bool urlAllowed(const std::string& url, const std::vector<std::string>& allowList) {
    if (!validAllowListEntry(url) && url.compare(0, 8, "https://") != 0) return false;
    if (url.find('@') != std::string::npos && url.find('@') < url.find('/', 8)) return false;   // user info smuggles another host
    const std::string u = lower(url);
    for (std::string a : allowList) {
        if (!validAllowListEntry(a)) continue;
        a = lower(a);
        while (a.size() > 8 && a.back() == '/') a.pop_back();
        if (u.compare(0, a.size(), a) != 0) continue;
        if (u.size() == a.size() || u[a.size()] == '/') return true;   // path boundary
    }
    return false;
}

bool parseManifest(const std::string& text, Manifest& out, std::string& err) {
    out = Manifest();
    if (text.size() > (4u << 20)) { err = "manifest larger than 4 MB"; return false; }
    JP jp(text);
    J root;
    if (!jp.doc(root)) { err = "not JSON: " + jp.err; return false; }
    if (root.k != J::Obj) { err = "the manifest must be an object"; return false; }
    const J* f = root.get("format");
    if (!f || f->k != J::Str || f->str != "onair-datasets-1") { err = "format must be \"onair-datasets-1\""; return false; }
    out.format = f->str;
    const J* es = root.get("entries");
    if (!es || es->k != J::Arr) { err = "entries must be a list"; return false; }
    std::set<std::string> seen;
    for (size_t i = 0; i < es->arr.size(); i++) {
        const J& e = es->arr[i];
        const std::string at = "entry " + std::to_string(i + 1) + ": ";
        if (e.k != J::Obj) { err = at + "not an object"; return false; }
        auto s = [&](const char* k, std::string& o) { const J* x = e.get(k); if (!x || x->k != J::Str || x->str.empty()) { err = at + "missing \"" + k + "\""; return false; } o = x->str; return true; };
        ManifestEntry m;
        if (!s("id", m.id) || !s("version", m.version) || !s("url", m.url) || !s("sha256", m.sha256) || !s("licence", m.licence)) return false;
        if (!validDatasetId(m.id)) { err = at + "bad id"; return false; }
        if (!seen.insert(m.id).second) { err = at + "duplicate id " + m.id; return false; }
        if (m.version.size() > 40) { err = at + "version too long"; return false; }
        if (m.url.compare(0, 8, "https://") != 0 || m.url.size() > 1024) { err = at + "url must be https"; return false; }
        for (char c : m.url) if (std::isspace((unsigned char)c) || (unsigned char)c < 0x21) { err = at + "bad url"; return false; }
        if (!isHex64(m.sha256)) { err = at + "sha256 must be 64 hex digits"; return false; }
        m.sha256 = lower(m.sha256);
        const J* sz = e.get("size");
        if (!sz || sz->k != J::Num || sz->num < 1 || sz->num > 64.0 * 1048576) { err = at + "size must be 1 to 67108864 bytes"; return false; }
        m.size = (unsigned long long)sz->num;
        out.entries.push_back(std::move(m));
    }
    return true;
}

std::vector<UpdateOffer> selectUpdates(const Manifest& m, const std::map<std::string, std::string>& installed, const std::vector<std::string>& allowList,
                                       std::vector<std::string>* rejected) {
    std::vector<UpdateOffer> o;
    for (const ManifestEntry& e : m.entries) {
        if (!urlAllowed(e.url, allowList)) { if (rejected) rejected->push_back(e.id); continue; }
        const auto it = installed.find(e.id);
        if (it == installed.end()) { o.push_back({e, "", true}); continue; }
        if (it->second.empty() || compareVersions(e.version, it->second) > 0) o.push_back({e, it->second, false});
    }
    return o;
}

bool verifyDownload(const ManifestEntry& e, const std::string& body, std::string& err) {
    if (body.size() != e.size) { err = "size " + std::to_string(body.size()) + " bytes, the manifest says " + std::to_string(e.size); return false; }
    const std::string h = dect2::sha256Bytes(body.data(), body.size());
    if (lower(h) != lower(e.sha256)) { err = "sha256 does not match the manifest"; return false; }
    return true;
}

// ---------------------------------------------------------------- text helpers
namespace {
std::vector<std::string> lines(const std::string& t) {
    std::vector<std::string> v;
    size_t p = 0;
    while (p < t.size()) {
        size_t e = t.find('\n', p);
        if (e == std::string::npos) e = t.size();
        size_t le = e;
        if (le > p && t[le - 1] == '\r') le--;
        v.push_back(t.substr(p, le - p));
        p = e + 1;
    }
    return v;
}
std::vector<std::string> splitC(const std::string& s, char d) {
    std::vector<std::string> o;
    size_t p = 0;
    for (;;) { const size_t e = s.find(d, p); if (e == std::string::npos) { o.push_back(s.substr(p)); break; } o.push_back(s.substr(p, e - p)); p = e + 1; }
    return o;
}
std::string trimS(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) a++;
    while (b > a && std::isspace((unsigned char)s[b - 1])) b--;
    return s.substr(a, b - a);
}
std::string joinC(const std::vector<std::string>& f, char d) { std::string o; for (size_t i = 0; i < f.size(); i++) { if (i) o += d; o += f[i]; } return o; }
bool isNum(const std::string& s) { char* e = nullptr; const std::string t = trimS(s); if (t.empty()) return false; std::strtod(t.c_str(), &e); return *e == 0; }
}  // namespace

// ---------------------------------------------------------------- validation
Validation validateText(const Schema& s, const std::string& text, size_t previewLines) {
    Validation v;
    auto add = [&](int ln, std::string m, bool err = true) { v.issues.push_back({ln, std::move(m), err}); };
    auto keep = [&](const std::string& l) { if (v.preview.size() < previewLines) v.preview.push_back(l.size() > 160 ? l.substr(0, 160) + "..." : l); };
    if (text.empty()) { add(0, "the file is empty"); return v; }
    if (text.find('\0') != std::string::npos) { add(0, "binary data (a NUL byte) in a text dataset"); return v; }
    if (text.size() > (64u << 20)) { add(0, "larger than 64 MB"); return v; }
    const std::vector<std::string> L = lines(text);
    switch (s.format) {
        case Format::FreqTsv: {
            FreqParseReport rep;
            const auto e = parseFreqTsv(text, rep);
            v.records = e.size();
            for (const std::string& m : rep.log) {   // "line N: text"
                int ln = 0;
                const size_t sp = m.find("line ");
                if (sp != std::string::npos) ln = std::atoi(m.c_str() + sp + 5);
                add(ln, m);
            }
            for (const std::string& l : L) if (!l.empty() && l[0] != '#' && l[0] != '@') keep(l);
            break;
        }
        case Format::AirportsTsv: {
            bool open = false;
            for (size_t i = 0; i < L.size(); i++) {
                const std::string& l = L[i];
                if (l.empty() || l[0] == '#') continue;
                const auto f = splitC(l, '\t');
                const int ln = (int)i + 1;
                if (f[0] == "A") {
                    open = true;
                    if (f.size() < 11) { add(ln, "A line needs 11 tab-separated fields"); open = false; continue; }
                    if (f[1].empty()) { add(ln, "no airport code"); open = false; continue; }
                    if (!isNum(f[6]) || !isNum(f[7]) || std::fabs(std::atof(f[6].c_str())) > 90 || std::fabs(std::atof(f[7].c_str())) > 180) { add(ln, "bad latitude or longitude"); open = false; continue; }
                    v.records++;
                    keep(l);
                } else if (f[0] == "F") {
                    if (!open) { add(ln, "F line without an airport above it"); continue; }
                    const long k = std::atol(f.size() > 2 ? f[2].c_str() : "0");
                    if (f.size() < 3 || k <= 0 || k > 3000000) add(ln, "bad frequency (kHz expected)");
                } else add(ln, "line must start with A or F");
            }
            break;
        }
        case Format::BandplanJson: {
            JP jp(text);
            J root;
            if (!jp.doc(root)) { add(0, "not JSON: " + jp.err); break; }
            if (root.k != J::Obj) { add(0, "the file must be a JSON object"); break; }
            const J* f = root.get("format");
            if (!f || f->k != J::Str || f->str.compare(0, 15, "onair-bandplans") != 0) add(0, "\"format\" must start with onair-bandplans");
            const J* pl = root.get("plans");
            if (!pl || (pl->k != J::Obj && pl->k != J::Arr)) add(0, "\"plans\" missing");
            else v.records = pl->k == J::Obj ? pl->obj.size() : pl->arr.size();
            keep(L[0]);
            break;
        }
        case Format::Delimited: {
            size_t hi = 0;
            while (hi < L.size() && (L[hi].empty() || L[hi][0] == '#')) hi++;
            if (hi >= L.size()) { add(0, "no header row"); break; }
            const auto head = splitC(L[hi], s.delim);
            std::set<std::string> names;
            for (const std::string& h : head) if (!names.insert(trimS(h)).second) add((int)hi + 1, "duplicate column " + trimS(h));
            for (const std::string& k : s.keyColumns) if (!names.count(k)) add((int)hi + 1, "key column \"" + k + "\" missing in the header");
            std::vector<size_t> kc;
            for (const std::string& k : s.keyColumns) for (size_t i = 0; i < head.size(); i++) if (trimS(head[i]) == k) kc.push_back(i);
            std::set<std::string> keys;
            keep(L[hi]);
            for (size_t i = hi + 1; i < L.size(); i++) {
                if (L[i].empty() || L[i][0] == '#') continue;
                const auto f = splitC(L[i], s.delim);
                if (f.size() != head.size()) { add((int)i + 1, "expected " + std::to_string(head.size()) + " columns, found " + std::to_string(f.size())); continue; }
                if (!kc.empty()) {
                    std::string k;
                    for (size_t c : kc) k += trimS(f[c]) + "\x1f";
                    if (trimS(f[kc[0]]).empty()) { add((int)i + 1, "empty key"); continue; }
                    if (!keys.insert(k).second) add((int)i + 1, "duplicate key (the later row wins)", false);
                }
                v.records++;
                keep(L[i]);
            }
            break;
        }
        case Format::PlainText:
            for (const std::string& l : L) if (!l.empty() && l[0] != '#') { v.records++; keep(l); }
            break;
    }
    if (v.records == 0 && v.issues.empty()) add(0, "no records found");
    return v;
}

// ---------------------------------------------------------------- layering
std::string layerDelimited(const std::string& bundled, const std::string& user, char delim, const std::vector<std::string>& keyColumns, std::string* err) {
    const auto B = lines(bundled), U = lines(user);
    auto headerAt = [](const std::vector<std::string>& v) { size_t i = 0; while (i < v.size() && (v[i].empty() || v[i][0] == '#')) i++; return i; };
    const size_t bh = headerAt(B), uh = headerAt(U);
    if (bh >= B.size() || uh >= U.size()) { if (err) *err = "no header row"; return bundled; }
    const auto bHead = splitC(B[bh], delim), uHead = splitC(U[uh], delim);
    std::vector<size_t> bk;
    for (const std::string& k : keyColumns) {
        bool f = false;
        for (size_t i = 0; i < bHead.size(); i++) if (trimS(bHead[i]) == k) { bk.push_back(i); f = true; }
        if (!f) { if (err) *err = "key column \"" + k + "\" is not in the bundled header"; return bundled; }
    }
    if (bk.empty()) { if (err) *err = "no key column"; return bundled; }
    std::vector<int> map(bHead.size(), -1);   // bundled column -> user column
    for (size_t i = 0; i < bHead.size(); i++) for (size_t j = 0; j < uHead.size(); j++) if (trimS(uHead[j]) == trimS(bHead[i])) map[i] = (int)j;
    for (size_t c : bk) if (map[c] < 0) { if (err) *err = "the file has no key column \"" + trimS(bHead[c]) + "\""; return bundled; }
    auto keyOf = [&](const std::vector<std::string>& f) { std::string k; for (size_t c : bk) k += trimS(f[c]) + "\x1f"; return k; };
    std::vector<std::string> out(B.begin(), B.end());
    std::unordered_map<std::string, size_t> at;
    for (size_t i = bh + 1; i < B.size(); i++) {
        if (B[i].empty() || B[i][0] == '#') continue;
        const auto f = splitC(B[i], delim);
        if (f.size() == bHead.size()) at[keyOf(f)] = i;
    }
    for (size_t i = uh + 1; i < U.size(); i++) {
        if (U[i].empty() || U[i][0] == '#') continue;
        const auto uf = splitC(U[i], delim);
        if (uf.size() != uHead.size()) continue;
        std::vector<std::string> f(bHead.size());
        for (size_t c = 0; c < f.size(); c++) if (map[c] >= 0) f[c] = uf[(size_t)map[c]];
        const std::string line = joinC(f, delim), k = keyOf(f);
        const auto it = at.find(k);
        if (it != at.end()) out[it->second] = line;
        else { at[k] = out.size(); out.push_back(line); }
    }
    std::string o;
    for (const std::string& l : out) { o += l; o += '\n'; }
    return o;
}

std::string layerGrouped(const std::string& bundled, const std::string& user, const std::string& startTag, size_t keyField, char delim) {
    struct Rec { std::string key; std::vector<std::string> ls; };
    auto groups = [&](const std::string& t, std::vector<std::string>& head) {
        std::vector<Rec> r;
        for (const std::string& l : lines(t)) {
            if (l.empty()) continue;
            const auto f = splitC(l, delim);
            if (f[0] == startTag) { r.push_back({keyField < f.size() ? f[keyField] : std::string(), {l}}); }
            else if (!r.empty() && l[0] != '#') r.back().ls.push_back(l);
            else if (r.empty()) head.push_back(l);
        }
        return r;
    };
    std::vector<std::string> head, uhead;
    std::vector<Rec> B = groups(bundled, head);
    const std::vector<Rec> U = groups(user, uhead);
    std::unordered_map<std::string, size_t> at;
    for (size_t i = 0; i < B.size(); i++) at[B[i].key] = i;
    for (const Rec& u : U) {
        if (u.key.empty()) continue;
        const auto it = at.find(u.key);
        if (it != at.end()) B[it->second] = u;
        else { at[u.key] = B.size(); B.push_back(u); }
    }
    std::string o;
    for (const std::string& l : head) o += l + "\n";
    for (const Rec& r : B) for (const std::string& l : r.ls) o += l + "\n";
    return o;
}

// ---------------------------------------------------------------- the user's layer on disk
namespace {
std::string gDir;
fs::path dirOf(const std::string& id) { return dect2::u8file(gDir + "/" + id); }
bool readAll(const fs::path& p, std::string& o) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    std::stringstream ss;
    ss << f.rdbuf();
    o = ss.str();
    return true;
}
bool writeAtomic(const fs::path& p, const std::string& body, std::string& err) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    const fs::path tmp = p.string() + ".part";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        f.write(body.data(), (std::streamsize)body.size());
        if (!f) { err = "cannot write " + p.string(); return false; }
    }
    fs::rename(tmp, p, ec);
    if (ec) { err = "cannot write " + p.string() + ": " + ec.message(); return false; }
    return true;
}
bool safeId(const std::string& id) { return validDatasetId(id); }
}  // namespace

void setUserDir(const std::string& dir) { gDir = dir; }
const std::string& userDir() { return gDir; }
std::string userCopyPath(const std::string& id) { return gDir.empty() || !safeId(id) ? std::string() : gDir + "/" + id + "/override.dat"; }
bool hasUserCopy(const std::string& id) { if (gDir.empty() || !safeId(id)) return false; std::error_code ec; return fs::is_regular_file(dirOf(id) / "override.dat", ec); }
unsigned long long userCopyBytes(const std::string& id) { if (!hasUserCopy(id)) return 0; std::error_code ec; const auto n = fs::file_size(dirOf(id) / "override.dat", ec); return ec ? 0 : (unsigned long long)n; }
std::string userText(const std::string& id) { std::string t; if (!hasUserCopy(id) || !readAll(dirOf(id) / "override.dat", t)) return std::string(); return t; }

bool readMeta(const std::string& id, UserMeta& m) {
    m = UserMeta();
    std::string t;
    if (gDir.empty() || !safeId(id) || !readAll(dirOf(id) / "meta.txt", t)) return false;
    for (const std::string& l : lines(t)) {
        const size_t c = l.find('=');
        if (c == std::string::npos) continue;
        const std::string k = l.substr(0, c), val = l.substr(c + 1);
        if (k == "version") m.version = val; else if (k == "from") m.from = val; else if (k == "sha256") m.sha256 = val; else if (k == "when") m.when = std::atoll(val.c_str());
    }
    return true;
}

bool importText(const Descriptor& d, const std::string& text, const std::string& from, const std::string& version, Validation& v, std::string& err) {
    if (!d.replaceable) { err = d.name + " cannot be replaced"; return false; }
    if (gDir.empty() || !safeId(d.id)) { err = "no data folder"; return false; }
    v = validateText(d.schema, text);
    if (!v.ok()) { err = std::to_string(v.errors()) + " error(s); nothing was imported"; return false; }
    if (!writeAtomic(dirOf(d.id) / "override.dat", text, err)) return false;
    std::string meta = "version=" + version + "\nfrom=" + from + "\nsha256=" + dect2::sha256Bytes(text.data(), text.size()) + "\nwhen=" +
                       std::to_string((long long)std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count()) + "\n";
    return writeAtomic(dirOf(d.id) / "meta.txt", meta, err);
}

bool importFile(const Descriptor& d, const std::string& path, Validation& v, std::string& err, const std::string& version) {
    std::string t;
    std::error_code ec;
    if (!fs::is_regular_file(dect2::u8file(path), ec) || !readAll(dect2::u8file(path), t)) { err = "cannot read " + path; return false; }
    if (t.size() > (64u << 20)) { err = "larger than 64 MB"; return false; }
    return importText(d, t, fs::path(dect2::u8file(path)).filename().string(), version, v, err);
}

bool importFolder(const Descriptor& d, const std::string& folder, Validation& v, std::string& err) {
    std::error_code ec;
    if (!fs::is_directory(dect2::u8file(folder), ec)) { err = "not a folder: " + folder; return false; }
    std::vector<fs::path> files;
    for (const auto& e : fs::directory_iterator(dect2::u8file(folder), ec)) {
        if (!e.is_regular_file()) continue;
        std::string x = lower(e.path().extension().string());
        if (x == ".tsv" || x == ".csv" || x == ".txt" || x == ".json") files.push_back(e.path());
    }
    std::sort(files.begin(), files.end());
    if (files.empty()) { err = "no .tsv, .csv, .txt or .json file in the folder"; return false; }
    if (d.schema.format == Format::BandplanJson || d.schema.format == Format::Delimited) {
        if (files.size() > 1) { err = "this dataset is one file; the folder has " + std::to_string(files.size()); return false; }
    }
    std::string all;
    for (const fs::path& p : files) { std::string t; if (!readAll(p, t)) { err = "cannot read " + p.string(); return false; } all += t; if (all.empty() || all.back() != '\n') all += '\n'; }
    return importText(d, all, fs::path(dect2::u8file(folder)).filename().string() + "/", "", v, err);
}

bool resetToBundled(const std::string& id, std::string& err) {
    if (gDir.empty() || !safeId(id)) { err = "bad dataset id"; return false; }
    std::error_code ec;
    fs::remove(dirOf(id) / "override.dat", ec);
    fs::remove(dirOf(id) / "meta.txt", ec);
    fs::remove(dirOf(id), ec);
    return true;
}

std::string effectiveText(const Descriptor& d) {
    std::string b = d.bundledText ? d.bundledText() : std::string();
    const std::string u = userText(d.id);
    if (u.empty()) return b;
    switch (d.schema.format) {
        case Format::AirportsTsv: return layerGrouped(b, u, "A", 1);
        case Format::Delimited: { std::string e; return layerDelimited(b, u, d.schema.delim, d.schema.keyColumns, &e); }
        case Format::FreqTsv: return b + "\n@source Your own records\n" + u;   // export view; the loader layers by key
        default: return u;
    }
}

bool exportEffective(const Descriptor& d, const std::string& path, std::string& err) {
    const std::string t = effectiveText(d);
    if (t.empty()) { err = "nothing to export"; return false; }
    return writeAtomic(dect2::u8file(path), t, err);
}
bool exportBundled(const Descriptor& d, const std::string& path, std::string& err) {
    const std::string t = d.bundledText ? d.bundledText() : std::string();
    if (t.empty()) { err = "no bundled text"; return false; }
    return writeAtomic(dect2::u8file(path), t, err);
}

Status statusOf(const Descriptor& d, const std::map<std::string, std::string>& offeredNewer) {
    if (d.downloadable) return Status::Download;
    if (offeredNewer.count(d.id)) return Status::Outdated;
    return hasUserCopy(d.id) ? Status::UserInstalled : Status::Bundled;
}

}  // namespace dect2::datasets
