// The voice monitor of the digital voice modes (voice_monitor.h).
#include "dect2/voice/voice_monitor.h"
#include "dect2/audio/audioout.h"
#include "dect2/common/timecompat.h"
#include "dect2/session/session_store.h"
#include "dect2/session/voice_bridge.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <sstream>

namespace dect2 {
namespace voice {

namespace fs = std::filesystem;

const char* verdictText(Verdict v) {
    switch (v) {
    case Verdict::Play: return "playing";
    case Verdict::Busy: return "another call is playing";
    case Verdict::LowerPriority: return "a priority call is playing";
    case Verdict::Muted: return "muted";
    case Verdict::Filtered: return "not on the allow list";
    case Verdict::NotHeld: return "held on another talkgroup";
    case Verdict::Squelched: return "squelched (too many errors)";
    case Verdict::Encrypted: return "encrypted, not decoded";
    case Verdict::NoCodec: return "no voice codec";
    }
    return "?";
}

namespace {

std::string upper(std::string s) { for (char& ch : s) ch = (char)std::toupper((unsigned char)ch); return s; }
std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) a++;
    while (b > a && std::isspace((unsigned char)s[b - 1])) b--;
    return s.substr(a, b - a);
}
bool allDigits(const std::string& s) { if (s.empty()) return false; for (char ch : s) if (!std::isdigit((unsigned char)ch)) return false; return true; }
std::string num(uint32_t v) { return std::to_string(v); }

// a file name part: letters, digits, '-' and '_' only
std::string safe(const std::string& s) {
    std::string o;
    for (char ch : s) o += std::isalnum((unsigned char)ch) || ch == '-' ? ch : '_';
    while (!o.empty() && o.back() == '_') o.pop_back();
    return o.empty() ? "unknown" : o.substr(0, 40);
}

std::string jsonEsc(const std::string& s) {
    std::string o;
    for (char ch : s) {
        if (ch == '"' || ch == '\\') { o += '\\'; o += ch; }
        else if ((unsigned char)ch < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", ch); o += b; }
        else o += ch;
    }
    return o;
}

std::string isoUtc(int64_t t) {
    struct tm g; gmTime((time_t)t, &g);
    char b[32]; snprintf(b, sizeof b, "%04d-%02d-%02dT%02d:%02d:%02dZ", g.tm_year + 1900, g.tm_mon + 1, g.tm_mday, g.tm_hour, g.tm_min, g.tm_sec);
    return b;
}

void put16(FILE* f, uint16_t v) { uint8_t b[2] = {(uint8_t)v, (uint8_t)(v >> 8)}; fwrite(b, 1, 2, f); }
void put32(FILE* f, uint32_t v) { uint8_t b[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)}; fwrite(b, 1, 4, f); }
void wavHeader(FILE* f, int rate, uint32_t dataBytes) {
    fwrite("RIFF", 1, 4, f); put32(f, 36 + dataBytes); fwrite("WAVEfmt ", 1, 8, f);
    put32(f, 16); put16(f, 1); put16(f, 1); put32(f, (uint32_t)rate); put32(f, (uint32_t)rate * 2); put16(f, 2); put16(f, 16);
    fwrite("data", 1, 4, f); put32(f, dataBytes);
}

std::string streamKey(const CallInfo& c, const std::string& target) {
    char b[64]; snprintf(b, sizeof b, "%lld|%d|", (long long)std::llround(c.carrierHz / 100.0), c.slot);
    return b + target;
}
std::string carrierSlot(const std::string& key) { const size_t p = key.find('|', key.find('|') + 1); return key.substr(0, p + 1); }

std::string esc(const std::string& s) {
    std::string o;
    for (char ch : s) { if (ch == '\\') o += "\\\\"; else if (ch == '\n') o += "\\n"; else o += ch; }
    return o;
}
std::string unesc(const std::string& s) {
    std::string o;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '\\' && i + 1 < s.size()) { o += s[i + 1] == 'n' ? '\n' : s[i + 1]; i++; }
        else o += s[i];
    }
    return o;
}

} // namespace

