// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/automation/rules.h"
#include "dect2/automation/catalog.h"
#include "dect2/automation/expr.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <ctime>

namespace dect2::automation {

namespace {
constexpr size_t kMaxName = 100, kMaxNote = 600, kMaxTemplate = 4000, kMaxCode = 65536, kMaxRendered = 8000;
constexpr size_t kMaxActions = 8, kMaxParams = 16;

bool isHttp(const std::string& u) {
    if (u.compare(0, 7, "http://") != 0 && u.compare(0, 8, "https://") != 0) return false;
    for (unsigned char c : u) if (c <= 0x20 || c == 0x7F) return false;
    return u.size() < 1000;
}
std::string clip(std::string s, size_t n) { if (s.size() > n) s.resize(n); return s; }

bool knownTrigger(const std::string& t) {
    if (t == "*") return true;
    for (const EventInfo& e : eventCatalog()) if (triggerMatches(t, e.type)) return true;
    return false;
}

std::string getp(const ActionSpec& a, const char* k) {
    auto it = a.p.find(k);
    return it == a.p.end() ? std::string() : it->second;
}
}  // namespace

const std::vector<std::string>& actionTypes() {
    static const std::vector<std::string> v = {"notify", "sound", "record_iq", "record_audio", "save_rewind", "tune", "memory", "log", "webhook", "screenshot", "run"};
    return v;
}

bool validId(const std::string& id) {
    if (id.empty() || id.size() > 48) return false;
    for (unsigned char c : id) if (!std::isalnum(c) && c != '-' && c != '_') return false;
    return true;
}

std::string makeId(const std::string& name) {
    std::string o;
    for (unsigned char c : name) {
        if (std::isalnum(c) && c < 128) o += (char)std::tolower(c);
        else if (!o.empty() && o.back() != '-') o += '-';
    }
    while (!o.empty() && o.back() == '-') o.pop_back();
    if (o.size() > 48) o.resize(48);
    while (!o.empty() && o.back() == '-') o.pop_back();
    return o.empty() ? "rule" : o;
}

std::string hashText(const std::string& s) {
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : s) { h ^= c; h *= 1099511628211ull; }
    char b[20];
    snprintf(b, sizeof b, "%016llx", (unsigned long long)h);
    return b;
}

std::string runFingerprint(const Rule& r) {
    std::string t = r.id;
    for (const ActionSpec& a : r.actions) if (a.type == "run") t += "\n" + getp(a, "program") + "\n" + getp(a, "args");
    return hashText(t);
}

std::string renderTemplate(const std::string& tpl, const Event& ev, const std::string& ruleName, bool asJson) {
    std::string out;
    size_t i = 0;
    auto put = [&](const std::string& v) { out += asJson ? json::escape(v) : v; };
    while (i < tpl.size() && out.size() < kMaxRendered) {
        if (tpl.compare(i, 2, "{{") == 0) {
            const size_t e = tpl.find("}}", i + 2);
            if (e != std::string::npos && e - i < 70) {
                std::string k = tpl.substr(i + 2, e - i - 2);
                const size_t b = k.find_first_not_of(" \t"), z = k.find_last_not_of(" \t");
                k = b == std::string::npos ? std::string() : k.substr(b, z - b + 1);
                if (k == "type") put(ev.type);
                else if (k == "mode") put(ev.mode);
                else if (k == "rule") put(ruleName);
                else if (k == "freq") put(Value::number(ev.freqHz).text());
                else if (k == "freq_mhz") { char b2[40]; snprintf(b2, sizeof b2, "%.4f", ev.freqHz / 1e6); put(b2); }
                else if (k == "time") put(Value::number(std::floor(ev.time)).text());
                else if (k == "time_iso") {
                    const time_t t = (time_t)ev.time;
                    struct tm g;
#ifdef _WIN32
                    gmtime_s(&g, &t);
#else
                    gmtime_r(&t, &g);
#endif
                    char b2[40];
                    strftime(b2, sizeof b2, "%Y-%m-%dT%H:%M:%SZ", &g);
                    put(b2);
                } else { auto it = ev.f.find(k); if (it != ev.f.end()) put(it->second.text()); }
                i = e + 2;
                continue;
            }
        }
        out += tpl[i++];
    }
    if (out.size() > kMaxRendered) out.resize(kMaxRendered);
    return out;
}

