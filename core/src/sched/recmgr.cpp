// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/sched/recmgr.h"
#include "dect2/automation/json_mini.h"
#include "dect2/common/u8file.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace dect2::recmgr {
namespace fs = std::filesystem;
namespace js = dect2::automation::json;

namespace {
std::string lower(std::string s) { for (char& c : s) c = (char)tolower((unsigned char)c); return s; }
bool endsWith(const std::string& s, const std::string& e) { return s.size() >= e.size() && s.compare(s.size() - e.size(), e.size(), e) == 0; }
std::string ext(const std::string& p) {
    const std::string n = lower(fs::path(u8file(p)).filename().string());
    if (endsWith(n, ".sigmf-meta")) return ".sigmf-meta";
    if (endsWith(n, ".sigmf-data")) return ".sigmf-data";
    const size_t d = n.rfind('.');
    return d == std::string::npos ? "" : n.substr(d);
}
bool readFile(const std::string& p, std::string& out, size_t maxBytes = 4u << 20) {
    FILE* f = u8fopen(p, "rb");
    if (!f) return false;
    char buf[8192]; size_t n;
    out.clear();
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) { out.append(buf, n); if (out.size() > maxBytes) break; }
    fclose(f);
    return true;
}
bool writeFile(const std::string& p, const std::string& data, std::string& err) {
    FILE* f = u8fopen(p, "wb");
    if (!f) { err = "cannot write " + p; return false; }
    const bool ok = fwrite(data.data(), 1, data.size(), f) == data.size();
    if (fclose(f) != 0 || !ok) { err = "write failed: " + p; return false; }
    return true;
}
uint64_t fileSize(const std::string& p) { std::error_code ec; const auto s = fs::file_size(u8file(p), ec); return ec ? 0 : (uint64_t)s; }
int64_t fileTime(const std::string& p) {
    std::error_code ec;
    const auto t = fs::last_write_time(u8file(p), ec);
    if (ec) return 0;
    const auto sys = std::chrono::time_point_cast<std::chrono::seconds>(t - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
    return (int64_t)sys.time_since_epoch().count();
}
uint32_t rd32(const unsigned char* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
uint16_t rd16(const unsigned char* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
void wr32(std::string& s, uint32_t v) { for (int i = 0; i < 4; i++) s += (char)(v >> (8 * i)); }
void wr16(std::string& s, uint16_t v) { s += (char)v; s += (char)(v >> 8); }
}  // namespace

const char* kindName(Kind k) {
    switch (k) {
    case Kind::Iq: return "IQ";
    case Kind::Audio: return "audio";
    case Kind::Video: return "video";
    case Kind::Image: return "image";
    case Kind::Log: return "log";
    case Kind::SigMF: return "SigMF";
    case Kind::Other: return "other";
    }
    return "?";
}

Kind classify(const std::string& path) {
    const std::string e = ext(path);
    if (e == ".sigmf-meta") return Kind::SigMF;
    for (const char* x : {".cs8", ".cu8", ".cf32", ".ci16", ".cu16", ".iq", ".raw", ".sigmf-data", ".fc32", ".c16"}) if (e == x) return Kind::Iq;
    for (const char* x : {".wav", ".flac", ".mp3", ".ogg", ".opus", ".m4a", ".aac"}) if (e == x) return Kind::Audio;
    for (const char* x : {".ts", ".mp4", ".mkv", ".m2ts", ".mpg"}) if (e == x) return Kind::Video;
    for (const char* x : {".png", ".jpg", ".jpeg", ".gif", ".bmp", ".tif", ".tiff"}) if (e == x) return Kind::Image;
    for (const char* x : {".txt", ".log", ".csv", ".json", ".jsonl", ".kml", ".nmea"}) if (e == x) return Kind::Log;
    return Kind::Other;
}

bool Entry::isProtected() const {
    if (favourite) return true;
    for (const std::string& t : tags) { const std::string l = lower(t); if (l == "favourite" || l == "favorite" || l == "keep") return true; }
    return false;
}

// ---- the index -----------------------------------------------------------------------------------------------------------------------
std::string Index::toJson() const {
    js::Value root = js::Value::object();
    root.set("version", js::Value::number(1));
    js::Value files = js::Value::object();
    for (const auto& [k, m] : meta) {
        js::Value v = js::Value::object();
        js::Value t = js::Value::array();
        for (const std::string& s : m.tags) t.push(js::Value::string(s));
        t.type = js::Value::Arr;
        v.set("tags", t); v.set("notes", js::Value::string(m.notes)); v.set("mode", js::Value::string(m.mode));
        v.set("freqHz", js::Value::number(m.freqHz)); v.set("favourite", js::Value::boolean(m.favourite));
        files.set(k, v);
    }
    files.type = js::Value::Obj;
    root.set("files", files);
    return js::dump(root, true);
}

bool Index::fromJson(const std::string& text, std::string& err) {
    js::Value root;
    meta.clear();
    if (!js::parse(text, root, err)) return false;
    const js::Value* f = root.get("files");
    if (!f || f->type != js::Value::Obj) { err = "no files object"; return false; }
    for (const auto& [k, v] : f->o) {
        Meta m;
        if (const js::Value* t = v.get("tags")) for (const js::Value& x : t->a) if (x.type == js::Value::Str) m.tags.push_back(x.s);
        m.notes = v.str("notes"); m.mode = v.str("mode"); m.freqHz = v.num("freqHz"); m.favourite = v.flag("favourite");
        meta[k] = std::move(m);
    }
    return true;
}

bool Index::load(const std::string& path) {
    std::string t, err;
    meta.clear();
    return readFile(path, t) && fromJson(t, err);
}
bool Index::save(const std::string& path, std::string& err) const { return writeFile(path, toJson(), err); }
void Index::rename(const std::string& a, const std::string& b) {
    auto it = meta.find(a);
    if (it == meta.end() || a == b) return;
    meta[b] = std::move(it->second);
    meta.erase(a);
}

// ---- scanning ----------------------------------------------------------------------------------------------------------------------
std::string relKey(const std::string& root, const std::string& path) {
    std::error_code ec;
    std::string r = fs::relative(u8file(path), u8file(root), ec).generic_string();
    if (ec || r.empty()) r = path;
    return r;
}

static double rateFromName(const std::string& name) {
    const std::string n = lower(name);
    for (size_t i = 0; i < n.size(); i++) {
        if (!isdigit((unsigned char)n[i])) continue;
        size_t j = i;
        while (j < n.size() && (isdigit((unsigned char)n[j]) || n[j] == '.')) j++;
        const double v = atof(n.substr(i, j - i).c_str());
        if (n.compare(j, 4, "msps") == 0) return v * 1e6;
        if (n.compare(j, 4, "ksps") == 0) return v * 1e3;
        i = j;
    }
    return 0;
}

static int rawBytesPerSample(const std::string& e) {
    if (e == ".cs8" || e == ".cu8") return 2;
    if (e == ".cf32" || e == ".fc32") return 8;
    if (e == ".ci16" || e == ".cu16" || e == ".c16") return 4;
    return 0;
}

std::vector<Entry> scan(const std::string& root, const Index& idx, int maxDepth) {
    std::vector<Entry> out;
    std::error_code ec;
    if (!fs::is_directory(u8file(root), ec)) return out;
    std::vector<std::pair<fs::path, int>> stack{{u8file(root), 0}};
    while (!stack.empty()) {
        auto [dir, depth] = stack.back();
        stack.pop_back();
        for (fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
            const fs::path p = it->path();
            const std::string name = p.filename().string();
            std::error_code e2;
            if (it->is_directory(e2)) {
                if (depth + 1 < maxDepth && name != ".trash" && name != "trash" && !name.empty() && name[0] != '.') stack.push_back({p, depth + 1});
                continue;
            }
            if (!it->is_regular_file(e2) || name == "recindex.json" || name == "journal.json" || (!name.empty() && name[0] == '.')) continue;
            const std::string path = p.string();
            const std::string e = ext(path);
            if (e == ".sigmf-data") {   // listed with its meta when there is one
                std::string m = path.substr(0, path.size() - 4) + "meta";
                if (fs::exists(u8file(m), e2)) continue;
            }
            Entry en;
            en.path = path; en.name = name; en.kind = classify(path);
            en.mtime = fileTime(path); en.bytes = fileSize(path); en.files = {path};
            const std::string key = relKey(root, path);
            const size_t sl = key.find('/');
            en.folder = sl == std::string::npos ? "" : key.substr(0, sl);
            if (e == ".sigmf-meta") {
                std::string t, perr; js::Value v;
                const std::string data = path.substr(0, path.size() - 4) + "data";
                if (fs::exists(u8file(data), e2)) { en.files.push_back(data); en.bytes += fileSize(data); en.mtime = std::max(en.mtime, fileTime(data)); }
                if (readFile(path, t, 1u << 20) && js::parse(t, v, perr)) {
                    if (const js::Value* g = v.get("global")) {
                        en.datatype = g->str("core:datatype"); en.sampleRate = g->num("core:sample_rate");
                        const std::string d = g->str("core:description");
                        if (!d.empty() && d.size() <= 48) en.mode = d;
                    }
                    if (const js::Value* c = v.get("captures")) if (!c->a.empty()) en.freqHz = c->a[0].num("core:frequency");
                    const int bps = bytesPerComplexSample(en.datatype);
                    if (bps > 0 && en.files.size() > 1) {
                        en.samples = fileSize(data) / bps;
                        if (en.sampleRate > 0) en.seconds = (double)en.samples / en.sampleRate;
                    }
                }
            } else if (en.kind == Kind::Iq) {
                const int bps = rawBytesPerSample(e);
                en.sampleRate = rateFromName(name);
                if (bps > 0) {
                    en.samples = en.bytes / bps;
                    if (en.sampleRate > 0) en.seconds = (double)en.samples / en.sampleRate;
                }
            } else if (e == ".wav") {
                WavInfo wi; std::string err;
                if (readWavInfo(path, wi, err) && wi.rate) { en.sampleRate = wi.rate; en.seconds = (double)wi.frames() / wi.rate; }
            }
            auto mi = idx.meta.find(key);
            if (mi != idx.meta.end()) {
                en.tags = mi->second.tags; en.notes = mi->second.notes; en.favourite = mi->second.favourite;
                if (!mi->second.mode.empty()) en.mode = mi->second.mode;
                if (mi->second.freqHz > 0) en.freqHz = mi->second.freqHz;
            }
            out.push_back(std::move(en));
        }
    }
    std::sort(out.begin(), out.end(), [](const Entry& a, const Entry& b) { return a.mtime != b.mtime ? a.mtime > b.mtime : a.path < b.path; });
    return out;
}

// ---- search and sort -----------------------------------------------------------------------------------------------------------------
namespace {
bool parseQty(const std::string& t, double& v, char& what) {   // "100MB" bytes, "10min" seconds
    char* e = nullptr;
    v = strtod(t.c_str(), &e);
    if (e == t.c_str()) return false;
    const std::string u = lower(e);
    if (u == "kb") { v *= 1e3; what = 'b'; } else if (u == "mb") { v *= 1e6; what = 'b'; } else if (u == "gb") { v *= 1e9; what = 'b'; }
    else if (u == "s") what = 't'; else if (u == "min" || u == "m") { v *= 60; what = 't'; } else if (u == "h") { v *= 3600; what = 't'; }
    else return false;
    return true;
}
}  // namespace

bool matches(const Entry& e, const std::string& query) {
    size_t i = 0;
    const std::string hay = lower(e.name + " " + e.mode + " " + e.notes + " " + e.folder);
    while (i < query.size()) {
        while (i < query.size() && isspace((unsigned char)query[i])) i++;
        size_t j = i;
        while (j < query.size() && !isspace((unsigned char)query[j])) j++;
        if (j == i) break;
        const std::string w = lower(query.substr(i, j - i));
        i = j;
        if (w == "fav") { if (!e.isProtected()) return false; continue; }
        if (w.rfind("tag:", 0) == 0) {
            bool ok = false;
            for (const std::string& t : e.tags) if (lower(t) == w.substr(4)) ok = true;
            if (!ok) return false;
            continue;
        }
        if (w.rfind("kind:", 0) == 0) { if (lower(kindName(e.kind)) != w.substr(5)) return false; continue; }
        if (w.rfind("mode:", 0) == 0) { if (lower(e.mode).find(w.substr(5)) == std::string::npos) return false; continue; }
        if ((w[0] == '>' || w[0] == '<') && w.size() > 1) {
            double v; char what;
            if (parseQty(w.substr(1), v, what)) {
                const double have = what == 'b' ? (double)e.bytes : e.seconds;
                if (w[0] == '>' ? !(have > v) : !(have < v)) return false;
                continue;
            }
        }
        bool inTag = false;
        for (const std::string& t : e.tags) if (lower(t).find(w) != std::string::npos) inTag = true;
        if (!inTag && hay.find(w) == std::string::npos) return false;
    }
    return true;
}

void sortEntries(std::vector<Entry>& v, Column c, bool asc) {
    auto key = [c](const Entry& a, const Entry& b) {   // a before b when "less"
        switch (c) {
        case Column::Name: return lower(a.name) < lower(b.name);
        case Column::Date: return a.mtime < b.mtime;
        case Column::Mode: return lower(a.mode) < lower(b.mode);
        case Column::Frequency: return a.freqHz < b.freqHz;
        case Column::Length: return a.seconds < b.seconds;
        case Column::Size: return a.bytes < b.bytes;
        case Column::Kind: return (int)a.kind < (int)b.kind;
        }
        return false;
    };
    std::stable_sort(v.begin(), v.end(), [&](const Entry& a, const Entry& b) { return asc ? key(a, b) : key(b, a); });
}

// ---- cleanup -------------------------------------------------------------------------------------------------------------------------
CleanupPlan planCleanup(const std::vector<Entry>& entries, const CleanupRule& rule, int64_t now) {
    CleanupPlan plan;
    std::vector<size_t> order(entries.size());
    for (size_t i = 0; i < order.size(); i++) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return entries[a].mtime > entries[b].mtime; });   // newest first
    uint64_t kept = 0;
    bool over = false;
    for (size_t i : order) {
        const Entry& e = entries[i];
        bool del = false;
        if (!e.isProtected()) {
            if (rule.olderThanDays > 0 && e.mtime < now - (int64_t)rule.olderThanDays * 86400) del = true;
            if (rule.keepNewestBytes > 0 && (over || kept + e.bytes > rule.keepNewestBytes)) { del = true; over = true; }
        }
        if (del) { plan.remove.push_back(i); plan.freedBytes += e.bytes; }
        else {
            kept += e.bytes;
            if (rule.keepNewestBytes > 0 && kept > rule.keepNewestBytes) over = true;
        }
    }
    return plan;
}