std::string targetText(const CallInfo& c) {
    if (!c.dstText.empty()) return c.dstText;
    if (c.dst == 0) return "-";
    if (c.group && c.dst == 0xFFFFFF) return "all";
    return (c.group ? "TG " : "") + num(c.dst);
}
std::string sourceText(const CallInfo& c) {
    if (!c.srcText.empty()) return c.srcText;
    return c.src ? num(c.src) : "-";
}

// ---------------------------------------------------------------- IdList

void IdList::parse(const std::string& text) {
    ranges_.clear(); texts_.clear();
    std::string tok;
    std::vector<std::string> toks;
    for (size_t i = 0; i <= text.size(); i++) {
        const char ch = i < text.size() ? text[i] : ',';
        if (ch == ',' || ch == ';' || ch == '\n' || ch == '\r' || ch == ' ' || ch == '\t') { if (!tok.empty()) toks.push_back(tok); tok.clear(); }
        else tok += ch;
    }
    for (size_t i = 0; i < toks.size(); i++) {
        std::string t = upper(toks[i]);
        if (t == "TG" && i + 1 < toks.size()) continue;          // "TG 91": the number follows
        if (t.size() > 2 && t.compare(0, 2, "TG") == 0 && allDigits(t.substr(2))) t = t.substr(2);
        const size_t dash = t.find('-');
        if (dash != std::string::npos && dash > 0 && allDigits(t.substr(0, dash)) && allDigits(t.substr(dash + 1))) {
            uint32_t a = (uint32_t)std::strtoul(t.substr(0, dash).c_str(), nullptr, 10), b = (uint32_t)std::strtoul(t.substr(dash + 1).c_str(), nullptr, 10);
            if (a > b) std::swap(a, b);
            ranges_.push_back({a, b});
        } else if (allDigits(t)) {
            const uint32_t v = (uint32_t)std::strtoul(t.c_str(), nullptr, 10);
            ranges_.push_back({v, v});
        } else {
            texts_.push_back(t);
        }
    }
}

bool IdList::matchId(uint32_t id) const {
    if (id == 0) return false;
    for (const auto& r : ranges_) if (id >= r.first && id <= r.second) return true;
    return false;
}

bool IdList::matchText(const std::string& s0) const {
    const std::string s = upper(trim(s0));
    if (s.empty()) return false;
    for (const auto& t : texts_) {
        if (t == "*") return true;
        if (!t.empty() && t.back() == '*') { if (s.compare(0, t.size() - 1, t, 0, t.size() - 1) == 0) return true; }
        else if (s == t) return true;
    }
    return false;
}

bool IdList::matchTarget(const CallInfo& c) const {
    if (matchId(c.dst)) return true;
    if (!c.dstText.empty() && matchText(c.dstText)) return true;
    for (const auto& t : texts_) if (t == "*" && (c.dst || !c.dstText.empty())) return true;
    return false;
}

bool IdList::matchSource(const CallInfo& c) const {
    if (matchId(c.src)) return true;
    return !c.srcText.empty() && matchText(c.srcText);
}

// ---------------------------------------------------------------- recordings

struct Monitor::Rec {
    FILE* f = nullptr;
    std::string path;
    int rate = 0;
    uint32_t bytes = 0;
};

// ---------------------------------------------------------------- Monitor

namespace {
std::mutex gDirMu;
std::string gDefaultDir;
}

void setDefaultRecordDir(const std::string& dir) { std::lock_guard<std::mutex> lk(gDirMu); gDefaultDir = dir; }
std::string defaultRecordDir() {
    std::lock_guard<std::mutex> lk(gDirMu);
    if (!gDefaultDir.empty()) return gDefaultDir;
    std::error_code ec;
    return (fs::temp_directory_path(ec) / "onair-voice").string();
}

Monitor::Monitor(std::string mode) : mode_(std::move(mode)) {}

