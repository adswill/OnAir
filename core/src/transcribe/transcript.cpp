#include "dect2/transcribe/transcript.h"
#include "dect2/acars/acars_log.h"
#include "dect2/common/u8file.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <sstream>

namespace dect2 {

namespace {

std::string lower(const std::string& s) {
    std::string o = s;
    for (auto& c : o) c = (char)std::tolower((unsigned char)c);
    return o;
}

std::string jesc(const std::string& s) {
    std::string o;
    o.reserve(s.size() + 2);
    for (unsigned char c : s) {
        switch (c) {
        case '"': o += "\\\""; break;
        case '\\': o += "\\\\"; break;
        case '\n': o += "\\n"; break;
        case '\r': o += "\\r"; break;
        case '\t': o += "\\t"; break;
        default:
            if (c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); o += b; }
            else o += (char)c;
        }
    }
    return o;
}

// A flat JSON object of strings and numbers: key > raw value (strings unescaped). Enough for the lines this file writes.
bool parseFlat(const std::string& s, std::vector<std::pair<std::string, std::string>>& kv) {
    size_t i = 0;
    auto ws = [&] { while (i < s.size() && std::isspace((unsigned char)s[i])) i++; };
    auto str = [&](std::string& out) {
        if (i >= s.size() || s[i] != '"') return false;
        i++;
        out.clear();
        while (i < s.size() && s[i] != '"') {
            if (s[i] == '\\' && i + 1 < s.size()) {
                const char c = s[i + 1];
                i += 2;
                switch (c) {
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    if (i + 4 > s.size()) return false;
                    const unsigned v = (unsigned)strtoul(s.substr(i, 4).c_str(), nullptr, 16);
                    i += 4;
                    if (v < 0x80) out += (char)v;
                    else if (v < 0x800) { out += (char)(0xC0 | (v >> 6)); out += (char)(0x80 | (v & 0x3F)); }
                    else { out += (char)(0xE0 | (v >> 12)); out += (char)(0x80 | ((v >> 6) & 0x3F)); out += (char)(0x80 | (v & 0x3F)); }
                    break;
                }
                default: out += c;
                }
            } else out += s[i++];
        }
        if (i >= s.size()) return false;
        i++;
        return true;
    };
    ws();
    if (i >= s.size() || s[i] != '{') return false;
    i++;
    for (;;) {
        ws();
        if (i < s.size() && s[i] == '}') return true;
        std::string k, v;
        if (!str(k)) return false;
        ws();
        if (i >= s.size() || s[i] != ':') return false;
        i++;
        ws();
        if (i < s.size() && s[i] == '"') { if (!str(v)) return false; }
        else { const size_t a = i; while (i < s.size() && s[i] != ',' && s[i] != '}') i++; v = s.substr(a, i - a); }
        kv.emplace_back(k, v);
        ws();
        if (i < s.size() && s[i] == ',') { i++; continue; }
    }
}

std::string clock(double sec, bool srt) {   // "00:01:02,500"
    if (sec < 0) sec = 0;
    const long long ms = (long long)std::llround(sec * 1000);
    char b[32];
    snprintf(b, sizeof b, "%02lld:%02lld:%02lld%c%03lld", ms / 3600000, ms / 60000 % 60, ms / 1000 % 60, srt ? ',' : '.', ms % 1000);
    return b;
}

std::string stamp(int64_t wall) {
    const std::time_t t = (std::time_t)wall;
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char b[32];
    std::strftime(b, sizeof b, "%Y-%m-%d %H:%M:%SZ", &tm);
    return b;
}

std::string label(const TranscriptEntry& e) {
    char b[32] = "";
    if (e.freqHz > 0 && e.channel.find('.') == std::string::npos) snprintf(b, sizeof b, "%.3f MHz", e.freqHz / 1e6);
    return e.channel.empty() ? std::string(b) : e.channel;
}

