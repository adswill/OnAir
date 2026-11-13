// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/common/thread_name.h"
#include "dect2/watchlist/watchlist.h"
#include "dect2/automation/json_mini.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <map>
#include <set>

namespace dect2::watchlist {
namespace au = dect2::automation;
namespace js = dect2::automation::json;

namespace {
constexpr size_t kQueueMax = 2000;
constexpr size_t kHistoryMax = 300;
constexpr size_t kMaxEntries = 500;

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && isspace((unsigned char)s[a])) a++;
    while (b > a && isspace((unsigned char)s[b - 1])) b--;
    return s.substr(a, b - a);
}
std::string upper(std::string s) { for (char& c : s) c = (char)toupper((unsigned char)c); return s; }
std::string lower(std::string s) { for (char& c : s) c = (char)tolower((unsigned char)c); return s; }

std::string field(const au::Event& ev, const char* k) {
    auto it = ev.f.find(k);
    return it == ev.f.end() ? std::string() : it->second.text();
}
bool startsWith(const std::string& s, const char* p) { return s.compare(0, strlen(p), p) == 0; }

bool isPrefix(const std::string& e) { return !e.empty() && e.back() == '*'; }
std::string stripStar(std::string e) { if (isPrefix(e)) e.pop_back(); return e; }

// entry (already normalised, star removed) against a value
bool idEq(const std::string& entry, bool prefix, const std::string& value) {
    if (entry.empty() || value.empty()) return false;
    return prefix ? value.compare(0, entry.size(), entry) == 0 : value == entry;
}

bool freqRange(const std::string& e, double tol, double& lo, double& hi) {
    std::string s = lower(trim(e));
    if (s.empty()) return false;
    double mult = 1e6;
    auto unit = [&](const char* u, double m) {
        const size_t n = strlen(u);
        if (s.size() > n && s.compare(s.size() - n, n, u) == 0) { mult = m; s = trim(s.substr(0, s.size() - n)); return true; }
        return false;
    };
    if (!unit("mhz", 1e6) && !unit("khz", 1e3) && !unit("ghz", 1e9)) unit("hz", 1);
    const size_t dash = s.find('-', 1);
    auto num = [](const std::string& t, double& v) {
        char* end = nullptr;
        v = strtod(t.c_str(), &end);
        return end != t.c_str() && *end == 0 && std::isfinite(v) && v > 0;
    };
    double a = 0, b = 0;
    if (dash != std::string::npos) {
        if (!num(trim(s.substr(0, dash)), a) || !num(trim(s.substr(dash + 1)), b)) return false;
        lo = std::min(a, b) * mult; hi = std::max(a, b) * mult;
    } else {
        if (!num(s, a)) return false;
        lo = a * mult - tol; hi = a * mult + tol;
    }
    return true;
}

bool wordIn(const std::string& hayLower, const std::string& needleLower, bool prefix) {
    if (needleLower.empty()) return false;
    auto alnum = [](char c) { return isalnum((unsigned char)c) != 0; };
    for (size_t p = hayLower.find(needleLower); p != std::string::npos; p = hayLower.find(needleLower, p + 1)) {
        const bool left = p == 0 || !alnum(hayLower[p - 1]) || !alnum(needleLower[0]);
        const size_t e = p + needleLower.size();
        const bool right = prefix || e >= hayLower.size() || !alnum(hayLower[e]) || !alnum(needleLower.back());
        if (left && right) return true;
    }
    return false;
}

bool encryptedOrVoice(const au::Event& ev) {
    auto it = ev.f.find("encrypted");
    if (it != ev.f.end() && it->second.truthy()) return true;
    const size_t d = ev.type.find('.');
    const std::string fam = ev.type.substr(0, d);
    return fam == "voice" || fam == "audio";
}

std::string firstOf(const au::Event& ev, std::initializer_list<const char*> keys) {
    for (const char* k : keys) { std::string v = field(ev, k); if (!v.empty()) return v; }
    return {};
}

std::string asciiLine(const std::string& s, size_t max) {
    std::string o;
    for (unsigned char c : s) {
        if (c == '\n' || c == '\r' || c == '\t') o += ' ';
        else if (c >= 32 && c < 127) o += (char)c;
        else if ((c & 0xC0) != 0x80) o += '?';   // one ? for a UTF-8 character
        if (o.size() >= max) break;
    }
    return o;
}
std::string clip(const std::string& s, size_t max) {
    if (s.size() <= max) return s;
    size_t n = max;
    while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) n--;   // do not cut a UTF-8 character
    return s.substr(0, n) + "...";
}

