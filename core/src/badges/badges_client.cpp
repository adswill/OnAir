// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Map badges, app side: see badges_client.h and docs/MAP-BADGES-4.0.md.
#include "dect2/badges/badges_client.h"
#include "dect2/automation/json_mini.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace dect2::badges {
namespace aj = automation::json;
namespace fs = std::filesystem;

namespace {
std::string readSmall(const fs::path& p, size_t maxBytes) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (s.size() > maxBytes) return {};
    return s;
}
bool tokenChars(const std::string& t) {
    if (t.size() < 20 || t.size() > 2000) return false;
    for (char c : t) if (!(std::isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.')) return false;
    return true;
}
bool loginChars(const std::string& l) {
    if (l.empty() || l.size() > 39) return false;
    for (char c : l) if (!(std::isalnum((unsigned char)c) || c == '-')) return false;
    return true;
}
std::string clip(const std::string& s, size_t n) { return s.size() > n ? s.substr(0, n) : s; }
bool safeId(const std::string& s) {
    if (s.empty() || s.size() > 24) return false;
    for (char c : s) if (!((c >= 'a' && c <= 'z') || c == '_')) return false;
    return true;
}
}  // namespace

Session readSession(const std::string& dir, int64_t nowSec) {
    Session out;
    if (dir.empty()) return out;
    const std::string t = readSmall(fs::path(dir) / "session.json", 8192);
    aj::Value v; std::string e;
    if (t.empty() || !aj::parse(t, v, e) || v.type != aj::Value::Obj) return out;
    Session s;
    s.token = v.str("token"); s.login = v.str("login"); s.expires = (int64_t)v.num("expires");
    if (!tokenChars(s.token) || !loginChars(s.login) || s.expires <= nowSec) return out;
    return s;
}

std::string authHeader(const Session& s, int64_t nowSec) {
    return s.valid(nowSec) && tokenChars(s.token) ? "Authorization: Bearer " + s.token : std::string();
}

std::string locator6(double lat, double lon) {
    if (!std::isfinite(lat) || !std::isfinite(lon) || lat < -90 || lat > 90 || lon < -180 || lon > 180 || (lat == 0 && lon == 0)) return {};
    double x = std::min(lon + 180.0, 359.999999), y = std::min(lat + 90.0, 179.999999);
    char b[7];
    b[0] = (char)('A' + (int)(x / 20)); b[1] = (char)('A' + (int)(y / 10));
    x = std::fmod(x, 20.0); y = std::fmod(y, 10.0);
    b[2] = (char)('0' + (int)(x / 2)); b[3] = (char)('0' + (int)y);
    x = std::fmod(x, 2.0); y = std::fmod(y, 1.0);
    b[4] = (char)('a' + (int)(x * 12)); b[5] = (char)('a' + (int)(y * 24));
    b[6] = 0;
    return b;
}

IngestAccount parseIngestReply(const std::string& body) {
    IngestAccount a;
    aj::Value v; std::string e;
    if (body.empty() || !aj::parse(body, v, e) || v.type != aj::Value::Obj) return a;
    a.error = clip(v.str("account_error"), 200);
    if (const aj::Value* ac = v.get("account")) if (ac->type == aj::Value::Obj) {
        a.present = true;
        a.login = clip(ac->str("login"), 39);
        a.badgesOn = ac->flag("badges"); a.isPublic = ac->flag("public");
        a.note = clip(ac->str("note"), 40);
    }
    if (const aj::Value* aw = v.get("awards")) if (aw->type == aj::Value::Arr)
        for (const auto& x : aw->a) {
            if (a.awards.size() >= 40) break;
            Award w; w.id = x.str("id"); w.name = clip(x.str("name"), 60); w.icon = clip(x.str("icon"), 16); w.tier = (int)x.num("tier");
            if (safeId(w.id) && w.tier >= 1 && w.tier <= 4 && !w.name.empty()) a.awards.push_back(w);
        }
    return a;
}

std::string toastText(const Award& a) { return "Badge earned: " + a.name; }

ToastQueue::ToastQueue(std::string file) : file_(std::move(file)) {}
void ToastQueue::load() {
    if (loaded_) return;
    loaded_ = true;
    if (file_.empty()) return;
    std::istringstream is(readSmall(file_, 64 * 1024));
    std::string line;
    while (std::getline(is, line)) if (!line.empty() && line.size() < 40) shown_.insert(line);
}
void ToastQueue::save() const {
    if (file_.empty()) return;
    std::error_code ec;
    fs::create_directories(fs::path(file_).parent_path(), ec);
    std::ofstream f(file_, std::ios::binary | std::ios::trunc);
    for (const auto& k : shown_) f << k << '\n';
}
void ToastQueue::push(const std::vector<Award>& awards) {
    std::lock_guard<std::mutex> lk(mu_);
    load();
    bool changed = false;
    for (const auto& a : awards) {
        const std::string k = a.id + ":" + std::to_string(a.tier);
        if (shown_.count(k) || shown_.size() > 2000) continue;
        shown_.insert(k); changed = true;
        if (q_.size() < 20) q_.push_back(a);
    }
    if (changed) save();
}
bool ToastQueue::pop(Award& out) {
    std::lock_guard<std::mutex> lk(mu_);
    if (q_.empty()) return false;
    out = q_.front(); q_.pop_front();
    return true;
}
size_t ToastQueue::pending() const { std::lock_guard<std::mutex> lk(mu_); return q_.size(); }

const char* tierName(int t) {
    switch (t) { case 1: return "Bronze"; case 2: return "Silver"; case 3: return "Gold"; case 4: return "Platinum"; default: return ""; }
}

std::string fmtValue(double v, const std::string& unit) {
    std::string s;
    if (std::fabs(v - std::round(v)) < 1e-9 || std::fabs(v) >= 100) {
        long long n = std::llround(v);
        const bool neg = n < 0; if (neg) n = -n;
        std::string d = std::to_string(n);
        for (int i = (int)d.size() - 3; i > 0; i -= 3) d.insert((size_t)i, ",");
        s = (neg ? "-" : "") + d;
    } else {
        char b[32]; std::snprintf(b, sizeof b, "%.1f", v); s = b;
    }
    return unit.empty() ? s : s + " " + unit;
}

int Profile::earnedCount() const { int n = 0; for (const auto& b : badges) if (b.tier > 0) n++; return n; }

Profile parseMe(const std::string& body) {
    Profile p;
    aj::Value v; std::string e;
    if (body.empty() || !aj::parse(body, v, e) || v.type != aj::Value::Obj) { p.error = "The map server sent an answer OnAir could not read."; return p; }
    p.ok = true;
    p.signedIn = v.flag("signedIn");
    p.configured = v.flag("configured", true);
    p.error = clip(v.str("error"), 200);
    if (!p.signedIn) return p;
    p.login = clip(v.str("login"), 39); p.by = clip(v.str("by"), 24);
    p.badgesOn = v.flag("badges"); p.isPublic = v.flag("public"); p.apps = (int)v.num("apps");
    const aj::Value* pr = v.get("profile");
    if (!pr || pr->type != aj::Value::Obj) return p;
    if (const aj::Value* st = pr->get("stats")) if (st->type == aj::Value::Obj) {
        p.hours = st->num("hours"); p.rangeKm = st->num("rangeKm");
        p.days = (int64_t)st->num("days"); p.streak = (int64_t)st->num("streak"); p.bestStreak = (int64_t)st->num("bestStreak");
        p.objects = (int64_t)st->num("objects");
        if (const aj::Value* c = st->get("countries")) if (c->type == aj::Value::Arr) p.countries = (int64_t)c->a.size();
        if (const aj::Value* c = st->get("counts")) if (c->type == aj::Value::Obj) {
            p.aircraft = (int64_t)c->num("aircraft"); p.ships = (int64_t)c->num("vessel"); p.sondes = (int64_t)c->num("sonde");
            p.aprs = (int64_t)c->num("aprs"); p.sats = (int64_t)c->num("sat"); p.ham = (int64_t)c->num("ham");
            p.fixes = (int64_t)c->num("fix"); p.acars = (int64_t)c->num("acars");
        }
    }
    if (const aj::Value* bs = pr->get("badges")) if (bs->type == aj::Value::Arr)
        for (const auto& x : bs->a) {
            if (p.badges.size() >= 64) break;
            BadgeRow r;
            r.id = x.str("id"); if (!safeId(r.id)) continue;
            r.name = clip(x.str("name", r.id), 60); r.desc = clip(x.str("desc"), 300); r.icon = clip(x.str("icon"), 16); r.unit = clip(x.str("unit"), 4);
            r.tier = std::clamp((int)x.num("tier"), 0, 4); r.value = x.num("value");
            std::vector<double> tiers;
            if (const aj::Value* t = x.get("tiers")) if (t->type == aj::Value::Arr) for (const auto& y : t->a) if (y.type == aj::Value::Num && tiers.size() < 4) tiers.push_back(y.n);
            r.maxTier = (int)tiers.size();
            if (const aj::Value* er = x.get("earned")) if (er->type == aj::Value::Arr && !er->a.empty()) r.earnedAt = (int64_t)er->a.back().num("at");
            if (r.tier < r.maxTier) {
                const double lo = r.tier ? tiers[(size_t)r.tier - 1] : 0, hi = tiers[(size_t)r.tier];
                r.next = hi; r.frac = hi > lo ? std::clamp((r.value - lo) / (hi - lo), 0.0, 1.0) : 0;
                r.progress = fmtValue(r.value, r.unit) + " of " + fmtValue(hi, r.unit);
            } else { r.next = 0; r.frac = 1; r.progress = "Top tier"; }
            if (r.tier > 0 && r.maxTier > 1) r.name += std::string(" (") + tierName(r.tier) + ")";
            p.badges.push_back(std::move(r));
        }
    std::stable_sort(p.badges.begin(), p.badges.end(), [](const BadgeRow& a, const BadgeRow& b) {
        if ((a.tier > 0) != (b.tier > 0)) return a.tier > 0;
        if (a.tier > 0) { if (a.tier != b.tier) return a.tier > b.tier; return a.earnedAt > b.earnedAt; }
        return a.frac > b.frac;
    });
    return p;
}

std::string settingsBody(int badges, int isPublic, const std::string* by) {
    aj::Value o = aj::Value::object();
    if (badges >= 0) o.set("badges", aj::Value::boolean(badges != 0));
    if (isPublic >= 0) o.set("public", aj::Value::boolean(isPublic != 0));
    if (by) o.set("by", aj::Value::string(clip(*by, 80)));
    return aj::dump(o);
}

PanelStep panelStep(bool session, const Profile* me, bool earn, bool sharing) {
    if (me && me->ok && !me->configured) return PanelStep::NotConfigured;
    if (!session || (me && me->ok && !me->signedIn)) return PanelStep::SignIn;
    if (!earn || (me && me->ok && !me->badgesOn)) return PanelStep::TurnOnBadges;
    if (!sharing) return PanelStep::TurnOnSharing;
    return PanelStep::Earning;
}
const char* panelStepText(PanelStep s) {
    switch (s) {
    case PanelStep::NotConfigured: return "Badges are not set up on the map server yet.";
    case PanelStep::SignIn: return "Sign in with GitHub to earn badges for what you share to the live map.";
    case PanelStep::TurnOnBadges: return "Turn on Earn badges to count what you share.";
    case PanelStep::TurnOnSharing: return "Turn on Server > Send data to map server: badges come from what you share.";
    case PanelStep::Earning: return "Earning badges: every object you share counts once.";
    }
    return "";
}
bool sendToken(bool earn, const Session& s, int64_t nowSec) { return earn && s.valid(nowSec); }

std::string profileUrl(const std::string& origin, const std::string& login) {
    std::string o = origin;
    while (!o.empty() && o.back() == '/') o.pop_back();
    return loginChars(login) ? o + "/u/" + login : o + "/leaders";
}

} // namespace dect2::badges