std::string csvField(const std::string& s) {
    bool q = false;
    for (char c : s) q |= c == ',' || c == '"' || c == '\n' || c == '\r';
    if (!q) return s;
    std::string o = "\"";
    for (char c : s) { if (c == '"') o += '"'; o += c; }
    return o + "\"";
}

void oldestFirst(std::vector<TranscriptEntry>& v) {
    std::stable_sort(v.begin(), v.end(), [](const TranscriptEntry& a, const TranscriptEntry& b) { return a.wall != b.wall ? a.wall < b.wall : a.id < b.id; });
}

}  // namespace

std::string transcriptToJson(const TranscriptEntry& e) {
    char b[160];
    std::string j = "{";
    snprintf(b, sizeof b, "\"t\":%lld,\"f\":%.0f,\"d\":%.2f,\"p\":%.3f,\"n\":%d", (long long)e.wall, e.freqHz, e.durSec, (double)e.confidence, e.turn);
    j += b;
    if (!e.confidenceAvailable) j += ",\"ca\":false";
    j += ",\"m\":\"" + jesc(e.mode) + "\",\"c\":\"" + jesc(e.channel) + "\",\"x\":\"" + jesc(e.text) + "\"";
    if (!e.lang.empty()) j += ",\"l\":\"" + jesc(e.lang) + "\"";
    if (!e.translation.empty()) j += ",\"tr\":\"" + jesc(e.translation) + "\"";
    if (!e.speaker.empty()) j += ",\"s\":\"" + jesc(e.speaker) + "\"";
    if (!e.callsign.empty()) j += ",\"cs\":\"" + jesc(e.callsign) + "\"";
    if (!e.reg.empty()) j += ",\"r\":\"" + jesc(e.reg) + "\"";
    if (!e.aircraft.empty()) j += ",\"a\":\"" + jesc(e.aircraft) + "\"";
    return j + "}";
}

bool transcriptFromJson(const std::string& line, TranscriptEntry& e) {
    std::vector<std::pair<std::string, std::string>> kv;
    if (!parseFlat(line, kv)) return false;
    e = TranscriptEntry{};
    bool haveT = false, haveX = false;
    for (const auto& p : kv) {
        const std::string& k = p.first;
        const std::string& v = p.second;
        if (k == "t") { e.wall = atoll(v.c_str()); haveT = true; }
        else if (k == "f") e.freqHz = atof(v.c_str());
        else if (k == "d") e.durSec = atof(v.c_str());
        else if (k == "p") e.confidence = (float)atof(v.c_str());
        else if (k == "ca") e.confidenceAvailable = v != "false" && v != "0";
        else if (k == "n") e.turn = atoi(v.c_str());
        else if (k == "m") e.mode = v;
        else if (k == "c") e.channel = v;
        else if (k == "x") { e.text = v; haveX = true; }
        else if (k == "l") e.lang = v;
        else if (k == "tr") e.translation = v;
        else if (k == "s") e.speaker = v;
        else if (k == "cs") e.callsign = v;
        else if (k == "r") e.reg = v;
        else if (k == "a") e.aircraft = v;
    }
    return haveT && haveX;
}

bool TranscriptStore::matches(const TranscriptEntry& e, const TranscriptFilter& f) {
    if (f.from && e.wall < f.from) return false;
    if (f.to && e.wall > f.to) return false;
    if (!f.mode.empty() && e.mode != f.mode) return false;
    if (f.freqHz > 0 && std::fabs(e.freqHz - f.freqHz) > 5000) return false;
    if (!f.aircraft.empty()) {
        const std::string a = acarsNormFlight(f.aircraft), r = acarsNormReg(f.aircraft);
        const bool hit = (!e.callsign.empty() && e.callsign == a) || (!e.reg.empty() && e.reg == r) ||
                         (!e.aircraft.empty() && (acarsNormFlight(e.aircraft) == a || acarsNormReg(e.aircraft) == r));
        if (!hit) return false;
    }
    if (!f.airport.empty()) {
        const std::string ap = lower(f.airport);
        if (lower(e.channel).find(ap) == std::string::npos && lower(e.text).find(ap) == std::string::npos) return false;
    }
    if (!f.query.empty()) {
        const std::string hay = lower(e.text + " " + e.translation + " " + e.channel + " " + e.callsign + " " + e.reg + " " + e.aircraft);
        std::istringstream in(lower(f.query));
        std::string w;
        while (in >> w) if (hay.find(w) == std::string::npos) return false;
    }
    return true;
}