Monitor::~Monitor() {
    std::lock_guard<std::mutex> lk(mu_);
    for (auto& kv : live_) finishLocked(kv.second);
}

double Monitor::now() const { return clock_ ? clock_() : steadyNowSec(); }

void Monitor::setClock(std::function<double()> monotonic, std::function<int64_t()> wall) {
    std::lock_guard<std::mutex> lk(mu_);
    clock_ = std::move(monotonic);
    wall_ = std::move(wall);
}

float Monitor::volume() const { std::lock_guard<std::mutex> lk(mu_); return set_.volume; }
Settings Monitor::settings() const { std::lock_guard<std::mutex> lk(mu_); return set_; }
uint64_t Monitor::settingsGen() const { std::lock_guard<std::mutex> lk(mu_); return gen_; }

void Monitor::setSettings(const Settings& s) {
    std::lock_guard<std::mutex> lk(mu_);
    const bool stopRec = set_.record && !s.record;
    set_ = s;
    set_.volume = std::min(1.f, std::max(0.f, set_.volume));
    set_.squelchPct = std::min(100.f, std::max(0.f, set_.squelchPct));
    set_.hangSec = std::min(30.0, std::max(0.2, set_.hangSec));
    applyLocked();
    gen_++;
    if (stopRec) for (auto& kv : live_) if (kv.second.rec) {
        Rec& r = *kv.second.rec;
        if (r.f) { fseek(r.f, 0, SEEK_SET); wavHeader(r.f, r.rate, r.bytes); fclose(r.f); r.f = nullptr; }
    }
}

void Monitor::applyLocked() {
    allow_.parse(set_.allow); mute_.parse(set_.mute); prio_.parse(set_.priority); hold_.parse(set_.hold);
    labelNum_.clear(); labelText_.clear();
    std::string line;
    for (size_t i = 0; i <= set_.labels.size(); i++) {
        const char ch = i < set_.labels.size() ? set_.labels[i] : '\n';
        if (ch != '\n' && ch != ';') { line += ch; continue; }
        const size_t eq = line.find('=');
        if (eq != std::string::npos) {
            std::string k = upper(trim(line.substr(0, eq)));
            const std::string v = trim(line.substr(eq + 1));
            if (k.size() > 2 && k.compare(0, 2, "TG") == 0) k = trim(k.substr(2));
            if (!k.empty() && !v.empty()) {
                if (allDigits(k)) labelNum_[(uint32_t)std::strtoul(k.c_str(), nullptr, 10)] = v;
                else labelText_[k] = v;
            }
        }
        line.clear();
    }
}

std::string Monitor::labelLocked(const CallInfo& c) const {
    auto n = labelNum_.find(c.dst);
    if (c.dst && n != labelNum_.end()) return n->second;
    if (!c.dstText.empty()) { auto t = labelText_.find(upper(trim(c.dstText))); if (t != labelText_.end()) return t->second; }
    n = labelNum_.find(c.src);
    if (c.src && n != labelNum_.end()) return n->second;
    if (!c.srcText.empty()) { auto t = labelText_.find(upper(trim(c.srcText))); if (t != labelText_.end()) return t->second; }
    return std::string();
}

std::string Monitor::label(const CallInfo& c) const { std::lock_guard<std::mutex> lk(mu_); return labelLocked(c); }

std::string Monitor::targetToken(const CallInfo& c) {
    if (!c.dstText.empty()) return trim(c.dstText);
    if (c.dst) return num(c.dst);
    if (!c.srcText.empty()) return trim(c.srcText);
    return c.src ? num(c.src) : std::string();
}