CleanupPlan planCleanupByFolder(const std::vector<Entry>& entries, const std::function<CleanupRule(const std::string&)>& rule, int64_t now) {
    std::map<std::string, std::vector<size_t>> groups;
    for (size_t i = 0; i < entries.size(); i++) groups[entries[i].folder].push_back(i);
    CleanupPlan all;
    for (auto& [folder, ids] : groups) {
        std::vector<Entry> sub;
        for (size_t i : ids) sub.push_back(entries[i]);
        const CleanupPlan p = planCleanup(sub, rule(folder), now);
        for (size_t k : p.remove) all.remove.push_back(ids[k]);
        all.freedBytes += p.freedBytes;
    }
    std::sort(all.remove.begin(), all.remove.end());
    return all;
}

// ---- trimming ------------------------------------------------------------------------------------------------------------------------
bool trimRange(double startSec, double endSec, double rate, uint64_t total, SampleRange& out, std::string& err) {
    if (!(rate > 0)) { err = "the sample rate is unknown"; return false; }
    if (startSec < 0) startSec = 0;
    const double a = std::llround(startSec * rate), b = std::llround(endSec * rate);
    out.first = (uint64_t)std::min<double>(a, (double)total);
    out.end = (uint64_t)std::min<double>(std::max(b, 0.0), (double)total);
    if (out.end <= out.first) { err = "the time range is empty"; return false; }
    return true;
}