bool httpUrlOk(const std::string& u) {
    if (!(startsWith(u, "https://") || startsWith(u, "http://")) || u.size() > 500 || u.size() < 12) return false;
    for (unsigned char c : u) if (c <= 32 || c == '"' || c == '\'' || c == '\\' || c == '`' || c == '<' || c == '>' || c >= 127) return false;
    return true;
}
bool charsOk(const std::string& s, const char* extra, size_t max) {
    if (s.empty() || s.size() > max) return false;
    for (unsigned char c : s) if (!isalnum(c) && !strchr(extra, c)) return false;
    return true;
}
bool tokenOk(const std::string& t) {   // header value: printable, no spaces
    if (t.size() > 300) return false;
    for (unsigned char c : t) if (c <= 32 || c >= 127) return false;
    return true;
}
}  // namespace

const char* kindName(Kind k) {
    switch (k) {
    case Kind::Aircraft: return "aircraft";
    case Kind::Ship: return "ship";
    case Kind::Aprs: return "aprs";
    case Kind::Frequency: return "frequency";
    case Kind::Words: return "words";
    }
    return "aircraft";
}
bool kindFromName(const std::string& s, Kind& k) {
    for (Kind c : {Kind::Aircraft, Kind::Ship, Kind::Aprs, Kind::Frequency, Kind::Words}) if (s == kindName(c)) { k = c; return true; }
    return false;
}

std::string normId(const std::string& s) {
    std::string o;
    for (unsigned char c : s) if (isalnum(c)) o += (char)toupper(c);
    return o;
}

bool matchList(const List& l, const au::Event& ev, std::string& matched, std::string& ident) {
    if (l.values.empty() || encryptedOrVoice(ev)) return false;
    const std::string& t = ev.type;
    std::vector<std::pair<const char*, std::string>> cand;   // (field, value) to compare for the id-like kinds
    auto add = [&](std::initializer_list<const char*> keys) { for (const char* k : keys) { std::string v = field(ev, k); if (!v.empty()) cand.push_back({k, v}); } };
    switch (l.kind) {
    case Kind::Aircraft:
        if (startsWith(t, "adsb.")) add({"hex", "callsign"});
        else if (startsWith(t, "acars.")) add({"tail", "flight"});
        else if (startsWith(t, "vdl2.")) add({"src", "dst"});
        else if (startsWith(t, "hfdl.")) add({"icao"});
        break;
    case Kind::Ship:
        if (startsWith(t, "ais.")) add({"mmsi", "name", "callsign"});
        else if (startsWith(t, "dsc.")) add({"from_mmsi", "to_mmsi", "distress_mmsi"});
        break;
    case Kind::Aprs:
        if (startsWith(t, "aprs.")) add({"from", "to"});
        break;
    case Kind::Frequency: {
        if (!(ev.freqHz > 0)) return false;
        for (const std::string& e : l.values) {
            double lo, hi;
            if (freqRange(e, l.tolHz, lo, hi) && ev.freqHz >= lo && ev.freqHz <= hi) { matched = trim(e); ident = ""; return true; }
        }
        return false;
    }
    case Kind::Words: {
        static const char* const types[] = {"adsb.", "acars.", "vdl2.", "hfdl.", "pager.", "ais.", "aprs.", "lora.", "ble.", "rid.", "dsc.", "sonde.", "plugin."};
        bool ok = false;
        for (const char* p : types) if (startsWith(t, p)) { ok = true; break; }
        if (!ok) return false;
        std::string hay;
        for (const char* k : {"text", "info", "summary", "destination", "name", "callsign"}) { const std::string v = field(ev, k); if (!v.empty()) hay += lower(v) + "\n"; }
        if (hay.empty()) return false;
        for (const std::string& e : l.values) {
            const std::string w = trim(e);
            if (wordIn(hay, lower(stripStar(w)), isPrefix(w))) {
                matched = w;
                ident = firstOf(ev, {"tail", "hex", "icao", "mmsi", "from_mmsi", "from", "src", "serial", "id", "mac", "address"});
                return true;
            }
        }
        return false;
    }
    }
    for (const std::string& raw : l.values) {
        const std::string e = trim(raw);
        if (e.empty()) continue;
        // "DSC distress" in a ship list: every DSC distress alert, relay or acknowledgement, whoever sends it
        if (l.kind == Kind::Ship && startsWith(t, "dsc.") && normId(e) == "DSCDISTRESS") {
            if (field(ev, "category") == "distress") { matched = e; ident = field(ev, "from_mmsi"); return true; }
            continue;
        }
        const bool pre = isPrefix(e);
        const std::string body = stripStar(e);
        for (const auto& c : cand) {
            bool hit;
            if (l.kind == Kind::Aprs) {
                const std::string want = upper(body), have = upper(trim(c.second));
                const bool hasSsid = want.find('-') != std::string::npos;
                const std::string haveBase = have.substr(0, have.find('-'));
                hit = idEq(want, pre, hasSsid ? have : haveBase) || (!hasSsid && idEq(want, pre, have));
            } else {
                hit = idEq(normId(body), pre, normId(c.second));
            }
            if (hit) { matched = e; ident = c.second; return true; }
        }
    }
    return false;
}