std::string csvField(const std::string& s0) {
    std::string s = s0;
    if (!s.empty() && (s[0] == '=' || s[0] == '+' || s[0] == '-' || s[0] == '@' || s[0] == '\t' || s[0] == '\r')) s.insert(s.begin(), '\'');
    if (s.find_first_of(",\"\n\r") == std::string::npos) return s;
    std::string o = "\"";
    for (char c : s) { if (c == '"') o += '"'; o += c; }
    return o + "\"";
}

bool buildWebhook(const ActionSpec& a, const Event& ev, const std::string& ruleName, std::string& url, std::string& body, std::string& err) {
    std::string preset = getp(a, "preset");
    if (preset.empty()) preset = "generic";
    auto R = [&](const char* k, const std::string& def) { const std::string v = getp(a, k); return renderTemplate(v.empty() ? def : v, ev, ruleName, true); };
    const std::string text = R("text", "{{rule}}: {{type}}");
    if (preset == "generic") {
        url = getp(a, "url");
        const std::string b = getp(a, "body");
        if (!b.empty()) body = renderTemplate(b, ev, ruleName, true);
        else {
            json::Value j = json::Value::object();
            j.set("rule", json::Value::string(ruleName));
            j.set("type", json::Value::string(ev.type));
            j.set("mode", json::Value::string(ev.mode));
            j.set("freq_hz", json::Value::number(ev.freqHz));
            j.set("time", json::Value::number(std::floor(ev.time)));
            json::Value f = json::Value::object();
            for (const auto& kv : ev.f) {
                if (kv.second.t == Value::Num) f.set(kv.first, json::Value::number(std::isfinite(kv.second.n) ? kv.second.n : 0));
                else if (kv.second.t == Value::Bool) f.set(kv.first, json::Value::boolean(kv.second.b));
                else f.set(kv.first, json::Value::string(kv.second.s));
            }
            j.set("fields", f);
            body = json::dump(j);
        }
    } else if (preset == "discord") {
        url = getp(a, "url");
        body = "{\"content\":\"" + text + "\"}";
    } else if (preset == "slack") {
        url = getp(a, "url");
        body = "{\"text\":\"" + text + "\"}";
    } else if (preset == "ntfy") {
        const std::string topic = getp(a, "topic");
        if (topic.empty()) { err = "ntfy needs a topic"; return false; }
        url = getp(a, "url");
        if (url.empty()) url = "https://ntfy.sh";
        body = "{\"topic\":\"" + json::escape(topic) + "\",\"title\":\"" + R("title", "{{rule}}") + "\",\"message\":\"" + text + "\"}";
    } else if (preset == "telegram") {
        const std::string tok = getp(a, "token"), chat = getp(a, "chat_id");
        if (tok.empty() || chat.empty()) { err = "Telegram needs a bot token and a chat id"; return false; }
        for (unsigned char c : tok) if (!std::isalnum(c) && c != ':' && c != '_' && c != '-') { err = "bad bot token"; return false; }
        url = "https://api.telegram.org/bot" + tok + "/sendMessage";
        body = "{\"chat_id\":\"" + json::escape(chat) + "\",\"text\":\"" + text + "\"}";
    } else { err = "unknown webhook preset '" + preset + "'"; return false; }
    if (!isHttp(url)) { err = "the webhook address must start with http:// or https://"; return false; }
    return true;
}

namespace { bool validField(const std::string& f); }