int bytesPerComplexSample(const std::string& dt) {
    if (dt.rfind("cf32", 0) == 0) return 8;
    if (dt.rfind("ci16", 0) == 0 || dt.rfind("cu16", 0) == 0) return 4;
    if (dt.rfind("ci8", 0) == 0 || dt.rfind("cu8", 0) == 0) return 2;
    return 0;
}

static bool copyRange(const std::string& src, const std::string& dst, uint64_t off, uint64_t len, std::string& err, const std::string* header = nullptr) {
    FILE* in = u8fopen(src, "rb");
    if (!in) { err = "cannot read " + src; return false; }
    FILE* out = u8fopen(dst, "wb");
    if (!out) { fclose(in); err = "cannot write " + dst; return false; }
    bool ok = true;
#ifdef _WIN32
    ok = _fseeki64(in, (long long)off, SEEK_SET) == 0;
#else
    ok = fseeko(in, (off_t)off, SEEK_SET) == 0;
#endif
    if (header && ok) ok = fwrite(header->data(), 1, header->size(), out) == header->size();
    std::vector<char> buf(1 << 20);
    uint64_t left = len;
    while (ok && left > 0) {
        const size_t want = (size_t)std::min<uint64_t>(left, buf.size());
        const size_t n = fread(buf.data(), 1, want, in);
        if (n == 0) { err = "the file is shorter than it says"; ok = false; break; }
        if (fwrite(buf.data(), 1, n, out) != n) { err = "write failed (disk full?)"; ok = false; break; }
        left -= n;
    }
    fclose(in);
    if (fclose(out) != 0 && ok) { err = "write failed"; ok = false; }
    if (!ok) { std::error_code ec; fs::remove(u8file(dst), ec); }
    return ok;
}