void Monitor::finishLocked(Live& l) {
    if (l.rec && l.rec->f) {
        Rec& r = *l.rec;
        fseek(r.f, 0, SEEK_SET);
        wavHeader(r.f, r.rate, r.bytes);
        fclose(r.f);
        r.f = nullptr;
    }
    l.e.active = false;
    l.e.wallEnd = wall_ ? wall_() : unixNow();
    if (l.rec && !l.rec->path.empty()) {
        // the details of the call next to the sound
        const CallInfo& c = l.e.info;
        std::string js = "{\n";
        auto kv = [&](const char* k, const std::string& v, bool str = true) { js += std::string("  \"") + k + "\": " + (str ? "\"" + jsonEsc(v) + "\"" : v) + ",\n"; };
        kv("mode", mode_);
        kv("target", targetText(c)); kv("source", sourceText(c));
        kv("target_id", num(c.dst), false); kv("source_id", num(c.src), false);
        kv("group", c.group ? "true" : "false", false);
        kv("slot", std::to_string(c.slot), false);
        { char b[32]; snprintf(b, sizeof b, "%.0f", c.carrierHz); kv("carrier_hz", b, false); }
        kv("access", c.access); kv("alias", c.alias); kv("label", l.e.label);
        kv("emergency", c.emergency ? "true" : "false", false);
        kv("start_utc", isoUtc(l.e.wallStart)); kv("end_utc", isoUtc(l.e.wallEnd));
        { char b[32]; snprintf(b, sizeof b, "%.2f", l.rec->rate ? l.rec->bytes / 2.0 / l.rec->rate : 0.0); kv("audio_seconds", b, false); }
        kv("frames", std::to_string(l.e.frames), false);
        { char b[32]; snprintf(b, sizeof b, "%.4f", std::max(0.f, l.e.errorRate)); kv("error_rate", b, false); }
        js += "  \"file\": \"" + jsonEsc(fs::path(l.rec->path).filename().string()) + "\"\n}\n";
        fs::path jp = fs::path(l.rec->path); jp.replace_extension(".json");
        if (FILE* f = fopen(jp.string().c_str(), "wb")) { fwrite(js.data(), 1, js.size(), f); fclose(f); }
    }
    sessionEndLocked(l);
    hist_.push_front(l.e);
    while (hist_.size() > 1000) hist_.pop_back();
}

// ---------------------------------------------------------------- the session store bridge (voice_session_bridge.cpp builds the Session)

sess::SessionStore* Monitor::sessionStore() const { return sessStore_ ? sessStore_ : &sess::global(); }
void Monitor::setSessionReceiver(uint16_t receiver) { std::lock_guard<std::mutex> lk(mu_); sessRx_ = receiver; }
uint16_t Monitor::sessionReceiver() const { std::lock_guard<std::mutex> lk(mu_); return sessRx_; }
void Monitor::setSessionStore(sess::SessionStore* store) { std::lock_guard<std::mutex> lk(mu_); sessStore_ = store; }

void Monitor::sessionSyncLocked(Live& l, double t) {
    if (sess::voicebridge::modeNumber(mode_) < 0) return;
    sess::SessionStore* st = sessionStore();
    if (!st->isOpen()) return;
    CallEntry& e = l.e;
    const bool enc = e.info.encrypted, emerg = e.info.emergency;
    if (e.sessionId == 0) {
        if (!sessNamed_) { st->setModeName((uint16_t)sess::voicebridge::modeNumber(mode_), sess::voicebridge::modeName(mode_)); sessNamed_ = true; }
        sess::Session s = sess::voicebridge::toSession(e, mode_, sessRx_, false);
        s.id = 0;
        e.sessionId = st->begin(std::move(s));
    } else if (t - l.sessSec >= 1.0 || e.verdict != l.sessVerdict || enc != l.sessEnc || emerg != l.sessEmerg) {
        st->update(sess::voicebridge::toSession(e, mode_, sessRx_, false));   // at most once a second, sooner when something that matters changed
    } else {
        return;
    }
    l.sessSec = t;
    l.sessVerdict = e.verdict;
    l.sessEnc = enc;
    l.sessEmerg = emerg;
}