std::string validateRule(const Rule& r) {
    if (!validId(r.id)) return "bad rule id";
    if (r.name.empty()) return "the rule has no name";
    if (!knownTrigger(r.trigger)) return "unknown trigger '" + r.trigger + "'";
    for (size_t i = 0; i < r.conds.size(); i++) {
        const Rule::Cond& c = r.conds[i];
        if (!validField(c.field)) return "condition row " + std::to_string(i + 1) + ": bad field name";
        if (std::find(condOps().begin(), condOps().end(), c.op) == condOps().end()) return "condition row " + std::to_string(i + 1) + ": unknown operator";
        if (c.value.size() > 400) return "condition row " + std::to_string(i + 1) + ": value too long";
    }
    if (r.conds.size() > 12) return "too many condition rows (12 at most)";
    ExprError ee;
    if (!Expr::parse(conditionExpr(r), ee)) return "condition: " + ee.text();
    if (!(r.cooldownSec >= 0 && r.cooldownSec <= 7 * 86400)) return "cooldown must be between 0 and a week";
    if (r.maxPerHour < 0 || r.maxPerHour > 10000) return "limit per hour out of range";
    if (r.windowFromHour < -1 || r.windowFromHour > 23 || r.windowToHour < -1 || r.windowToHour > 23 || ((r.windowFromHour < 0) != (r.windowToHour < 0))) return "time window hours must be 0 to 23, both or neither";
    if (r.geo == Rule::GeoCircle) {
        if (!r.geoHome && (!(std::fabs(r.geoLat) <= 90) || !(std::fabs(r.geoLon) <= 180))) return "geofence centre out of range";
        if (!(r.geoRadiusKm > 0 && r.geoRadiusKm <= 20000)) return "geofence radius must be between 0 and 20000 km";
    } else if (r.geo == Rule::GeoPolygon) {
        std::vector<std::pair<double, double>> pts;
        if (!geo::parsePolygon(r.geoPolygon, pts)) return "geofence polygon: write 3 to 64 points as \"lat,lon; lat,lon; ...\"";
    }
    if (r.actions.empty()) return "the rule has no action";
    if (r.actions.size() > kMaxActions) return "too many actions (8 at most)";
    for (const ActionSpec& a : r.actions) {
        const auto& t = actionTypes();
        if (std::find(t.begin(), t.end(), a.type) == t.end()) return "unknown action '" + a.type + "'";
        if (a.type == "notify" && getp(a, "title").empty() && getp(a, "body").empty()) return "notify needs a title or a text";
        if (a.type == "tune" && getp(a, "freq_mhz").empty()) return "tune needs a frequency";
        if (a.type == "memory" && getp(a, "name").empty()) return "memory needs a name";
        if (a.type == "log" && getp(a, "file").empty()) return "log needs a file name";
        if (a.type == "run" && getp(a, "program").empty()) return "run needs a program";
        if (a.type == "webhook") {
            ActionSpec probe = a;
            std::string url, body, err;
            if (!buildWebhook(probe, Event(), "x", url, body, err)) return "webhook: " + err;
        }
    }
    return {};
}

const std::vector<std::string>& condOps() {
    static const std::vector<std::string> v = {"==", "!=", "<", "<=", ">", ">=", "contains", "startswith", "endswith", "matches", "in", "has"};
    return v;
}

namespace {
bool validField(const std::string& f) {
    if (f.empty() || f.size() > 40 || !(std::isalpha((unsigned char)f[0]) || f[0] == '_')) return false;
    for (unsigned char c : f) if (!std::isalnum(c) && c != '_' && c != '.') return false;
    return true;
}
// a number (also with a unit such as 20km or 118.1MHz), true / false, or a quoted text
std::string literalOf(const std::string& v0) {
    size_t b = v0.find_first_not_of(" \t"), e = v0.find_last_not_of(" \t");
    const std::string v = b == std::string::npos ? std::string() : v0.substr(b, e - b + 1);
    if (v == "true" || v == "false") return v;
    size_t i = 0;
    if (i < v.size() && (v[i] == '-' || v[i] == '+')) i++;
    const size_t d0 = i;
    while (i < v.size() && (std::isdigit((unsigned char)v[i]) || v[i] == '.')) i++;
    if (i > d0 && v.find_first_of("0123456789", d0) != std::string::npos) {
        size_t u = i;
        while (u < v.size() && std::isalpha((unsigned char)v[u])) u++;
        std::string unit = v.substr(i, u - i);
        for (char& c : unit) c = (char)std::tolower((unsigned char)c);
        static const char* const units[] = {"", "hz", "khz", "mhz", "ghz", "ms", "s", "min", "h", "km", "nm", "mi", "ft", "kt"};
        bool known = false;
        for (const char* k : units) if (unit == k) known = true;
        if (u == v.size() && known && v[0] != '+') return v;
    }
    std::string o = "'";
    for (char c : v) { if (c == '\\' || c == '\'') o += '\\'; o += c == '\n' ? ' ' : c; }
    return o + "'";
}
}  // namespace

