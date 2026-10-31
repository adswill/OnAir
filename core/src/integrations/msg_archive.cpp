#include "dect2/integrations/msg_archive.h"
#include "dect2/automation/json_mini.h"
#include "dect2/integrations/integrations.h"
#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>

namespace dect2::integrations {
namespace au = dect2::automation;
namespace js = dect2::automation::json;
namespace fs = std::filesystem;

const char* archiveExt(ArchiveFormat f) { return f == ArchiveFormat::Json ? "json" : f == ArchiveFormat::Csv ? "csv" : "jsonl"; }

static std::tm utcTm(double t) {
    std::time_t tt = (std::time_t)std::floor(t);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &tt);
#else
    gmtime_r(&tt, &tm);
#endif
    return tm;
}

std::string archiveIso(double t) {
    const std::tm tm = utcTm(t);
    char b[40];
    snprintf(b, sizeof b, "%04d-%02d-%02dT%02d:%02d:%02dZ", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
    return b;
}

static std::string modeName(const au::Event& ev) {
    std::string m = ev.mode;
    if (m.empty()) { const size_t d = ev.type.find('.'); m = ev.type.substr(0, d); }
    std::string o;
    for (unsigned char c : m) {
        if (isalnum(c) || c == '-' || c == '_') o += (char)c;
        else if (c == ' ' || c == '/' || c == '.') o += '_';
    }
    return o.empty() ? "unknown" : o;
}

std::string archiveFileName(const au::Event& ev, ArchiveFormat f) {
    const std::tm tm = utcTm(ev.time > 0 ? ev.time : au::wallNow());
    char b[24];
    snprintf(b, sizeof b, "%04d-%02d-%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
    return "onair-" + modeName(ev) + "-" + b + "." + archiveExt(f);
}

namespace {
const char* const kQuality[] = {"snr_db", "snr", "rssi_dbm", "rssi", "quality", "ber", "level_db", "level"};
const char* const kText[] = {"text", "message", "msg", "body", "payload", "data", "decoded"};

const au::Value* firstOf(const au::Event& ev, const char* const* names, size_t n, std::string* which = nullptr) {
    for (size_t i = 0; i < n; i++) {
        auto it = ev.f.find(names[i]);
        if (it != ev.f.end() && !it->second.null()) { if (which) *which = names[i]; return &it->second; }
    }
    return nullptr;
}

js::Value fieldValue(const au::Value& v) {
    switch (v.t) {
    case au::Value::Num: return std::isfinite(v.n) ? js::Value::number(v.n) : js::Value();
    case au::Value::Str: return js::Value::string(v.s);
    case au::Value::Bool: return js::Value::boolean(v.b);
    default: return js::Value();
    }
}

std::string csvCell(const std::string& s) {
    bool q = false;
    for (char c : s) if (c == ',' || c == '"' || c == '\n' || c == '\r') q = true;
    if (!q) return s;
    std::string o = "\"";
    for (char c : s) { if (c == '"') o += '"'; o += c; }
    return o + "\"";
}
}  // namespace

std::string archiveRecordJson(const au::Event& ev) {
    js::Value o = js::Value::object();
    o.set("timestamp", js::Value::string(archiveIso(ev.time > 0 ? ev.time : au::wallNow())));
    o.set("mode", js::Value::string(ev.mode.empty() ? ev.type.substr(0, ev.type.find('.')) : ev.mode));
    o.set("type", js::Value::string(ev.type));
    o.set("source", js::Value::string(eventIdent(ev)));
    o.set("freq_hz", js::Value::number(ev.freqHz));
    std::string qn;
    if (const au::Value* q = firstOf(ev, kQuality, sizeof kQuality / sizeof *kQuality, &qn)) {
        js::Value v = fieldValue(*q);
        if (v.type != js::Value::Null) { o.set("quality", v); o.set("quality_field", js::Value::string(qn)); }
    }
    if (const au::Value* t = firstOf(ev, kText, sizeof kText / sizeof *kText)) o.set("text", js::Value::string(t->text()));
    js::Value f = js::Value::object();
    for (const auto& kv : ev.f) { js::Value v = fieldValue(kv.second); if (v.type != js::Value::Null) f.set(kv.first, v); }
    o.set("fields", f);
    return js::dump(o);
}

std::string archiveCsvHeader() { return "timestamp,mode,type,source,freq_hz,quality,text,fields_json"; }

std::string archiveCsvRow(const au::Event& ev) {
    std::string qv, text;
    if (const au::Value* q = firstOf(ev, kQuality, sizeof kQuality / sizeof *kQuality)) qv = q->text();
    if (const au::Value* t = firstOf(ev, kText, sizeof kText / sizeof *kText)) text = t->text();
    js::Value f = js::Value::object();
    for (const auto& kv : ev.f) { js::Value v = fieldValue(kv.second); if (v.type != js::Value::Null) f.set(kv.first, v); }
    char fq[40];
    snprintf(fq, sizeof fq, "%.0f", ev.freqHz);
    return csvCell(archiveIso(ev.time > 0 ? ev.time : au::wallNow())) + "," + csvCell(ev.mode.empty() ? ev.type.substr(0, ev.type.find('.')) : ev.mode) + "," + csvCell(ev.type) + "," +
           csvCell(eventIdent(ev)) + "," + fq + "," + csvCell(qv) + "," + csvCell(text) + "," + csvCell(js::dump(f));
}

std::string archiveWrite(const ArchiveSettings& s, const au::Event& ev) {
    if (s.folder.empty()) return "no folder chosen";
    std::error_code ec;
    fs::create_directories(fs::u8path(s.folder), ec);
    const fs::path p = fs::u8path(s.folder) / fs::u8path(archiveFileName(ev, s.format));
    if (s.format == ArchiveFormat::Json) {
        // an array kept valid after every record: "[\n rec \n]\n"; a new record replaces the closing "\n]\n"
        const std::string rec = archiveRecordJson(ev);
        const bool fresh = !fs::exists(p, ec) || fs::file_size(p, ec) < 4;
        if (fresh) {
            std::ofstream o(p, std::ios::binary | std::ios::trunc);
            if (!o) return "cannot write " + p.string();
            o << "[\n" << rec << "\n]\n";
            return o ? std::string() : "write failed";
        }
        std::fstream o(p, std::ios::binary | std::ios::in | std::ios::out);
        if (!o) return "cannot write " + p.string();
        o.seekp(-3, std::ios::end);
        o << ",\n" << rec << "\n]\n";
        return o ? std::string() : "write failed";
    }
    const bool fresh = !fs::exists(p, ec) || fs::file_size(p, ec) == 0;
    std::ofstream o(p, std::ios::binary | std::ios::app);
    if (!o) return "cannot write " + p.string();
    if (s.format == ArchiveFormat::Csv) {
        if (fresh) o << archiveCsvHeader() << "\n";
        o << archiveCsvRow(ev) << "\n";
    } else {
        o << archiveRecordJson(ev) << "\n";
    }
    return o ? std::string() : "write failed";
}

}  // namespace dect2::integrations