bool inQuietHours(const Settings& s, double t) {
    if (!s.quietOn || s.quietStartMin == s.quietEndMin) return false;
    long long sec = (long long)std::floor(t) + s.tzOffsetSec;
    sec = ((sec % 86400) + 86400) % 86400;
    const int m = (int)(sec / 60);
    if (s.quietStartMin < s.quietEndMin) return m >= s.quietStartMin && m < s.quietEndMin;
    return m >= s.quietStartMin || m < s.quietEndMin;
}

bool buildNtfy(const Channels& c, const std::string& title, const std::string& body, Request& out, std::string& err) {
    std::string server = trim(c.ntfyServer);
    while (!server.empty() && server.back() == '/') server.pop_back();
    if (!httpUrlOk(server)) { err = "the ntfy server address is not a web address"; return false; }
    if (!charsOk(c.ntfyTopic, "_-", 64)) { err = "the ntfy topic may hold letters, digits, - and _ only"; return false; }
    if (!c.ntfyToken.empty() && !tokenOk(c.ntfyToken)) { err = "the ntfy token has odd characters"; return false; }
    out = Request();
    out.label = "ntfy";
    out.url = server + "/" + c.ntfyTopic;
    out.contentType = "text/plain; charset=utf-8";
    out.body = body;
    out.headers.push_back("Title: " + asciiLine(title, 100));
    if (!c.ntfyToken.empty()) out.headers.push_back("Authorization: Bearer " + c.ntfyToken);
    return true;
}

bool buildTelegram(const Channels& c, const std::string& title, const std::string& body, Request& out, std::string& err) {
    if (!charsOk(c.telegramToken, ":_-", 100) || c.telegramToken.find(':') == std::string::npos) { err = "the Telegram bot token is not valid"; return false; }
    if (!charsOk(c.telegramChat, "-@_", 64)) { err = "the Telegram chat id is not valid"; return false; }
    out = Request();
    out.label = "telegram";
    out.url = "https://api.telegram.org/bot" + c.telegramToken + "/sendMessage";
    out.contentType = "application/json";
    out.body = "{\"chat_id\":\"" + js::escape(c.telegramChat) + "\",\"text\":\"" + js::escape(title + "\n" + body) + "\"}";
    return true;
}

bool buildWebhook(const Channels& c, const Hit& h, const au::Event& ev, Request& out, std::string& err) {
    const std::string url = trim(c.webhookUrl);
    if (!httpUrlOk(url)) { err = "the webhook address is not a web address"; return false; }
    if (!c.webhookToken.empty() && !tokenOk(c.webhookToken)) { err = "the webhook token has odd characters"; return false; }
    js::Value o = js::Value::object();
    o.set("source", js::Value::string("OnAir"));
    o.set("list", js::Value::string(h.list));
    o.set("kind", js::Value::string(h.kind));
    o.set("matched", js::Value::string(h.matched));
    o.set("type", js::Value::string(ev.type));
    o.set("mode", js::Value::string(ev.mode));
    o.set("ident", js::Value::string(h.ident));
    o.set("freq_hz", js::Value::number(ev.freqHz));
    o.set("time", js::Value::number(ev.time));
    o.set("summary", js::Value::string(h.summary));
    out = Request();
    out.label = "webhook";
    out.url = url;
    out.contentType = "application/json";
    out.body = js::dump(o);
    if (!c.webhookToken.empty()) out.headers.push_back("Authorization: Bearer " + c.webhookToken);
    return true;
}