std::string conditionExpr(const Rule& r) {
    std::string rows;
    for (const Rule::Cond& c : r.conds) {
        std::string t;
        if (c.op == "has") t = "has('" + c.field + "')";
        else if (c.op == "in") {
            t = c.field + " in [";
            size_t i = 0;
            bool first = true;
            while (i <= c.value.size()) {
                size_t e = c.value.find(',', i);
                if (e == std::string::npos) e = c.value.size();
                t += (first ? "" : ", ") + literalOf(c.value.substr(i, e - i));
                first = false;
                i = e + 1;
            }
            t += "]";
        } else t = c.field + " " + c.op + " " + literalOf(c.value);
        rows += (rows.empty() ? "" : r.condAny ? " or " : " and ") + t;
    }
    if (rows.empty()) return r.when;
    if (r.when.find_first_not_of(" \t\r\n") == std::string::npos) return rows;
    return "(" + rows + ") and (" + r.when + ")";
}

// ---- JSON ----
namespace {
json::Value actionJson(const ActionSpec& a) {
    json::Value j = json::Value::object();
    j.set("type", json::Value::string(a.type));
    for (const auto& kv : a.p) j.set(kv.first, json::Value::string(kv.second));
    return j;
}
}  // namespace

json::Value ruleToJson(const Rule& r) {
    json::Value j = json::Value::object();
    j.set("id", json::Value::string(r.id));
    j.set("name", json::Value::string(r.name));
    if (!r.note.empty()) j.set("note", json::Value::string(r.note));
    j.set("enabled", json::Value::boolean(r.enabled));
    j.set("trigger", json::Value::string(r.trigger));
    j.set("when", json::Value::string(r.when));
    if (!r.conds.empty()) {
        json::Value rows = json::Value::array();
        for (const Rule::Cond& c : r.conds) {
            json::Value o = json::Value::object();
            o.set("field", json::Value::string(c.field));
            o.set("op", json::Value::string(c.op));
            o.set("value", json::Value::string(c.value));
            rows.push(o);
        }
        j.set("conditions", rows);
        j.set("match", json::Value::string(r.condAny ? "any" : "all"));
    }
    j.set("cooldown_s", json::Value::number(r.cooldownSec));
    if (!r.cooldownKey.empty()) j.set("cooldown_key", json::Value::string(r.cooldownKey));
    j.set("max_per_hour", json::Value::number(r.maxPerHour));
    if (r.windowFromHour >= 0) {
        json::Value w = json::Value::object();
        w.set("from", json::Value::number(r.windowFromHour));
        w.set("to", json::Value::number(r.windowToHour));
        j.set("window", w);
    }
    if (r.geo != Rule::GeoOff) {
        json::Value g = json::Value::object();
        g.set("mode", json::Value::string(r.geo == Rule::GeoCircle ? "circle" : "polygon"));
        if (r.geo == Rule::GeoCircle) {
            g.set("home", json::Value::boolean(r.geoHome));
            g.set("lat", json::Value::number(r.geoLat));
            g.set("lon", json::Value::number(r.geoLon));
            g.set("radius_km", json::Value::number(r.geoRadiusKm));
        } else g.set("polygon", json::Value::string(r.geoPolygon));
        g.set("inside", json::Value::boolean(r.geoInside));
        g.set("enter_only", json::Value::boolean(r.geoEnterOnly));
        j.set("geofence", g);
    }
    json::Value acts = json::Value::array();
    for (const ActionSpec& a : r.actions) acts.push(actionJson(a));
    j.set("actions", acts);
    return j;
}

json::Value scriptToJson(const Script& s) {
    json::Value j = json::Value::object();
    j.set("id", json::Value::string(s.id));
    j.set("name", json::Value::string(s.name));
    if (!s.note.empty()) j.set("note", json::Value::string(s.note));
    j.set("enabled", json::Value::boolean(s.enabled));
    json::Value e = json::Value::array();
    for (const std::string& t : s.events) e.push(json::Value::string(t));
    j.set("events", e);
    j.set("code", json::Value::string(s.code));
    return j;
}

json::Value recipeToJson(const Recipe& r) {
    json::Value j = r.kind == Recipe::KindRule ? ruleToJson(r.rule) : scriptToJson(r.script);
    j.set("format", json::Value::string("onair-automation"));
    j.set("version", json::Value::number(1));
    j.set("kind", json::Value::string(r.kind == Recipe::KindRule ? "rule" : "script"));
    return j;
}

std::string recipeToText(const Recipe& r) { return json::dump(recipeToJson(r), true) + "\n"; }