bool trimRawIq(const std::string& src, const std::string& dst, int bps, SampleRange r, std::string& err) {
    if (bps <= 0) { err = "unknown sample size"; return false; }
    return copyRange(src, dst, r.first * bps, (r.end - r.first) * bps, err);
}

bool trimSigmf(const std::string& metaPath, const std::string& dstBase, double s, double e, SampleRange* used, std::string& err) {
    std::string t; js::Value v;
    if (!readFile(metaPath, t, 1u << 20) || !js::parse(t, v, err)) { if (err.empty()) err = "cannot read " + metaPath; return false; }
    const js::Value* g = v.get("global");
    if (!g) { err = "no global section"; return false; }
    const std::string dt = g->str("core:datatype");
    const int bps = bytesPerComplexSample(dt);
    if (!bps) { err = "not complex IQ: " + dt; return false; }
    const double rate = g->num("core:sample_rate");
    const std::string data = metaPath.substr(0, metaPath.size() - 4) + "data";
    const uint64_t total = fileSize(data) / bps;
    SampleRange r;
    if (!trimRange(s, e, rate, total, r, err)) return false;
    if (!trimRawIq(data, dstBase + ".sigmf-data", bps, r, err)) return false;
    js::Value n = v;
    js::Value cap = js::Value::object();
    cap.set("core:sample_start", js::Value::number(0));
    if (const js::Value* c = v.get("captures")) if (!c->a.empty()) { cap = c->a[0]; cap.set("core:sample_start", js::Value::number(0)); }
    js::Value caps = js::Value::array(); caps.push(cap);
    n.set("captures", caps);
    n.o.erase("annotations");
    js::Value ng = *g;
    char b[96];
    snprintf(b, sizeof b, " [trimmed: samples %llu to %llu of %llu]", (unsigned long long)r.first, (unsigned long long)r.end, (unsigned long long)total);
    ng.set("core:description", js::Value::string(g->str("core:description") + b));
    n.set("global", ng);
    if (!writeFile(dstBase + ".sigmf-meta", js::dump(n, true), err)) return false;
    if (used) *used = r;
    return true;
}