void Monitor::sessionEndLocked(Live& l) {
    CallEntry& e = l.e;
    if (e.sessionId == 0) return;
    sess::SessionStore* st = sessionStore();
    // the call is over: wallEnd is set. A recording is linked (never one of an encrypted call: the monitor does not make one)
    st->end(sess::voicebridge::toSession(e, mode_, sessRx_, true));
    if (!e.file.empty() && !e.info.encrypted) st->link(e.sessionId, sess::LinkType::Audio, e.file, e.wallEnd * 1000);
}

void Monitor::housekeepLocked(double t) {
    for (auto it = live_.begin(); it != live_.end();) {
        if (t - it->second.e.lastSec > set_.hangSec) { finishLocked(it->second); it = live_.erase(it); }
        else ++it;
    }
    if (!cur_.empty() && (t - curLast_ > set_.hangSec || !live_.count(cur_))) { cur_.clear(); curPrio_ = false; }
}

void Monitor::housekeep() { std::lock_guard<std::mutex> lk(mu_); housekeepLocked(now()); }

Verdict Monitor::offer(const CallInfo& c0) {
    CallInfo c = c0;   // callsigns come padded with spaces (D-STAR, Yaesu)
    c.srcText = trim(c.srcText); c.dstText = trim(c.dstText); c.alias = trim(c.alias);
    std::lock_guard<std::mutex> lk(mu_);
    const double t = now();
    housekeepLocked(t);
    const std::string tgt = c.dst || !c.dstText.empty() ? upper(targetText(c)) : std::string("?");
    std::string key = streamKey(c, tgt);
    auto it = live_.find(key);
    if (it == live_.end()) {
        // the same stream with its target found only now, or a frame without the target: one entry
        const std::string cs = carrierSlot(key);
        for (auto j = live_.begin(); j != live_.end(); ++j) {
            if (carrierSlot(j->first) != cs) continue;
            const bool jUnknown = j->first.size() >= 1 && j->first.back() == '?' && j->first.size() == cs.size() + 1;
            if (tgt == "?" || jUnknown) {
                if (jUnknown && tgt != "?") {   // rekey to the target now known
                    Live moved = std::move(j->second);
                    if (cur_ == j->first) cur_ = key;
                    live_.erase(j);
                    moved.e.key = key;
                    it = live_.emplace(key, std::move(moved)).first;
                } else { it = j; key = j->first; }
                break;
            }
        }
    }
    if (it != live_.end()) {
        // another talker on the same talkgroup: a new call (the sound goes on)
        const CallInfo& o = it->second.e.info;
        const bool newTalker = (c.src && o.src && c.src != o.src) || (!c.srcText.empty() && !o.srcText.empty() && upper(trim(c.srcText)) != upper(trim(o.srcText)));
        if (newTalker) { finishLocked(it->second); live_.erase(it); it = live_.end(); }
    }
    if (it == live_.end()) {
        Live l;
        l.e.id = nextId_++;
        l.e.key = key;
        l.e.firstSec = t;
        l.e.wallStart = wall_ ? wall_() : unixNow();
        l.e.info = c;
        it = live_.emplace(key, std::move(l)).first;
    }
    CallEntry& e = it->second.e;
    {   // what is known stays known
        CallInfo m = c;
        if (!m.src && e.info.src) m.src = e.info.src;
        if (!m.dst && e.info.dst) { m.dst = e.info.dst; m.group = e.info.group; }
        if (m.srcText.empty()) m.srcText = e.info.srcText;
        if (m.dstText.empty()) m.dstText = e.info.dstText;
        if (m.alias.empty()) m.alias = e.info.alias;
        if (m.access.empty()) m.access = e.info.access;
        m.emergency = m.emergency || e.info.emergency;
        m.encrypted = m.encrypted || e.info.encrypted;
        e.info = m;
    }
    e.lastSec = t;
    e.frames++;
    if (c.errorRate >= 0) e.errorRate = e.errorRate < 0 ? c.errorRate : e.errorRate * 0.8f + c.errorRate * 0.2f;
    e.label = labelLocked(e.info);
    e.priority = prio_.match(e.info);
    const CallInfo& ci = e.info;
    Verdict v;
    if (ci.encrypted) v = Verdict::Encrypted;
    else if (c.codecMissing) v = Verdict::NoCodec;
    else if (mute_.match(ci)) v = Verdict::Muted;
    else if (set_.allowOnly && !allow_.match(ci)) v = Verdict::Filtered;
    else if (!hold_.empty() && !hold_.matchTarget(ci)) v = Verdict::NotHeld;
    else if (set_.squelchPct > 0 && c.errorRate >= 0 && c.errorRate * 100.f > set_.squelchPct) v = Verdict::Squelched;
    else {
        const bool curAlive = !cur_.empty() && t - curLast_ <= set_.hangSec && live_.count(cur_);
        if (!curAlive || cur_ == key) v = Verdict::Play;
        else if (e.priority && !curPrio_) v = Verdict::Play;   // a priority call takes the sound
        else v = curPrio_ ? Verdict::LowerPriority : Verdict::Busy;
    }
    if (v == Verdict::Play) { cur_ = key; curLast_ = t; curPrio_ = e.priority; e.played++; }
    e.verdict = v;
    sessionSyncLocked(it->second, t);
    return v;
}