void TranscriptStore::setLimits(size_t maxEntries, int retentionDays) {
    std::lock_guard<std::mutex> lk(mu_);
    max_ = std::max<size_t>(10, maxEntries);
    days_ = retentionDays;
}

uint64_t TranscriptStore::add(TranscriptEntry e) {
    std::lock_guard<std::mutex> lk(mu_);
    e.id = nextId_++;
    ver_++;
    if (!path_.empty()) {
        if (FILE* f = u8fopen(path_, "ab")) {
            const std::string l = transcriptToJson(e) + "\n";
            fwrite(l.data(), 1, l.size(), f);
            fclose(f);
        }
    }
    v_.push_back(std::move(e));
    const uint64_t id = v_.back().id;
    if (v_.size() > max_ + max_ / 10) {   // trimmed in steps, so the file is not rewritten for every entry
        while (v_.size() > max_) v_.pop_front();
        if (!path_.empty()) rewriteLocked();
    }
    return id;
}

std::vector<TranscriptEntry> TranscriptStore::find(const TranscriptFilter& f, size_t limit) const {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<TranscriptEntry> out;
    for (auto it = v_.rbegin(); it != v_.rend(); ++it) {
        if (!matches(*it, f)) continue;
        out.push_back(*it);
        if (limit && out.size() >= limit) break;
    }
    return out;
}

std::vector<TranscriptEntry> TranscriptStore::forAircraft(const std::string& callsign, const std::string& reg, size_t limit) const {
    const std::string cs = acarsNormFlight(callsign), rg = acarsNormReg(reg);
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<TranscriptEntry> out;
    if (cs.empty() && rg.empty()) return out;
    for (auto it = v_.rbegin(); it != v_.rend(); ++it) {
        const bool hit = (!cs.empty() && !it->callsign.empty() && it->callsign == cs) || (!rg.empty() && !it->reg.empty() && it->reg == rg);
        if (!hit) continue;
        out.push_back(*it);
        if (limit && out.size() >= limit) break;
    }
    return out;
}

size_t TranscriptStore::size() const { std::lock_guard<std::mutex> lk(mu_); return v_.size(); }
uint64_t TranscriptStore::version() const { std::lock_guard<std::mutex> lk(mu_); return ver_; }

void TranscriptStore::clear() {
    std::lock_guard<std::mutex> lk(mu_);
    v_.clear();
    ver_++;
    if (!path_.empty()) rewriteLocked();
}

void TranscriptStore::prune(int64_t nowWall) {
    std::lock_guard<std::mutex> lk(mu_);
    const size_t before = v_.size();
    if (days_ > 0) while (!v_.empty() && v_.front().wall < nowWall - (int64_t)days_ * 86400) v_.pop_front();
    while (v_.size() > max_) v_.pop_front();
    if (v_.size() != before) { ver_++; if (!path_.empty()) rewriteLocked(); }
}

void TranscriptStore::rewriteLocked() {
    const std::string tmp = path_ + ".tmp";
    FILE* f = u8fopen(tmp, "wb");
    if (!f) return;
    for (const auto& e : v_) { const std::string l = transcriptToJson(e) + "\n"; fwrite(l.data(), 1, l.size(), f); }
    fclose(f);
    std::error_code ec;
    std::filesystem::rename(u8file(tmp), u8file(path_), ec);
    if (ec) std::filesystem::remove(u8file(tmp), ec);
}