bool readWavInfo(const std::string& path, WavInfo& w, std::string& err) {
    FILE* f = u8fopen(path, "rb");
    if (!f) { err = "cannot read " + path; return false; }
    unsigned char h[12];
    if (fread(h, 1, 12, f) != 12 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4)) { fclose(f); err = "not a WAV file"; return false; }
    uint64_t pos = 12;
    bool haveFmt = false;
    w = WavInfo();
    unsigned char c[8];
    while (fread(c, 1, 8, f) == 8) {
        const uint32_t len = rd32(c + 4);
        pos += 8;
        if (!memcmp(c, "fmt ", 4) && len >= 16) {
            unsigned char b[16];
            if (fread(b, 1, 16, f) != 16) break;
            w.format = rd16(b); w.channels = rd16(b + 2); w.rate = rd32(b + 4); w.bits = rd16(b + 14);
            haveFmt = true;
            if (len > 16) fseek(f, (long)(len - 16), SEEK_CUR);
        } else if (!memcmp(c, "data", 4)) {
            if (!haveFmt) break;
            w.dataOffset = pos;
            const uint64_t avail = fileSize(path) > pos ? fileSize(path) - pos : 0;
            w.dataBytes = std::min<uint64_t>(len, avail);   // a recording that was cut off keeps what is there
            fclose(f);
            return true;
        } else fseek(f, (long)len, SEEK_CUR);
        pos += len;
        if (len & 1) { fseek(f, 1, SEEK_CUR); pos++; }
    }
    fclose(f);
    err = "no audio data in the WAV file";
    return false;
}