std::vector<std::string> splitValues(const std::string& text) {
    std::vector<std::string> v;
    std::set<std::string> seen;
    std::string cur;
    auto flush = [&] {
        std::string t = trim(cur);
        cur.clear();
        if (t.empty() || t.size() > 120 || v.size() >= kMaxEntries) return;
        if (seen.insert(lower(t)).second) v.push_back(t);
    };
    for (char c : text) { if (c == '\n' || c == '\r' || c == ',' || c == ';') flush(); else cur += c; }
    flush();
    return v;
}
std::string joinValues(const std::vector<std::string>& v) {
    std::string o;
    for (const std::string& s : v) { if (!o.empty()) o += "\n"; o += s; }
    return o;
}

std::string toJson(const std::vector<List>& lists, const Settings& s) {
    js::Value root = js::Value::object();
    root.set("version", js::Value::number(1));
    js::Value st = js::Value::object();
    st.set("enabled", js::Value::boolean(s.enabled));
    st.set("max_per_hour", js::Value::number(s.maxPerHour));
    st.set("cooldown_sec", js::Value::number(s.cooldownSec));
    st.set("quiet_on", js::Value::boolean(s.quietOn));
    st.set("quiet_start_min", js::Value::number(s.quietStartMin));
    st.set("quiet_end_min", js::Value::number(s.quietEndMin));
    const Channels& c = s.ch;
    st.set("notify", js::Value::boolean(c.notify));
    st.set("sound", js::Value::boolean(c.sound));
    st.set("sound_file", js::Value::string(c.soundFile));
    st.set("ntfy", js::Value::boolean(c.ntfy));
    st.set("ntfy_server", js::Value::string(c.ntfyServer));
    st.set("ntfy_topic", js::Value::string(c.ntfyTopic));
    st.set("ntfy_token", js::Value::string(c.ntfyToken));
    st.set("telegram", js::Value::boolean(c.telegram));
    st.set("telegram_token", js::Value::string(c.telegramToken));
    st.set("telegram_chat", js::Value::string(c.telegramChat));
    st.set("webhook", js::Value::boolean(c.webhook));
    st.set("webhook_url", js::Value::string(c.webhookUrl));
    st.set("webhook_token", js::Value::string(c.webhookToken));
    root.set("settings", st);
    js::Value arr = js::Value::array();
    for (const List& l : lists) {
        js::Value o = js::Value::object();
        o.set("id", js::Value::string(l.id));
        o.set("name", js::Value::string(l.name));
        o.set("kind", js::Value::string(kindName(l.kind)));
        o.set("enabled", js::Value::boolean(l.enabled));
        o.set("tol_hz", js::Value::number(l.tolHz));
        js::Value vals = js::Value::array();
        for (const std::string& v : l.values) vals.push(js::Value::string(v));
        o.set("values", vals);
        arr.push(o);
    }
    root.set("lists", arr);
    return js::dump(root, true);
}

bool fromJson(const std::string& text, std::vector<List>& lists, Settings& s, std::string& err) {
    js::Value root;
    if (!js::parse(text, root, err)) return false;
    if (root.type != js::Value::Obj) { err = "not a watchlist file"; return false; }
    Settings n;
    if (const js::Value* st = root.get("settings")) {
        n.enabled = st->flag("enabled", true);
        n.maxPerHour = (int)std::max(0.0, std::min(10000.0, st->num("max_per_hour", 30)));
        n.cooldownSec = (int)std::max(0.0, std::min(86400.0, st->num("cooldown_sec", 300)));
        n.quietOn = st->flag("quiet_on", false);
        n.quietStartMin = (int)std::max(0.0, std::min(1439.0, st->num("quiet_start_min", 22 * 60)));
        n.quietEndMin = (int)std::max(0.0, std::min(1439.0, st->num("quiet_end_min", 7 * 60)));
        Channels& c = n.ch;
        c.notify = st->flag("notify", true);
        c.sound = st->flag("sound", false);
        c.soundFile = st->str("sound_file");
        c.ntfy = st->flag("ntfy", false);
        c.ntfyServer = st->str("ntfy_server", "https://ntfy.sh");
        c.ntfyTopic = st->str("ntfy_topic");
        c.ntfyToken = st->str("ntfy_token");
        c.telegram = st->flag("telegram", false);
        c.telegramToken = st->str("telegram_token");
        c.telegramChat = st->str("telegram_chat");
        c.webhook = st->flag("webhook", false);
        c.webhookUrl = st->str("webhook_url");
        c.webhookToken = st->str("webhook_token");
    }
    std::vector<List> out;
    if (const js::Value* arr = root.get("lists")) {
        if (arr->type == js::Value::Arr) {
            for (const js::Value& o : arr->a) {
                List l;
                l.id = o.str("id");
                l.name = o.str("name");
                if (!kindFromName(o.str("kind"), l.kind)) continue;
                l.enabled = o.flag("enabled", true);
                l.tolHz = std::max(0.0, std::min(10e6, o.num("tol_hz", 5000)));
                if (const js::Value* vals = o.get("values")) if (vals->type == js::Value::Arr) for (const js::Value& v : vals->a) if (v.type == js::Value::Str && !trim(v.s).empty() && l.values.size() < kMaxEntries) l.values.push_back(trim(v.s));
                if (l.id.empty()) l.id = "l" + std::to_string(out.size() + 1);
                if (out.size() < 100) out.push_back(l);
            }
        }
    }
    lists = out;
    s = n;
    return true;
}