void Monitor::openRecLocked(Live& l, int channels, int rate) {
    (void)channels;
    l.rec = std::make_unique<Rec>();
    Rec& r = *l.rec;
    r.rate = rate;
    const fs::path base = set_.recordDir.empty() ? fs::path(defaultRecordDir()) / mode_ : fs::path(set_.recordDir);
    struct tm g; gmTime((time_t)l.e.wallStart, &g);
    char day[16], hms[16];
    snprintf(day, sizeof day, "%04d-%02d-%02d", g.tm_year + 1900, g.tm_mon + 1, g.tm_mday);
    snprintf(hms, sizeof hms, "%02d%02d%02d", g.tm_hour, g.tm_min, g.tm_sec);
    const fs::path dir = base / day;
    std::error_code ec;
    fs::create_directories(dir, ec);
    std::string stem = std::string(hms) + "_" + safe(mode_) + "_" + safe(targetText(l.e.info)) + "_from_" + safe(sourceText(l.e.info));
    fs::path p = dir / (stem + ".wav");
    for (int k = 2; fs::exists(p, ec) && k < 1000; k++) p = dir / (stem + "_" + std::to_string(k) + ".wav");
    r.f = fopen(p.string().c_str(), "wb");
    if (!r.f) return;
    r.path = p.string();
    wavHeader(r.f, rate, 0);
    l.e.file = r.path;
}

void Monitor::audio(const CallInfo& c, const float* pcm, size_t frames, int channels, int rate) {
    if (!pcm || !frames || channels < 1 || rate <= 0) return;
    std::lock_guard<std::mutex> lk(mu_);
    if (!set_.record || c.encrypted) return;
    auto it = live_.find(cur_);
    if (it == live_.end() || it->second.e.verdict != Verdict::Play || it->second.e.info.encrypted) return;
    Live& l = it->second;
    if (!l.rec) openRecLocked(l, channels, rate);
    Rec& r = *l.rec;
    if (!r.f || r.rate != rate) return;
    std::vector<int16_t> s(frames);
    for (size_t i = 0; i < frames; i++) {
        const float v = std::max(-1.f, std::min(1.f, pcm[i * (size_t)channels]));
        s[i] = (int16_t)std::lrint(v * 32767.f);
    }
    fwrite(s.data(), 2, frames, r.f);
    r.bytes += (uint32_t)(frames * 2);
}

void Monitor::endCall(const CallInfo& c) {
    std::lock_guard<std::mutex> lk(mu_);
    const std::string tgt = c.dst || !c.dstText.empty() ? upper(targetText(c)) : std::string("?");
    const std::string key = streamKey(c, tgt), cs = carrierSlot(key);
    for (auto it = live_.begin(); it != live_.end();) {
        if (it->first == key || (tgt == "?" && carrierSlot(it->first) == cs)) {
            if (cur_ == it->first) { cur_.clear(); curPrio_ = false; }
            finishLocked(it->second);
            it = live_.erase(it);
        } else ++it;
    }
}