bool trimWav(const std::string& src, const std::string& dst, double s, double e, SampleRange* used, std::string& err) {
    WavInfo w;
    if (!readWavInfo(src, w, err)) return false;
    if (w.format != 1 && w.format != 3 && w.format != 0xFFFE) { err = "only PCM and float WAV files can be trimmed"; return false; }
    SampleRange r;
    if (!trimRange(s, e, w.rate, w.frames(), r, err)) return false;
    const uint32_t fb = (uint32_t)((w.bits / 8) * w.channels);
    const uint64_t bytes = (r.end - r.first) * fb;
    if (bytes > 0xFFFFFFF0ull - 44) { err = "too long for a WAV file"; return false; }
    std::string h = "RIFF";
    wr32(h, (uint32_t)(36 + bytes)); h += "WAVEfmt "; wr32(h, 16);
    wr16(h, (uint16_t)(w.format == 0xFFFE ? 1 : w.format)); wr16(h, (uint16_t)w.channels); wr32(h, w.rate); wr32(h, w.rate * fb); wr16(h, (uint16_t)fb); wr16(h, (uint16_t)w.bits);
    h += "data"; wr32(h, (uint32_t)bytes);
    if (!copyRange(src, dst, w.dataOffset + r.first * fb, bytes, err, &h)) return false;
    if (used) *used = r;
    return true;
}

// ---- export, convert -------------------------------------------------------------------------------------------------------------------
bool writeSigmfSidecar(const std::string& metaPath, const std::string& datatype, double rate, double freqHz, const std::string& desc, std::string& err) {
    if (!bytesPerComplexSample(datatype)) { err = "unknown SigMF datatype " + datatype; return false; }
    js::Value root = js::Value::object(), g = js::Value::object(), cap = js::Value::object(), caps = js::Value::array();
    g.set("core:datatype", js::Value::string(datatype)); g.set("core:sample_rate", js::Value::number(rate));
    g.set("core:version", js::Value::string("1.0.0")); g.set("core:description", js::Value::string(desc));
    cap.set("core:sample_start", js::Value::number(0));
    if (freqHz > 0) cap.set("core:frequency", js::Value::number(freqHz));
    caps.push(cap);
    js::Value ann = js::Value::array(); ann.type = js::Value::Arr;
    root.set("global", g); root.set("captures", caps); root.set("annotations", ann);
    return writeFile(metaPath, js::dump(root, true), err);
}