// ---------------------------------------------------------------------------------------------------------------------------------

struct Watcher::Impl {
    mutable std::mutex mu;
    std::shared_ptr<const std::vector<List>> lists = std::make_shared<std::vector<List>>();
    Settings set;
    Sink* sink = nullptr;
    std::function<double()> clock = [] { return au::wallNow(); };
    std::deque<Hit> history;
    std::map<std::string, double> lastAlert;
    std::deque<double> sendTimes;
    uint64_t sent = 0;
    std::atomic<bool> active{false};
    std::mutex qmu;
    std::condition_variable qcv;
    std::deque<au::Event> q;
    std::thread th;
    bool stopReq = false;

    void refreshActive() {   // mu held
        bool a = set.enabled;
        bool any = false;
        for (const List& l : *lists) if (l.enabled && !l.values.empty()) any = true;
        active.store(a && any);
    }

    // delivers one alert on the enabled channels; returns what happened
    std::string deliver(Sink* sk, const Settings& s, const Hit& h, const au::Event& ev, const std::string& title, const std::string& body, bool webhookToo) {
        std::string res;
        auto note = [&](const std::string& t) { if (!res.empty()) res += ", "; res += t; };
        if (!sk) return "no sink";
        const Channels& c = s.ch;
        if (c.notify) { sk->notify(title, body); note("notify"); }
        if (c.sound) { sk->sound(c.soundFile); note("sound"); }
        Request r;
        std::string err;
        if (c.ntfy) { if (buildNtfy(c, title, body, r, err)) { sk->post(r); note("ntfy"); } else note("ntfy: " + err); }
        if (c.telegram) { if (buildTelegram(c, title, body, r, err)) { sk->post(r); note("telegram"); } else note("telegram: " + err); }
        if (c.webhook && webhookToo) { if (buildWebhook(c, h, ev, r, err)) { sk->post(r); note("webhook"); } else note("webhook: " + err); }
        return res.empty() ? "no channel is switched on" : res;
    }
};

Watcher::Watcher() : p_(new Impl) {}
Watcher::~Watcher() { stop(); }

void Watcher::setLists(const std::vector<List>& l) {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->lists = std::make_shared<std::vector<List>>(l);
    p_->refreshActive();
}
std::vector<List> Watcher::lists() const { std::lock_guard<std::mutex> lk(p_->mu); return *p_->lists; }
void Watcher::setSettings(const Settings& s) { std::lock_guard<std::mutex> lk(p_->mu); p_->set = s; p_->refreshActive(); }
Settings Watcher::settings() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->set; }
void Watcher::setSink(Sink* s) { std::lock_guard<std::mutex> lk(p_->mu); p_->sink = s; }
void Watcher::setClock(std::function<double()> now) { std::lock_guard<std::mutex> lk(p_->mu); p_->clock = now ? now : [] { return au::wallNow(); }; }

