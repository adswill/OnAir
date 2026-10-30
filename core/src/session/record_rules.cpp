#include "dect2/session/record_rules.h"
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <sstream>

namespace dect2::sess {

namespace {
std::string upperS(std::string s) { for (char& c : s) c = (char)std::toupper((unsigned char)c); return s; }

// "%7C" for '|', "%0A" for newline, "%25" for '%'
std::string esc(const std::string& s) {
    std::string o;
    for (char c : s) {
        if (c == '%') o += "%25"; else if (c == '|') o += "%7C"; else if (c == '\n') o += "%0A"; else if (c == '\r') o += "%0D"; else o += c;
    }
    return o;
}
std::string unesc(const std::string& s) {
    std::string o;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '%' && i + 2 < s.size()) {
            const std::string h = s.substr(i + 1, 2);
            if (h == "25") { o += '%'; i += 2; continue; }
            if (h == "7C") { o += '|'; i += 2; continue; }
            if (h == "0A") { o += '\n'; i += 2; continue; }
            if (h == "0D") { o += '\r'; i += 2; continue; }
        }
        o += s[i];
    }
    return o;
}
std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> v;
    std::string cur;
    for (char c : s) { if (c == sep) { v.push_back(cur); cur.clear(); } else cur += c; }
    v.push_back(cur);
    return v;
}
bool isNum(const std::string& s) { return !s.empty() && std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isdigit(c) != 0; }) && s.size() < 12; }

bool wordMatch(const std::string& tokenU, const std::string& wordU) {
    if (tokenU.empty() || wordU.empty()) return false;
    const size_t dash = tokenU.find('-');
    if (dash != std::string::npos && dash > 0 && isNum(tokenU.substr(0, dash)) && isNum(tokenU.substr(dash + 1)) && isNum(wordU)) {
        const unsigned long long a = std::strtoull(tokenU.c_str(), nullptr, 10), b = std::strtoull(tokenU.c_str() + dash + 1, nullptr, 10), v = std::strtoull(wordU.c_str(), nullptr, 10);
        return v >= std::min(a, b) && v <= std::max(a, b);
    }
    if (tokenU.back() == '*') return wordU.compare(0, tokenU.size() - 1, tokenU, 0, tokenU.size() - 1) == 0;
    return tokenU == wordU;
}
}  // namespace

const char* recFieldName(RecField f) {
    switch (f) {
    case RecField::Target: return "Talkgroup / channel / capcode";
    case RecField::Source: return "Source";
    case RecField::Callsign: return "Name or callsign";
    case RecField::Frequency: return "Frequency";
    default: return "Anything";
    }
}

struct RecordRules::Impl {
    mutable std::mutex mu;
    bool on = false;
    std::vector<RecordRule> rules;
    uint32_t nextId = 1;
    std::atomic<uint64_t> gen{1};
    std::atomic<uint64_t> capped{0};
    StartFn onStart;
    EndFn onEnd;
};

RecordRules::RecordRules() : p_(new Impl) {}
RecordRules::~RecordRules() { delete p_; }

bool RecordRules::enabled() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->on; }
void RecordRules::setEnabled(bool on) { std::lock_guard<std::mutex> lk(p_->mu); if (p_->on != on) { p_->on = on; p_->gen++; } }
std::vector<RecordRule> RecordRules::list() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->rules; }
uint64_t RecordRules::gen() const { return p_->gen.load(); }
uint64_t RecordRules::cappedCount() const { return p_->capped.load(); }
void RecordRules::noteCapped() { p_->capped++; }

bool RecordRules::has(uint16_t mode, RecField f, const std::string& match) const {
    std::lock_guard<std::mutex> lk(p_->mu);
    const std::string m = upperS(match);
    for (const RecordRule& r : p_->rules) if (r.mode == mode && r.field == f && upperS(r.match) == m) return true;
    return false;
}

uint32_t RecordRules::add(RecordRule r) {
    std::lock_guard<std::mutex> lk(p_->mu);
    for (const RecordRule& x : p_->rules)
        if (x.mode == r.mode && x.field == r.field && upperS(x.match) == upperS(r.match) && x.actions == r.actions && x.freqMinMHz == r.freqMinMHz && x.freqMaxMHz == r.freqMaxMHz) return x.id;
    r.id = p_->nextId++;
    if (r.name.empty()) r.name = r.match.empty() ? std::string("Everything") : r.match;
    p_->rules.push_back(std::move(r));
    p_->gen++;
    return p_->rules.back().id;
}
void RecordRules::update(const RecordRule& r) {
    std::lock_guard<std::mutex> lk(p_->mu);
    for (RecordRule& x : p_->rules) if (x.id == r.id) { x = r; p_->gen++; return; }
}
void RecordRules::remove(uint32_t id) {
    std::lock_guard<std::mutex> lk(p_->mu);
    auto it = std::remove_if(p_->rules.begin(), p_->rules.end(), [&](const RecordRule& r) { return r.id == id; });
    if (it != p_->rules.end()) { p_->rules.erase(it, p_->rules.end()); p_->gen++; }
}
void RecordRules::clear() { std::lock_guard<std::mutex> lk(p_->mu); p_->rules.clear(); p_->on = false; p_->gen++; }