std::vector<CallEntry> Monitor::calls() const {
    std::lock_guard<std::mutex> lk(mu_);
    const double t = now();
    std::vector<CallEntry> v;
    for (const auto& kv : live_) if (t - kv.second.e.lastSec <= set_.hangSec) v.push_back(kv.second.e);
    std::stable_sort(v.begin(), v.end(), [&](const CallEntry& a, const CallEntry& b) {
        const bool pa = a.key == cur_, pb = b.key == cur_;
        if (pa != pb) return pa;
        return a.lastSec > b.lastSec;
    });
    return v;
}

bool Monitor::nowTalking(CallEntry& out) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = live_.find(cur_);
    if (cur_.empty() || it == live_.end() || now() - curLast_ > set_.hangSec) return false;
    out = it->second.e;
    return true;
}

std::vector<CallEntry> Monitor::history(const std::string& query, size_t max) const {
    std::lock_guard<std::mutex> lk(mu_);
    const std::string q = upper(trim(query));
    std::vector<CallEntry> all;
    for (const auto& kv : live_) all.push_back(kv.second.e);
    std::sort(all.begin(), all.end(), [](const CallEntry& a, const CallEntry& b) { return a.firstSec > b.firstSec; });
    for (const auto& e : hist_) all.push_back(e);
    std::vector<CallEntry> out;
    for (const auto& e : all) {
        if (out.size() >= max) break;
        if (!q.empty()) {
            const std::string hay = upper(targetText(e.info) + " " + sourceText(e.info) + " " + e.label + " " + e.info.alias + " " + e.info.access + " " +
                                          verdictText(e.verdict) + (e.info.emergency ? " EMERGENCY" : "") + (e.info.encrypted ? " ENCRYPTED" : ""));
            if (hay.find(q) == std::string::npos) continue;
        }
        out.push_back(e);
    }
    return out;
}

void Monitor::holdTarget(const std::string& token) {
    Settings s = settings();
    s.hold = token;
    setSettings(s);
}

void Monitor::holdCurrent() {
    std::string tok;
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = live_.find(cur_);
        if (it != live_.end()) tok = targetToken(it->second.e.info);
        else {
            double best = -1e18;
            for (const auto& kv : live_) if (kv.second.e.lastSec > best) { best = kv.second.e.lastSec; tok = targetToken(kv.second.e.info); }
            if (tok.empty() && !hist_.empty()) tok = targetToken(hist_.front().info);
        }
    }
    if (!tok.empty()) holdTarget(tok);
}

void Monitor::releaseHold() { holdTarget(std::string()); }

void Monitor::muteToken(const std::string& token) {
    if (trim(token).empty()) return;
    Settings s = settings();
    if (IdList(s.mute).matchText(token) || (allDigits(trim(token)) && IdList(s.mute).matchId((uint32_t)std::strtoul(trim(token).c_str(), nullptr, 10)))) return;
    s.mute += (s.mute.empty() ? "" : ", ") + trim(token);
    setSettings(s);
}

void Monitor::clearHistory() { std::lock_guard<std::mutex> lk(mu_); hist_.clear(); }

std::string Monitor::saveSettings() const {
    std::lock_guard<std::mutex> lk(mu_);
    std::ostringstream o;
    char b[64];
    snprintf(b, sizeof b, "%.3f", set_.volume); o << "volume=" << b << "\n";
    snprintf(b, sizeof b, "%.2f", set_.squelchPct); o << "squelch=" << b << "\n";
    snprintf(b, sizeof b, "%.2f", set_.hangSec); o << "hang=" << b << "\n";
    o << "device=" << esc(set_.device) << "\n";
    o << "record=" << (set_.record ? 1 : 0) << "\n";
    o << "recordDir=" << esc(set_.recordDir) << "\n";
    o << "allowOnly=" << (set_.allowOnly ? 1 : 0) << "\n";
    o << "allow=" << esc(set_.allow) << "\n";
    o << "mute=" << esc(set_.mute) << "\n";
    o << "priority=" << esc(set_.priority) << "\n";
    o << "hold=" << esc(set_.hold) << "\n";
    o << "labels=" << esc(set_.labels) << "\n";
    return o.str();
}