int Watcher::process(const au::Event& ev) {
    std::shared_ptr<const std::vector<List>> lists;
    Settings s;
    Sink* sk;
    double now;
    {
        std::lock_guard<std::mutex> lk(p_->mu);
        if (!p_->set.enabled) return 0;
        lists = p_->lists;
        s = p_->set;
        sk = p_->sink;
        now = p_->clock();
    }
    int delivered = 0;
    for (const List& l : *lists) {
        if (!l.enabled) continue;
        Hit h;
        if (!matchList(l, ev, h.matched, h.ident)) continue;
        h.time = now;
        h.list = l.name.empty() ? kindName(l.kind) : l.name;
        h.kind = kindName(l.kind);
        h.type = ev.type;
        h.mode = ev.mode;
        std::string txt = firstOf(ev, {"text", "info", "summary", "detail", "name", "callsign", "destination"});
        for (char& c : txt) if (c == '\n' || c == '\r') c = ' ';
        std::string who = h.ident.empty() ? std::string() : h.ident;
        if (ev.freqHz > 0 && l.kind == Kind::Frequency) { char b[40]; snprintf(b, sizeof b, "%.4f MHz", ev.freqHz / 1e6); who = b; }
        h.summary = clip((ev.mode.empty() ? ev.type : ev.mode) + (who.empty() ? "" : " " + who) + (txt.empty() ? "" : ": " + txt), 220);
        const std::string key = l.id + "|" + h.matched + "|" + h.ident;
        bool deliverIt = false;
        {
            std::lock_guard<std::mutex> lk(p_->mu);
            auto it = p_->lastAlert.find(key);
            if (it != p_->lastAlert.end() && now >= it->second && now - it->second < s.cooldownSec) continue;   // the same station again: silent
            if (p_->lastAlert.size() > 20000) p_->lastAlert.clear();
            p_->lastAlert[key] = now;
            while (!p_->sendTimes.empty() && now - p_->sendTimes.front() > 3600) p_->sendTimes.pop_front();
            if (inQuietHours(s, now)) h.why = "quiet hours";
            else if (s.maxPerHour > 0 && (int)p_->sendTimes.size() >= s.maxPerHour) h.why = "rate limit";
            else { deliverIt = true; p_->sendTimes.push_back(now); p_->sent++; }
        }
        if (deliverIt) {
            h.delivered = true;
            h.result = p_->deliver(sk, s, h, ev, "Watchlist: " + h.list, h.summary, true);
            delivered++;
        }
        std::lock_guard<std::mutex> lk(p_->mu);
        p_->history.push_back(h);
        while (p_->history.size() > kHistoryMax) p_->history.pop_front();
    }
    return delivered;
}

void Watcher::sendTest() {
    Settings s;
    Sink* sk;
    {
        std::lock_guard<std::mutex> lk(p_->mu);
        s = p_->set;
        sk = p_->sink;
    }
    Hit h;
    h.time = au::wallNow();
    h.list = "Test";
    h.kind = "test";
    h.matched = "test";
    h.summary = "This is a test alert from OnAir.";
    au::Event ev("plugin.test");
    ev.at(h.time, "OnAir", 0);
    h.result = p_->deliver(sk, s, h, ev, "OnAir watchlist test", h.summary, true);
    h.delivered = true;
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->history.push_back(h);
    while (p_->history.size() > kHistoryMax) p_->history.pop_front();
}

void Watcher::submit(const au::Event& ev) {
    if (!p_->active.load(std::memory_order_relaxed)) return;
    {
        std::lock_guard<std::mutex> lk(p_->qmu);
        if (p_->q.size() >= kQueueMax) return;
        p_->q.push_back(ev);
    }
    p_->qcv.notify_one();
}

void Watcher::start() {
    if (p_->th.joinable()) return;
    p_->stopReq = false;
    p_->th = dect2::namedThread("onair-wlcore", [this] {
        for (;;) {
            std::deque<au::Event> batch;
            {
                std::unique_lock<std::mutex> lk(p_->qmu);
                p_->qcv.wait(lk, [&] { return p_->stopReq || !p_->q.empty(); });
                if (p_->stopReq) return;
                batch.swap(p_->q);
            }
            for (const au::Event& e : batch) process(e);
        }
    });
}

void Watcher::stop() {
    {
        std::lock_guard<std::mutex> lk(p_->qmu);
        p_->stopReq = true;
    }
    p_->qcv.notify_all();
    if (p_->th.joinable()) p_->th.join();
}

std::vector<Hit> Watcher::history() const { std::lock_guard<std::mutex> lk(p_->mu); return std::vector<Hit>(p_->history.begin(), p_->history.end()); }
void Watcher::clearHistory() { std::lock_guard<std::mutex> lk(p_->mu); p_->history.clear(); }
uint64_t Watcher::alertsSent() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->sent; }

Watcher& global() {
    static Watcher w;
    return w;
}

}  // namespace dect2::watchlist