bool RecordRules::matchList(const std::string& list, const std::string& field) {
    if (list.empty()) return true;
    const std::string f = upperS(field);
    std::vector<std::string> words;
    {
        std::string cur;
        for (char c : f) { if (c == ' ' || c == '/' || c == ',' || c == ':' || c == ';') { if (!cur.empty()) words.push_back(cur); cur.clear(); } else cur += c; }
        if (!cur.empty()) words.push_back(cur);
    }
    std::string tok;
    auto test = [&](const std::string& t0) {
        std::string t = t0;
        if (t.compare(0, 2, "TG") == 0 && t.size() > 2 && std::isdigit((unsigned char)t[2])) t = t.substr(2);
        if (t.empty()) return false;
        for (std::string w : words) {
            if (w.compare(0, 2, "TG") == 0 && w.size() > 2 && std::isdigit((unsigned char)w[2])) w = w.substr(2);
            if (wordMatch(t, w)) return true;
        }
        return f == t;
    };
    for (char c : list + ",") {
        if (c == ',' || c == ' ' || c == '\n' || c == ';' || c == '\t') {
            if (!tok.empty()) { if (test(upperS(tok))) return true; tok.clear(); }
        } else tok += c;
    }
    return false;
}

bool RecordRules::matches(const RecordRule& r, const Session& s) {
    if (r.mode && r.mode != s.mode) return false;
    if (r.emergencyOnly && !(s.flags & FlagEmergency)) return false;
    switch (r.field) {
    case RecField::Frequency: {
        if (!s.freqHz) return false;
        const double mhz = s.freqHz / 1e6;
        const double lo = r.freqMinMHz, hi = r.freqMaxMHz > 0 ? r.freqMaxMHz : r.freqMinMHz;
        if (hi <= 0) return true;
        const double slack = r.freqMaxMHz > 0 ? 0 : 0.0125;
        return mhz >= lo - slack && mhz <= hi + slack;
    }
    case RecField::Target: return matchList(r.match, s.who[1]) || (!s.key.empty() && !r.match.empty() && matchList(r.match, s.key));
    case RecField::Source: return matchList(r.match, s.who[0]);
    case RecField::Callsign: return r.match.empty() || matchList(r.match, s.alias[0]) || matchList(r.match, s.alias[1]) || matchList(r.match, s.who[0]) || matchList(r.match, s.who[1]) || matchList(r.match, s.key);
    default:
        return r.match.empty() || matchList(r.match, s.key) || matchList(r.match, s.who[0]) || matchList(r.match, s.who[1]) || matchList(r.match, s.alias[0]) || matchList(r.match, s.alias[1]);
    }
}

RecordDecision RecordRules::decide(const Session& s) const {
    RecordDecision d;
    if (s.verdict == (uint8_t)Verdict::Encrypted || s.verdict == (uint8_t)Verdict::Unreadable) return d;
    std::lock_guard<std::mutex> lk(p_->mu);
    if (!p_->on) return d;
    for (const RecordRule& r : p_->rules) {
        if (!r.enabled || !matches(r, s)) continue;
        if (r.capMB && r.usedBytes >= (uint64_t)r.capMB << 20) { p_->capped++; continue; }
        if ((r.actions & RecAudio) && !d.audio) { d.audio = true; if (!d.ruleId) d.ruleId = r.id; d.maxSeconds = r.maxSeconds; }
        if ((r.actions & RecIq) && !d.iq) { d.iq = true; if (!d.ruleId) d.ruleId = r.id; d.iqSeconds = r.iqSeconds ? r.iqSeconds : 10; }
        if (d.audio && d.iq) break;
    }
    return d;
}

void RecordRules::addUsage(uint32_t ruleId, uint64_t bytes) {
    std::lock_guard<std::mutex> lk(p_->mu);
    for (RecordRule& r : p_->rules) if (r.id == ruleId) { r.usedBytes += bytes; p_->gen++; return; }
}
bool RecordRules::atCap(uint32_t ruleId) const {
    std::lock_guard<std::mutex> lk(p_->mu);
    for (const RecordRule& r : p_->rules) if (r.id == ruleId) return r.capMB && r.usedBytes >= (uint64_t)r.capMB << 20;
    return false;
}