void Monitor::loadSettings(const std::string& text) {
    Settings s;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = line.substr(0, eq), v = unesc(line.substr(eq + 1));
        if (k == "volume") s.volume = (float)std::atof(v.c_str());
        else if (k == "squelch") s.squelchPct = (float)std::atof(v.c_str());
        else if (k == "hang") s.hangSec = std::atof(v.c_str());
        else if (k == "device") s.device = v;
        else if (k == "record") s.record = v == "1";
        else if (k == "recordDir") s.recordDir = v;
        else if (k == "allowOnly") s.allowOnly = v == "1";
        else if (k == "allow") s.allow = v;
        else if (k == "mute") s.mute = v;
        else if (k == "priority") s.priority = v;
        else if (k == "hold") s.hold = v;
        else if (k == "labels") s.labels = v;
    }
    setSettings(s);
}

// ---------------------------------------------------------------- the registry

namespace {
std::mutex gRegMu;
std::map<std::string, std::unique_ptr<Monitor>>& reg() { static auto* m = new std::map<std::string, std::unique_ptr<Monitor>>(); return *m; }   // lives to the end
}

Monitor& monitor(const std::string& mode) {
    std::lock_guard<std::mutex> lk(gRegMu);
    auto& m = reg()[mode];
    if (!m) m = std::make_unique<Monitor>(mode);
    return *m;
}

std::vector<std::string> monitorModes() {
    std::lock_guard<std::mutex> lk(gRegMu);
    std::vector<std::string> v;
    for (const auto& kv : reg()) v.push_back(kv.first);
    return v;
}

std::string saveAllSettings() {
    std::string o;
    for (const auto& m : monitorModes()) o += "[" + m + "]\n" + monitor(m).saveSettings();
    return o;
}

void loadAllSettings(const std::string& text) {
    std::istringstream in(text);
    std::string line, mode, body;
    auto flush = [&] { if (!mode.empty()) monitor(mode).loadSettings(body); body.clear(); };
    while (std::getline(in, line)) {
        if (line.size() > 2 && line.front() == '[' && line.back() == ']') { flush(); mode = line.substr(1, line.size() - 2); }
        else body += line + "\n";
    }
    flush();
}

void ensureOutput(std::unique_ptr<AudioOut>& out, const Monitor& m, int rate, int startThreshold) {
    const std::string dev = m.settings().device;
    if (out && out->device() == dev) return;
    out.reset();
    out = std::make_unique<AudioOut>();
    out->setDevice(dev);
    out->start(rate);
    out->setStartThreshold(startThreshold);
}

bool readWav(const std::string& path, int& rate, int& channels, std::vector<int16_t>& samples) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    uint8_t h[44];
    const bool ok = fread(h, 1, 44, f) == 44 && std::memcmp(h, "RIFF", 4) == 0 && std::memcmp(h + 8, "WAVEfmt ", 8) == 0;
    if (!ok) { fclose(f); return false; }
    channels = h[22] | h[23] << 8;
    rate = (int)(h[24] | h[25] << 8 | h[26] << 16 | (uint32_t)h[27] << 24);
    const uint32_t bytes = h[40] | h[41] << 8 | h[42] << 16 | (uint32_t)h[43] << 24;
    samples.resize(bytes / 2);
    const size_t got = samples.empty() ? 0 : fread(samples.data(), 2, samples.size(), f);
    fclose(f);
    samples.resize(got);
    return true;
}

} // namespace voice
} // namespace dect2