bool TranscriptStore::attachFile(const std::string& path, std::string* err) {
    std::lock_guard<std::mutex> lk(mu_);
    path_.clear();
    std::error_code ec;
    const auto p = u8file(path);
    if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path(), ec);
    bool dropped = false;
    if (FILE* f = u8fopen(path, "rb")) {
        std::string line;
        int c;
        const int64_t now = (int64_t)time(nullptr);
        auto take = [&] {
            TranscriptEntry e;
            if (!line.empty() && transcriptFromJson(line, e)) {
                if (days_ > 0 && e.wall < now - (int64_t)days_ * 86400) dropped = true;
                else { e.id = nextId_++; v_.push_back(std::move(e)); }
            } else if (!line.empty()) dropped = true;
            line.clear();
        };
        while ((c = fgetc(f)) != EOF) { if (c == '\n') take(); else line += (char)c; }
        take();
        fclose(f);
        while (v_.size() > max_) { v_.pop_front(); dropped = true; }
    }
    FILE* t = u8fopen(path, "ab");
    if (!t) { if (err) *err = "cannot write " + path; return false; }
    fclose(t);
    path_ = path;
    ver_++;
    if (dropped) rewriteLocked();
    return true;
}

void TranscriptStore::detachFile() { std::lock_guard<std::mutex> lk(mu_); path_.clear(); }

std::string transcriptsTxt(std::vector<TranscriptEntry> v) {
    oldestFirst(v);
    std::string o;
    for (const auto& e : v) {
        o += stamp(e.wall) + "  " + label(e);
        if (!e.speaker.empty()) o += "  " + e.speaker;
        if (!e.callsign.empty()) o += "  " + e.callsign;
        else if (!e.reg.empty()) o += "  " + e.reg;
        o += "\n" + e.text + "\n";
        if (!e.translation.empty()) o += "(" + (e.lang.empty() ? std::string("EN") : e.lang + " > EN") + ") " + e.translation + "\n";
        o += "\n";
    }
    return o;
}

std::string transcriptsSrt(std::vector<TranscriptEntry> v) {
    oldestFirst(v);
    std::string o;
    if (v.empty()) return o;
    const int64_t t0 = v.front().wall;
    int n = 0;
    for (const auto& e : v) {
        const double a = (double)(e.wall - t0), b = a + std::max(1.0, e.durSec);
        o += std::to_string(++n) + "\n" + clock(a, true) + " --> " + clock(b, true) + "\n[" + label(e) + "] " + e.text + "\n" + (e.translation.empty() ? std::string() : "[EN] " + e.translation + "\n") + "\n";
    }
    return o;
}

std::string transcriptsJson(std::vector<TranscriptEntry> v) {
    oldestFirst(v);
    std::string o = "[";
    bool first = true;
    for (const auto& e : v) { if (!first) o += ",\n"; first = false; o += transcriptToJson(e); }
    return o + "]\n";
}

std::string transcriptsCsv(std::vector<TranscriptEntry> v) {
    oldestFirst(v);
    std::string o = "time_utc,frequency_mhz,mode,channel,duration_s,confidence,speaker,callsign,registration,aircraft,language,text,translation\r\n";
    char b[96];
    for (const auto& e : v) {
        snprintf(b, sizeof b, ",%.4f,", e.freqHz / 1e6);
        o += stamp(e.wall) + b + csvField(e.mode) + "," + csvField(e.channel);
        if (e.confidenceAvailable) snprintf(b, sizeof b, ",%.2f,%.2f,", e.durSec, (double)e.confidence);
        else snprintf(b, sizeof b, ",%.2f,,", e.durSec);
        o += b + csvField(e.speaker) + "," + csvField(e.callsign) + "," + csvField(e.reg) + "," + csvField(e.aircraft) + "," + csvField(e.lang) + "," + csvField(e.text) + "," + csvField(e.translation) + "\r\n";
    }
    return o;
}

}  // namespace dect2