void RecordRules::setHandlers(StartFn a, EndFn b) { std::lock_guard<std::mutex> lk(p_->mu); p_->onStart = std::move(a); p_->onEnd = std::move(b); }
void RecordRules::notifyStart(const Session& s, const RecordDecision& d) const {
    StartFn f;
    { std::lock_guard<std::mutex> lk(p_->mu); f = p_->onStart; }
    if (f) f(s, d);
}
void RecordRules::notifyEnd(uint64_t id) const {
    EndFn f;
    { std::lock_guard<std::mutex> lk(p_->mu); f = p_->onEnd; }
    if (f) f(id);
}

std::string RecordRules::describe(const RecordRule& r) {
    std::string o = r.match.empty() ? std::string("Everything unencrypted") : std::string(recFieldName(r.field)) + " " + r.match;
    if (r.field == RecField::Frequency) { char b[64]; snprintf(b, sizeof b, "%.4f%s%.4f MHz", r.freqMinMHz, r.freqMaxMHz > 0 ? "-" : "", r.freqMaxMHz > 0 ? r.freqMaxMHz : 0.0); o = b; }
    if (r.emergencyOnly) o += ", emergencies only";
    o += ": ";
    o += (r.actions & RecAudio) ? "audio" : "";
    if ((r.actions & RecAudio) && (r.actions & RecIq)) o += " + ";
    o += (r.actions & RecIq) ? "IQ" : "";
    if (r.actions & RecIq) { char b[32]; snprintf(b, sizeof b, " (%u s clip)", r.iqSeconds); o += b; }
    if (r.maxSeconds) { char b[48]; if (r.maxSeconds >= 3600) snprintf(b, sizeof b, ", up to %u h", r.maxSeconds / 3600); else if (r.maxSeconds >= 60) snprintf(b, sizeof b, ", up to %u min", r.maxSeconds / 60); else snprintf(b, sizeof b, ", up to %u s", r.maxSeconds); o += b; }
    if (r.capMB) { char b[48]; snprintf(b, sizeof b, ", %u MB cap", r.capMB); o += b; }
    return o;
}

// file: "enabled=0|1", then "rule=id|enabled|mode|field|actions|emerg|maxSec|capMB|used|fmin|fmax|name|match|iqSec"
std::string RecordRules::saveText() const {
    std::lock_guard<std::mutex> lk(p_->mu);
    std::ostringstream o;
    o << "enabled=" << (p_->on ? 1 : 0) << "\n";
    for (const RecordRule& r : p_->rules)
        o << "rule=" << r.id << '|' << (r.enabled ? 1 : 0) << '|' << r.mode << '|' << (int)r.field << '|' << (int)r.actions << '|' << (r.emergencyOnly ? 1 : 0) << '|' << r.maxSeconds << '|' << r.capMB << '|'
          << r.usedBytes << '|' << r.freqMinMHz << '|' << r.freqMaxMHz << '|' << esc(r.name) << '|' << esc(r.match) << '|' << r.iqSeconds << "\n";
    return o.str();
}

void RecordRules::loadText(const std::string& text) {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->rules.clear();
    p_->on = false;
    p_->nextId = 1;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (line.compare(0, 8, "enabled=") == 0) { p_->on = line.size() > 8 && line[8] == '1'; continue; }
        if (line.compare(0, 5, "rule=") != 0) continue;
        const std::vector<std::string> f = split(line.substr(5), '|');
        if (f.size() < 13) continue;
        RecordRule r;
        r.id = (uint32_t)std::strtoul(f[0].c_str(), nullptr, 10);
        r.enabled = f[1] == "1";
        r.mode = (uint16_t)std::strtoul(f[2].c_str(), nullptr, 10);
        r.field = (RecField)std::min(4, std::max(0, atoi(f[3].c_str())));
        r.actions = (uint8_t)(atoi(f[4].c_str()) & 3);
        r.emergencyOnly = f[5] == "1";
        r.maxSeconds = (uint32_t)std::strtoul(f[6].c_str(), nullptr, 10);
        r.capMB = (uint32_t)std::strtoul(f[7].c_str(), nullptr, 10);
        r.usedBytes = std::strtoull(f[8].c_str(), nullptr, 10);
        r.freqMinMHz = atof(f[9].c_str());
        r.freqMaxMHz = atof(f[10].c_str());
        r.name = unesc(f[11]);
        r.match = unesc(f[12]);
        if (f.size() > 13) r.iqSeconds = (uint32_t)std::strtoul(f[13].c_str(), nullptr, 10);
        if (!r.id) r.id = p_->nextId;
        p_->nextId = std::max(p_->nextId, r.id + 1);
        if (!r.actions) r.actions = RecAudio;
        p_->rules.push_back(std::move(r));
    }
    p_->gen++;
}

RecordRules& rules() { static auto* r = new RecordRules; return *r; }

}  // namespace dect2::sess