bool convertIq(const std::string& src, const std::string& sf, const std::string& dst, const std::string& df, std::string& err) {
    auto bps = [](const std::string& f) { return f == "cs8" || f == "cu8" ? 2 : f == "ci16" ? 4 : f == "cf32" ? 8 : 0; };
    const int sb = bps(sf), db = bps(df);
    if (!sb || !db) { err = "formats are cs8, cu8, ci16, cf32"; return false; }
    FILE* in = u8fopen(src, "rb");
    if (!in) { err = "cannot read " + src; return false; }
    FILE* out = u8fopen(dst, "wb");
    if (!out) { fclose(in); err = "cannot write " + dst; return false; }
    const size_t N = 1 << 16;
    std::vector<unsigned char> ib(N * sb), ob(N * db);
    bool ok = true;
    size_t n;
    while (ok && (n = fread(ib.data(), sb, N, in)) > 0) {
        for (size_t k = 0; k < n * 2; k++) {
            float x;
            const unsigned char* p = ib.data() + k * (sb / 2);
            if (sf == "cs8") x = (int8_t)p[0] / 127.0f;
            else if (sf == "cu8") x = (p[0] - 127.5f) / 127.5f;
            else if (sf == "ci16") x = (int16_t)rd16(p) / 32768.0f;
            else { uint32_t u = rd32(p); memcpy(&x, &u, 4); }
            unsigned char* q = ob.data() + k * (db / 2);
            if (df == "cs8") q[0] = (unsigned char)(int8_t)std::lround(std::max(-1.0f, std::min(1.0f, x)) * 127.0f);
            else if (df == "cu8") q[0] = (unsigned char)std::lround(std::max(-1.0f, std::min(1.0f, x)) * 127.5f + 127.5f);
            else if (df == "ci16") { const int16_t v = (int16_t)std::lround(std::max(-1.0f, std::min(1.0f, x)) * 32767.0f); q[0] = (unsigned char)v; q[1] = (unsigned char)((uint16_t)v >> 8); }
            else memcpy(q, &x, 4);
        }
        if (fwrite(ob.data(), db, n, out) != n) { err = "write failed (disk full?)"; ok = false; }
    }
    fclose(in);
    if (fclose(out) != 0 && ok) { err = "write failed"; ok = false; }
    if (!ok) { std::error_code ec; fs::remove(u8file(dst), ec); }
    return ok;
}

bool copyFile(const std::string& src, const std::string& dst, std::string& err) {
    std::error_code ec;
    if (fs::exists(u8file(dst), ec)) { err = "the target exists: " + dst; return false; }
    fs::copy_file(u8file(src), u8file(dst), ec);
    if (ec) { err = ec.message(); return false; }
    return true;
}

// ---- rename, trash ---------------------------------------------------------------------------------------------------------------------
bool renameEntry(const Entry& e, const std::string& base, std::string& newPath, std::string& err) {
    std::string b;
    for (unsigned char c : base) b += (c < 32 || c == 127 || strchr("<>:\"|?*/\\", c)) ? '_' : (char)c;
    while (!b.empty() && (b.back() == '.' || b.back() == ' ')) b.pop_back();
    size_t st = 0;
    while (st < b.size() && b[st] == ' ') st++;
    b = b.substr(st);
    if (b.empty() || b[0] == '.') { err = "the name is empty"; return false; }
    // the extension (".sigmf-meta" / ".sigmf-data" together, otherwise the last one) stays
    std::vector<std::pair<fs::path, fs::path>> moves;
    for (const std::string& f : e.files) {
        const fs::path p = u8file(f);
        const std::string x = ext(f);
        moves.push_back({p, p.parent_path() / fs::path(u8file(b + x))});
    }
    std::error_code ec;
    for (auto& m : moves) {
        if (m.first == m.second) { err = "that is the name it has"; return false; }
        if (fs::exists(m.second, ec)) { err = "a file with that name exists"; return false; }
    }
    size_t done = 0;
    for (auto& m : moves) {
        fs::rename(m.first, m.second, ec);
        if (ec) { err = ec.message(); for (size_t i = 0; i < done; i++) fs::rename(moves[i].second, moves[i].first, ec); return false; }
        done++;
    }
    newPath = moves[0].second.string();
    return true;
}