bool recipeFromText(const std::string& text, Recipe& out, std::string& err, bool fromStore) {
    json::Value j;
    if (!json::parse(text, j, err)) return false;
    if (j.type != json::Value::Obj) { err = "not an OnAir automation file"; return false; }
    if (j.str("format") != "onair-automation") { err = "not an OnAir automation file (\"format\" is missing)"; return false; }
    if (j.num("version", 0) > 1) { err = "this file is from a newer OnAir"; return false; }
    const std::string kind = j.str("kind");
    Recipe r;
    auto str = [&](const char* k, size_t n) { return clip(j.str(k), n); };
    if (kind == "rule") {
        r.kind = Recipe::KindRule;
        Rule& u = r.rule;
        u.name = str("name", kMaxName);
        u.id = j.str("id");
        if (!validId(u.id)) u.id = makeId(u.name);
        u.note = str("note", kMaxNote);
        u.enabled = fromStore ? j.flag("enabled", true) : false;
        u.trigger = str("trigger", 40);
        u.when = str("when", 2000);
        if (const json::Value* cs = j.get("conditions")) if (cs->type == json::Value::Arr) {
            if (cs->a.size() > 12) { err = "too many condition rows (12 at most)"; return false; }
            for (const json::Value& cv : cs->a) u.conds.push_back({clip(cv.str("field"), 40), cv.str("op"), clip(cv.str("value"), 400)});
        }
        u.condAny = j.str("match") == "any";
        u.cooldownSec = j.num("cooldown_s", 60);
        u.cooldownKey = str("cooldown_key", 40);
        u.maxPerHour = (int)std::max(-1.0, std::min(1e6, j.num("max_per_hour", 30)));
        if (const json::Value* w = j.get("window")) { u.windowFromHour = (int)w->num("from", -1); u.windowToHour = (int)w->num("to", -1); }
        if (const json::Value* g = j.get("geofence")) {
            const std::string m = g->str("mode");
            u.geo = m == "circle" ? Rule::GeoCircle : m == "polygon" ? Rule::GeoPolygon : Rule::GeoOff;
            u.geoHome = g->flag("home");
            u.geoLat = g->num("lat"); u.geoLon = g->num("lon"); u.geoRadiusKm = g->num("radius_km", 20);
            u.geoPolygon = clip(g->str("polygon"), 2000);
            u.geoInside = g->flag("inside", true);
            u.geoEnterOnly = g->flag("enter_only");
        }
        const json::Value* acts = j.get("actions");
        if (!acts || acts->type != json::Value::Arr) { err = "\"actions\" is missing"; return false; }
        if (acts->a.size() > kMaxActions) { err = "too many actions (8 at most)"; return false; }
        for (const json::Value& av : acts->a) {
            if (av.type != json::Value::Obj) { err = "an action is not an object"; return false; }
            ActionSpec a;
            a.type = av.str("type");
            for (const auto& kv : av.o) {
                if (kv.first == "type") continue;
                if (a.p.size() >= kMaxParams || kv.first.size() > 32) { err = "an action has too many or too long settings"; return false; }
                if (kv.second.type == json::Value::Str) a.p[kv.first] = clip(kv.second.s, kMaxTemplate);
                else if (kv.second.type == json::Value::Num) a.p[kv.first] = Value::number(kv.second.n).text();
                else if (kv.second.type == json::Value::Bool) a.p[kv.first] = kv.second.b ? "true" : "false";
            }
            u.actions.push_back(a);
        }
        err = validateRule(u);
        if (!err.empty()) return false;
    } else if (kind == "script") {
        r.kind = Recipe::KindScript;
        Script& s = r.script;
        s.name = str("name", kMaxName);
        if (s.name.empty()) { err = "the script has no name"; return false; }
        s.id = j.str("id");
        if (!validId(s.id)) s.id = makeId(s.name);
        s.note = str("note", kMaxNote);
        s.enabled = fromStore ? j.flag("enabled", true) : false;
        if (const json::Value* e = j.get("events")) if (e->type == json::Value::Arr) for (const json::Value& v : e->a) if (v.type == json::Value::Str) {
            if (!knownTrigger(v.s)) { err = "unknown event type '" + v.s + "'"; return false; }
            if (s.events.size() < 32) s.events.push_back(v.s);
        }
        s.code = j.str("code");
        if (s.code.size() > kMaxCode) { err = "the script is longer than 64 KB"; return false; }
        if (s.code.empty()) { err = "the script is empty"; return false; }
    } else { err = "\"kind\" must be \"rule\" or \"script\""; return false; }
    out = std::move(r);
    return true;
}

}  // namespace dect2::automation