namespace {
std::atomic<int> gGroup{0};
std::string journalPath(const std::string& d) { return (u8file(d) / "journal.json").string(); }
void writeJournal(const std::string& d, const std::vector<TrashItem>& items) {
    js::Value root = js::Value::object(), arr = js::Value::array();
    for (const TrashItem& t : items) {
        js::Value v = js::Value::object();
        v.set("group", js::Value::string(t.group)); v.set("original", js::Value::string(t.original)); v.set("stored", js::Value::string(t.stored));
        v.set("when", js::Value::number((double)t.when));
        arr.push(v);
    }
    arr.type = js::Value::Arr;
    root.set("items", arr);
    std::string err;
    writeFile(journalPath(d), js::dump(root, true), err);
}
bool moveFile(const fs::path& a, const fs::path& b, std::string& err) {
    std::error_code ec;
    fs::rename(a, b, ec);
    if (!ec) return true;
    fs::copy_file(a, b, fs::copy_options::none, ec);   // another volume
    if (ec) { err = ec.message(); return false; }
    fs::remove(a, ec);
    return true;
}
}  // namespace

std::vector<TrashItem> listTrash(const std::string& d) {
    std::vector<TrashItem> r;
    std::string t, err;
    js::Value v;
    if (!readFile(journalPath(d), t) || !js::parse(t, v, err)) return r;
    if (const js::Value* a = v.get("items")) for (const js::Value& x : a->a) r.push_back({x.str("group"), x.str("original"), x.str("stored"), (int64_t)x.num("when")});
    return r;
}

bool moveToTrash(const std::vector<std::string>& files, const std::string& dir, int64_t now, std::string& group, std::string& err) {
    std::error_code ec;
    fs::create_directories(u8file(dir), ec);
    if (ec) { err = "cannot create the trash folder: " + ec.message(); return false; }
    group = "g" + std::to_string(now) + "_" + std::to_string(++gGroup);
    std::vector<TrashItem> items = listTrash(dir), added;
    int n = 0;
    for (const std::string& f : files) {
        const fs::path stored = u8file(dir) / group / (std::to_string(n++) + "_" + fs::path(u8file(f)).filename().string());
        fs::create_directories(stored.parent_path(), ec);
        if (!moveFile(u8file(f), stored, err)) {
            for (auto& a : added) { std::string e2; moveFile(u8file(a.stored), u8file(a.original), e2); }   // undo what was moved
            return false;
        }
        added.push_back({group, f, stored.string(), now});
    }
    items.insert(items.end(), added.begin(), added.end());
    writeJournal(dir, items);
    return true;
}

bool restoreFromTrash(const std::string& dir, const std::string& group, std::string& err) {
    std::vector<TrashItem> items = listTrash(dir), mine, rest;
    for (auto& t : items) (t.group == group ? mine : rest).push_back(t);
    if (mine.empty()) { err = "nothing to restore"; return false; }
    std::error_code ec;
    for (auto& t : mine) if (fs::exists(u8file(t.original), ec)) { err = "a file named " + t.original + " exists again"; return false; }
    for (auto& t : mine) {
        fs::create_directories(u8file(t.original).parent_path(), ec);
        if (!moveFile(u8file(t.stored), u8file(t.original), err)) return false;
    }
    fs::remove(u8file(dir) / group, ec);
    writeJournal(dir, rest);
    return true;
}

uint64_t purgeTrash(const std::string& dir, int days, int64_t now) {
    std::vector<TrashItem> items = listTrash(dir), keep;
    uint64_t freed = 0;
    std::error_code ec;
    for (auto& t : items) {
        if (days > 0 && t.when > now - (int64_t)days * 86400) { keep.push_back(t); continue; }
        freed += fileSize(t.stored);
        fs::remove(u8file(t.stored), ec);
        fs::remove(u8file(t.stored).parent_path(), ec);   // only when empty
    }
    writeJournal(dir, keep);
    return freed;
}

uint64_t folderBytes(const std::string& dir) {
    uint64_t n = 0;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(u8file(dir), fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code e2;
        if (it->is_regular_file(e2)) { const auto s = it->file_size(e2); if (!e2) n += (uint64_t)s; }
    }
    return n;
}

}  // namespace dect2::recmgr
