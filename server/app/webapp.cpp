#include "webapp.h"
#include "dect2/net/remote_api.h"
#include "dect2/results/web_schema.h"
#include "web/web_hub.h"
#include "web/web_json.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <thread>

namespace fs = std::filesystem;
using dect2::JsonVal;
using onair_web::AppReply;
using onair_web::AppRequest;

namespace onair_app {

std::string jsonStr(const std::string& s) { return "\"" + onair_web::jsonEsc(s) + "\""; }

// ---------------------------------------------------------------- the settings page
const std::vector<SettingDef>& settingDefs() {
    static const std::vector<SettingDef> d = {
        // Access
        {"app_lan", "Access", "Your own network needs no password", "Computers on your private network (192.168.x.x, 10.x.x.x) are owners without logging in. Turn it off when other people share your network.", "bool", "1", false},
        {"app_public", "Access", "Public: anyone may watch", "People on the internet can open the page without a password and watch (read-only). Needs the relay or a forwarded port to be reachable.", "bool", "0", false},
        {"app_public_tune", "Access", "Public may tune", "Visitors may also change the frequency and the mode. Never transmit, settings or recordings; private modes stay hidden.", "bool", "0", false},
        {"app_public_max", "Access", "Visitors at once", "More visitors than this see 'busy'. Each costs some CPU and bandwidth (see the figures in docs/WEB-3.9.md).", "int", "20", false},
        {"app_session_hours", "Access", "Stay logged in for (hours)", "How long a login lasts on one browser. 168 is one week.", "int", "168", false},
        // Server
        {"web_name", "Server", "Name", "Shown in the page title and to visitors.", "text", "OnAir Web", false},
        {"web_location", "Server", "Location", "Shown to visitors, e.g. a town or a grid square. Leave it empty to show nothing.", "text", "", false},
        {"web_antenna", "Server", "Antenna", "Shown to visitors, e.g. 'discone on the roof'.", "text", "", false},
        {"web_lan", "Server", "Reachable from other computers", "Off: only this computer can open the page (127.0.0.1). On: every computer on the network can.", "bool", "0", true},
        {"web_port", "Server", "Port", "The web port. 8073 is the default; the relay and port forwarding use it.", "int", "8073", true},
        {"web_relay", "Server", "Relay (public address, no port forwarding)", "The server connects out to the relay and gets a public https address; nothing on your router changes.", "bool", "0", true},
        {"web_relay_public", "Server", "List this server on the relay", "The relay shows it in its public list. Off: only people with the address find it.", "bool", "0", true},
        {"web_tls_cert", "Server", "TLS certificate (PEM file)", "With a certificate and key the server speaks https / wss directly. Not needed behind the relay.", "text", "", true},
        {"web_tls_key", "Server", "TLS key (PEM file)", "The private key that belongs to the certificate.", "text", "", true},
        // Radio
        {"autostart", "Radio", "Start receiving when the server starts", "The receiver starts on its own (and again after the radio was unplugged and comes back).", "bool", "0", false},
        {"amp", "Radio", "HackRF: allow the +14 dB amplifier", "Off keeps the gain at 102 dB at most. The amplifier can be damaged by strong signals.", "bool", "0", true},
        {"audio_demod", "Radio", "Listen demodulator", "auto picks by frequency (AM below 30 MHz on the broadcast bands, SSB on the ham bands, NFM above).", "choice:auto|nfm|am|usb|lsb", "auto", false},
        // Audio
        {"app_audio_rate", "Audio", "Audio rate for the page (Hz)", "16000 is clear for voice at 256 kbit/s; 8000 halves the bandwidth; 24000 or 48000 for music.", "choice:8000|12000|16000|24000|48000", "16000", false},
        // Limits
        {"web_max_users", "Limits", "Listeners at once (classic page)", "The classic WebSDR page (/index.html), where every listener has an own receiver.", "int", "8", true},
        {"web_idle_min", "Limits", "Idle minutes before a listener is disconnected", "0 never disconnects.", "int", "30", true},
        // Advanced
        {"web_trust_proxy", "Advanced", "Behind a reverse proxy on this computer", "Takes the visitor's address from X-Forwarded-For. The relay sets this on its own.", "bool", "0", true},
        {"web_plain_internet", "Advanced", "Allow plain http from the internet", "Not recommended: passwords would travel unencrypted. Use the relay or a certificate instead.", "bool", "0", true},
        {"web_log_ip", "Advanced", "Log visitors' addresses", "Off: the log says only 'session 7 joined'.", "bool", "0", true},
        {"app_home", "Advanced", "Open this page at /", "On: http://server:8073/ opens this page. Off: / opens the classic WebSDR page.", "bool", "1", true},
    };
    return d;
}

AccessConfig WebApp::accessFrom(const onair_server::ConfigFile& c) {
    AccessConfig a;
    a.passwordHash = c.get("app_password_hash");
    a.lanIsOwner = c.flag("app_lan", true);
    a.publicView = c.flag("app_public", false);
    a.publicTune = c.flag("app_public_tune", false);
    a.publicMax = std::max(0, std::min(200, (int)c.num("app_public_max", 20)));
    a.sessionHours = std::max(1.0, std::min(24.0 * 365, c.num("app_session_hours", 168)));
    return a;
}

WebApp::WebApp(onair_server::ServerHost& host, onair_server::ConfigFile& cfg, AppOptions o) : host_(host), cfg_(cfg), opt_(std::move(o)), sessions_(accessFrom(cfg)) {}

void WebApp::err(AppReply& out, int status, const std::string& msg) { json(out, status, "{\"error\":" + jsonStr(msg) + "}"); }

Who WebApp::who(const AppRequest& r) { return sessions_.identify(r.header("cookie"), r.peer, r.loopback, r.viaProxy, onair_web::wallSeconds()); }

void WebApp::tick(double now) { sessions_.prune(now); }

bool WebApp::viewerMayWatch(std::string& why) {
    if (!opt_.publicModeAllowed) return true;
    const std::string id = host_.modeId();
    if (opt_.publicModeAllowed(id)) return true;
    why = "this mode is private on this server";
    return false;
}

// ---------------------------------------------------------------- HTTP
bool WebApp::handle(const AppRequest& r, AppReply& out) {
    if (r.path == "/app") { out.status = 302; out.body.clear(); out.type = "text/plain"; out.headers = "Location: /app/\r\n"; return true; }
    if (r.path.compare(0, 8, "/app/v1/") == 0) return api(r, out);
    return serveStatic(r, out);
}

static const char* kPlaceholder =
    "<!doctype html><html><head><meta charset=\"utf-8\"><title>OnAir Web</title></head><body style=\"font-family:system-ui;padding:2em\">"
    "<h1>OnAir Web</h1><p>The page's files were not found. Set <code>app_root</code> in the server's settings file to the folder "
    "<code>web/onair-app</code> of OnAir (it holds index.html).</p><p>The API answers at <a href=\"/app/v1/info\">/app/v1/info</a>.</p></body></html>";

bool WebApp::serveStatic(const AppRequest& r, AppReply& out) {
    if (r.method != "GET" && r.method != "HEAD") { err(out, 405, "GET only"); return true; }
    const std::string csp = "Content-Security-Policy: default-src 'self'; connect-src 'self'; img-src 'self' data: blob:; media-src 'self' blob:; "
                            "style-src 'self' 'unsafe-inline'; script-src 'self'; worker-src 'self' blob:; object-src 'none'; frame-ancestors 'none'; base-uri 'none'; form-action 'self'\r\n";
    std::string sub = r.path.size() > 4 ? r.path.substr(4) : "/";   // "/app/x" -> "/x"
    if (sub.empty() || sub == "/") sub = "/index.html";
    std::string root;
    { std::lock_guard<std::mutex> g(mu_); root = opt_.root; }
    const std::string p = root.empty() ? std::string() : onair_web::staticPath(root, sub);
    if (p.empty()) {
        if (sub == "/index.html") { out.status = 200; out.body = kPlaceholder; out.type = "text/html; charset=utf-8"; out.headers = csp; return true; }
        err(out, 404, "not found");
        return true;
    }
    std::error_code ec;
    const auto sz = fs::file_size(p, ec);
    if (ec || sz > 8 * 1024 * 1024) { err(out, 404, "not found"); return true; }
    std::ifstream f(p, std::ios::binary);
    std::string data((size_t)sz, '\0');
    if (sz && !f.read(&data[0], (std::streamsize)sz)) { err(out, 404, "not found"); return true; }
    out.status = 200; out.body = std::move(data); out.type = onair_web::contentType(p); out.headers = csp;
    return true;
}

std::string WebApp::infoJson(const Who& w, bool tls) {
    const AccessConfig a = sessions_.config();
    std::string name, version;
    { std::lock_guard<std::mutex> g(mu_); name = opt_.name; version = opt_.version; }
    return "{\"name\":" + jsonStr(name) + ",\"version\":" + jsonStr(version) + ",\"role\":\"" + roleName(w.role) + "\",\"loggedIn\":" + (w.loggedIn ? "true" : "false") +
           ",\"needs\":" + jsonStr(w.why) + ",\"hasPassword\":" + (a.passwordHash.empty() ? "false" : "true") + ",\"public\":" + (a.publicView ? "true" : "false") +
           ",\"publicTune\":" + (a.publicTune ? "true" : "false") + ",\"publicMax\":" + std::to_string(a.publicMax) + ",\"tls\":" + (tls ? "true" : "false") +
           ",\"viewers\":" + std::to_string(viewers_.load()) + ",\"connections\":" + std::to_string(conns_.load()) + "}";
}

static int numField(const std::string& j, const char* key, int def) {
    const std::string k = std::string("\"") + key + "\":";
    const size_t p = j.find(k);
    if (p == std::string::npos) return def;
    return atoi(j.c_str() + p + k.size());
}

std::string WebApp::statusJson() {
    std::string s = host_.statusJson();
    if (s.empty() || s.back() != '}') return s;
    s.pop_back();
    const int family = numField(s, "family", -1);
    const std::string id = host_.modeId();
    int n = 0;
    const dect2::webschema::WebModeRow* rows = dect2::webschema::webModeRows(n);
    const dect2::webschema::WebModeRow* row = nullptr;
    for (int i = 0; i < n && !row; i++) if (rows[i].engineStd == family && rows[i].sub[0] && id == rows[i].sub) row = &rows[i];
    for (int i = 0; i < n && !row; i++) if (rows[i].engineStd == family && !rows[i].sub[0]) row = &rows[i];
    s += ",\"row\":";
    if (row) s += "{\"family\":" + std::to_string(row->family) + ",\"name\":" + jsonStr(row->name) + ",\"sub\":" + jsonStr(row->sub) + ",\"group\":" + std::to_string(row->group) + ",\"standard\":" + std::to_string(row->standard) + "}";
    else s += "null";
    std::string x;
    if (host_.handleExtra("GET", "/api/v1/x/server", {}, x) == 200 && x.size() > 2) s += ",\"server\":" + x;
    double c = 0, rt = 0;
    host_.captureInfo(c, rt);
    s += ",\"captureCentreHz\":" + dect2::jsonNum(c) + ",\"captureRateHz\":" + dect2::jsonNum(rt);
    s += ",\"app\":{\"viewers\":" + std::to_string(viewers_.load()) + ",\"connections\":" + std::to_string(conns_.load()) + "}";
    s += '}';
    return s;
}

static bool numArg(const std::map<std::string, JsonVal>& b, const char* k, double& v) {
    auto it = b.find(k);
    if (it == b.end() || it->second.kind != JsonVal::Num || !std::isfinite(it->second.num)) return false;
    v = it->second.num;
    return true;
}
static bool strArg(const std::map<std::string, JsonVal>& b, const char* k, std::string& v) {
    auto it = b.find(k);
    if (it == b.end() || it->second.kind != JsonVal::Str) return false;
    v = it->second.str;
    return true;
}

// what: "start", "stop", "mode", "frequency", "gain", "samplerate", "bandwidth", "demod", "autostart"
bool WebApp::control(const std::string& what, const std::map<std::string, JsonVal>& body, std::string& json, int& status) {
    std::string e;
    double v = 0;
    status = 200;
    json = "{\"ok\":true}";
    if (what == "start") {
        double dev = -1;
        numArg(body, "device", dev);
        if (!host_.start((int)dev, e)) { status = 409; json = "{\"error\":" + jsonStr(e) + "}"; }
        return true;
    }
    if (what == "stop") { if (!host_.stop(e)) { status = 409; json = "{\"error\":" + jsonStr(e) + "}"; } return true; }
    if (what == "frequency") {
        if (!numArg(body, "hz", v) || v < 0 || v > 1e12) { status = 400; json = "{\"error\":\"hz must be a number\"}"; return true; }
        if (!host_.setFrequencyHz(v, e)) { status = 400; json = "{\"error\":" + jsonStr(e) + "}"; }
        return true;
    }
    if (what == "gain") {
        if (!numArg(body, "db", v)) { status = 400; json = "{\"error\":\"db must be a number\"}"; return true; }
        if (!host_.setGainDb(v, e)) { status = 400; json = "{\"error\":" + jsonStr(e) + "}"; }
        return true;
    }
    if (what == "samplerate") {
        if (!numArg(body, "hz", v) || v < 0) { status = 400; json = "{\"error\":\"hz must be a number (0 = automatic)\"}"; return true; }
        if (!host_.setSampleRateHz(v, e)) { status = 400; json = "{\"error\":" + jsonStr(e) + "}"; }
        return true;
    }
    if (what == "bandwidth") {
        if (!numArg(body, "hz", v) || v < 0) { status = 400; json = "{\"error\":\"hz must be a number (0 = the mode's own)\"}"; return true; }
        if (!host_.setCaptureBandwidthHz(v, e)) { status = 400; json = "{\"error\":" + jsonStr(e) + "}"; }
        return true;
    }
    if (what == "mode") {
        std::string sub, id;
        double fam = -1;
        if (strArg(body, "id", id)) {
            const onair_server::ModeEntry* m = host_.findMode(id);
            if (!m) { status = 404; json = "{\"error\":\"no such mode\"}"; return true; }
            if (!host_.setMode(m->family, m->sub, e)) { status = 400; json = "{\"error\":" + jsonStr(e) + "}"; }
            return true;
        }
        if (!numArg(body, "family", fam)) { status = 400; json = "{\"error\":\"give family (and sub) or id\"}"; return true; }
        strArg(body, "sub", sub);
        int n = 0;
        const dect2::webschema::WebModeRow* rows = dect2::webschema::webModeRows(n);
        for (int i = 0; i < n; i++) {
            if (rows[i].family != (int)fam || sub != rows[i].sub) continue;
            if (!host_.setMode(rows[i].engineStd, rows[i].sub, e)) { status = 400; json = "{\"error\":" + jsonStr(e) + "}"; }
            return true;
        }
        status = 404; json = "{\"error\":\"no such mode\"}";
        return true;
    }
    if (what == "demod" || what == "autostart") {
        std::string j;
        const int st = host_.handleExtra("POST", "/api/v1/x/" + what, body, j);
        status = st ? st : 500;
        json = st == 200 ? j : "{\"error\":" + jsonStr(j) + "}";
        return true;
    }
    return false;
}

bool WebApp::api(const AppRequest& r, AppReply& out) {
    const double now = onair_web::wallSeconds();
    if (!sessions_.rateOk(r.peer, now)) { err(out, 429, "too many requests"); return true; }
    const std::string p = r.path.substr(8);   // after /app/v1/
    const bool post = r.method == "POST";
    if (!post && r.method != "GET" && r.method != "HEAD") { err(out, 405, "GET or POST"); return true; }
    Who w = who(r);
    if (p == "info") { json(out, 200, infoJson(w, r.tls)); return true; }
    std::map<std::string, JsonVal> body;
    if (post) {
        if (!r.body.empty() && !dect2::parseFlatJson(r.body, body)) { err(out, 400, "the body must be a flat JSON object"); return true; }
        // the browser's origin must be this server (a cross-site page cannot set the header below either)
        const std::string origin = r.header("origin");
        if (!origin.empty()) {
            const std::string host = r.header("host");
            const bool ok = origin == (r.tls ? "https://" : "http://") + host || (r.viaProxy && origin == "https://" + host);
            if (!ok) { err(out, 403, "this page's origin is not allowed"); return true; }
        }
    }
    if (p == "login" && post) {
        std::string pw, tok, why;
        strArg(body, "password", pw);
        if (!sessions_.login(pw, r.peer, now, tok, why)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            err(out, 401, why);
            return true;
        }
        w = sessions_.identify("onair_session=" + tok, r.peer, r.loopback, r.viaProxy, now);
        json(out, 200, "{\"ok\":true,\"role\":\"owner\",\"csrf\":" + jsonStr(w.csrf) + "}");
        out.headers = Sessions::setCookie(tok, r.tls, sessions_.config().sessionHours);
        return true;
    }
    if (p == "logout" && post) {
        sessions_.logout(r.header("cookie"));
        json(out, 200, "{\"ok\":true}");
        out.headers = Sessions::clearCookie(r.tls);
        return true;
    }
    if (p == "session") {
        json(out, 200, "{\"role\":\"" + std::string(roleName(w.role)) + "\",\"loggedIn\":" + (w.loggedIn ? "true" : "false") + ",\"implicit\":" + (w.implicit ? "true" : "false") +
                           ",\"csrf\":" + jsonStr(w.csrf) + ",\"needs\":" + jsonStr(w.why) + "}");
        return true;
    }
    if (w.role == Role::None) { json(out, 401, "{\"error\":\"login\",\"needs\":" + jsonStr(w.why) + "}"); return true; }
    if (post && !sessions_.csrfOk(w, r.header("x-onair-csrf"))) { err(out, 403, "missing or wrong X-OnAir-CSRF header (GET /app/v1/session gives it)"); return true; }
    // ---- a first password (an owner on this computer or the LAN), or a change
    if (p == "setup" && post) {
        if (w.role != Role::Owner || !w.implicit) { err(out, 403, "only an owner on this computer or the local network can set the first password"); return true; }
        if (!sessions_.config().passwordHash.empty()) { err(out, 409, "a password exists: use password"); return true; }
        std::string pw, why;
        strArg(body, "password", pw);
        if (!Sessions::passwordAcceptable(pw, why)) { err(out, 400, why); return true; }
        const std::string h = onair_web::hashPassword(pw);
        host_.editConfig([&](onair_server::ConfigFile& c) { c.set("app_password_hash", h); });
        sessions_.setPasswordHash(h);
        json(out, 200, "{\"ok\":true}");
        return true;
    }
    if (p == "password" && post) {
        if (w.role != Role::Owner) { err(out, 403, "owner only"); return true; }
        std::string oldPw, pw, why;
        strArg(body, "old", oldPw);
        strArg(body, "password", pw);
        const AccessConfig a = sessions_.config();
        if (w.loggedIn && !a.passwordHash.empty() && !onair_web::passwordMatchesHash(oldPw, a.passwordHash)) { err(out, 401, "the current password is wrong"); return true; }
        if (!Sessions::passwordAcceptable(pw, why)) { err(out, 400, why); return true; }
        const std::string h = onair_web::hashPassword(pw);
        host_.editConfig([&](onair_server::ConfigFile& c) { c.set("app_password_hash", h); });
        sessions_.setPasswordHash(h);
        sessions_.logoutAll();
        json(out, 200, "{\"ok\":true,\"loggedOut\":true}");
        out.headers = Sessions::clearCookie(r.tls);
        return true;
    }
    // ---- reads
    if (!post) {
        if (p == "modes") { json(out, 200, dect2::webschema::webModesJson()); return true; }
        if (p == "schema") { json(out, 200, dect2::webschema::allSchemasJson()); return true; }
        if (p == "devices") { json(out, 200, host_.devicesJson()); return true; }
        if (p == "status") { json(out, 200, statusJson()); return true; }
        if (p == "panel" || p == "telemetry") {
            std::string why;
            if (w.role == Role::Viewer && !viewerMayWatch(why)) { json(out, 200, "{\"seq\":0,\"private\":true,\"why\":" + jsonStr(why) + "}"); return true; }
            uint64_t seq = 0;
            const std::string d = p == "panel" ? host_.panelJson(seq) : host_.telemetryJson(seq);
            json(out, 200, "{\"seq\":" + std::to_string(seq) + ",\"data\":" + d + "}");
            return true;
        }
        if (p == "settings") {
            if (!sessions_.maySettings(w)) { err(out, 403, "owner only"); return true; }
            json(out, 200, settingsJson());
            return true;
        }
        if (p == "viewers") {
            if (!sessions_.maySettings(w)) { err(out, 403, "owner only"); return true; }
            json(out, 200, "{\"viewers\":" + std::to_string(viewers_.load()) + ",\"connections\":" + std::to_string(conns_.load()) + ",\"sessions\":" + std::to_string(sessions_.count()) + "}");
            return true;
        }
        err(out, 404, "no such endpoint");
        return true;
    }
    // ---- controls
    if (p == "settings") {
        if (!sessions_.maySettings(w)) { err(out, 403, "owner only"); return true; }
        std::string why;
        std::vector<std::string> changed;
        if (!applySettings(body, why, changed)) { err(out, 400, why); return true; }
        std::string j = "{\"ok\":true,\"changed\":[";
        for (size_t i = 0; i < changed.size(); i++) { if (i) j += ','; j += jsonStr(changed[i]); }
        json(out, 200, j + "]}");
        return true;
    }
    if (p == "record/start" || p == "record/stop") {
        if (!sessions_.mayRecord(w)) { err(out, 403, "owner only"); return true; }
        std::string e, kind = "iq";
        strArg(body, "kind", kind);
        const bool ok = p == "record/start" ? host_.recordStart(kind, e) : host_.recordStop(e);
        if (!ok) { err(out, 501, e.empty() ? "not supported" : e); return true; }
        json(out, 200, "{\"ok\":true}");
        return true;
    }
    if (p == "tx/start" || p == "tx/stop") {
        if (!sessions_.mayTransmit(w)) { err(out, 403, "the owner only"); return true; }
        std::string e, j;
        const bool ok = p == "tx/start" ? host_.txStart(body, j, e) : host_.txStop(j, e);
        if (!ok) { err(out, 501, e.empty() ? "this server has no transmitter" : e); return true; }
        json(out, 200, j.empty() ? "{\"ok\":true}" : j);
        return true;
    }
    static const char* const ctl[] = {"start", "stop", "mode", "frequency", "gain", "samplerate", "bandwidth", "demod", "autostart"};
    for (const char* c : ctl) {
        if (p != c) continue;
        if (!sessions_.mayControl(w)) { err(out, 403, "watching only: the owner did not allow visitors to tune"); return true; }
        if (w.role == Role::Viewer && p == "mode" && opt_.publicModeAllowed) {
            // a visitor may not switch into a mode the host keeps private
            std::string sub, id;
            double fam = -1;
            if (strArg(body, "id", id)) { if (!opt_.publicModeAllowed(id)) { err(out, 403, "that mode is private on this server"); return true; } }
            else if (numArg(body, "family", fam)) {
                strArg(body, "sub", sub);
                int n = 0;
                const dect2::webschema::WebModeRow* rows = dect2::webschema::webModeRows(n);
                for (int i = 0; i < n; i++) if (rows[i].family == (int)fam && sub == rows[i].sub) {
                    const dect2::ModeTuning* mt = dect2::modeTuning(rows[i].engineStd);
                    const std::string mid = rows[i].sub[0] ? rows[i].sub : mt ? mt->id : "";
                    if (!mid.empty() && !opt_.publicModeAllowed(mid)) { err(out, 403, "that mode is private on this server"); return true; }
                }
            }
        }
        std::string j;
        int st = 200;
        control(p, body, j, st);
        json(out, st, j);
        return true;
    }
    err(out, 404, "no such endpoint");
    return true;
}

// ---------------------------------------------------------------- settings
std::string WebApp::settingsJson() {
    std::lock_guard<std::mutex> g(mu_);
    std::string o = "{\"groups\":[\"Access\",\"Server\",\"Radio\",\"Audio\",\"Limits\",\"Advanced\"],\"hasPassword\":";
    o += cfg_.get("app_password_hash").empty() ? "false" : "true";
    o += ",\"items\":[";
    bool first = true;
    for (const SettingDef& d : settingDefs()) {
        if (!first) o += ',';
        first = false;
        const std::string v = cfg_.get(d.key, d.def);
        o += "{\"key\":" + jsonStr(d.key) + ",\"group\":" + jsonStr(d.group) + ",\"label\":" + jsonStr(d.label) + ",\"help\":" + jsonStr(d.help) +
             ",\"kind\":" + jsonStr(d.kind) + ",\"default\":" + jsonStr(d.def) + ",\"value\":" + jsonStr(v) + ",\"restart\":" + (d.restart ? "true" : "false") + "}";
    }
    return o + "]}";
}

static bool asBool(const JsonVal& v, bool& out) {
    if (v.kind == JsonVal::Bool) { out = v.b; return true; }
    if (v.kind == JsonVal::Num) { out = v.num != 0; return true; }
    if (v.kind == JsonVal::Str) { if (v.str == "1" || v.str == "true" || v.str == "on") { out = true; return true; } if (v.str == "0" || v.str == "false" || v.str == "off") { out = false; return true; } }
    return false;
}

bool WebApp::applySettings(const std::map<std::string, JsonVal>& body, std::string& why, std::vector<std::string>& changed) {
    std::map<std::string, std::string> values;
    for (const auto& kv : body) {
        const SettingDef* d = nullptr;
        for (const SettingDef& x : settingDefs()) if (kv.first == x.key) d = &x;
        if (!d) { why = "unknown setting " + kv.first; return false; }
        const std::string kind = d->kind;
        std::string v;
        if (kind == "bool") { bool b; if (!asBool(kv.second, b)) { why = kv.first + " must be true or false"; return false; } v = b ? "1" : "0"; }
        else if (kind == "int") {
            if (kv.second.kind != JsonVal::Num || !std::isfinite(kv.second.num) || kv.second.num != std::floor(kv.second.num) || kv.second.num < 0 || kv.second.num > 1e9) { why = kv.first + " must be a whole number"; return false; }
            v = std::to_string((long long)kv.second.num);
        } else if (kind.compare(0, 7, "choice:") == 0) {
            const std::string s = kv.second.kind == JsonVal::Str ? kv.second.str : kv.second.kind == JsonVal::Num ? std::to_string((long long)kv.second.num) : "";
            bool ok = false;
            size_t i = 7;
            while (i <= kind.size()) { const size_t e = kind.find('|', i); const std::string c = kind.substr(i, e == std::string::npos ? std::string::npos : e - i); if (c == s) ok = true; if (e == std::string::npos) break; i = e + 1; }
            if (!ok) { why = kv.first + " must be one of " + kind.substr(7); return false; }
            v = s;
        } else {
            if (kv.second.kind != JsonVal::Str || kv.second.str.size() > 300) { why = kv.first + " must be text (300 characters at most)"; return false; }
            for (char c : kv.second.str) if (c == '\n' || c == '\r') { why = kv.first + " must be one line"; return false; }
            v = kv.second.str;
        }
        values[kv.first] = v;
    }
    if (values.empty()) { why = "nothing to change"; return false; }
    host_.editConfig([&](onair_server::ConfigFile& c) { for (const auto& kv : values) c.set(kv.first, kv.second); });
    {
        std::lock_guard<std::mutex> g(mu_);
        for (const auto& kv : values) { cfg_.set(kv.first, kv.second); changed.push_back(kv.first); }
        if (values.count("web_name")) opt_.name = values["web_name"];
    }
    sessions_.setConfig(accessFrom(cfg_));
    return true;
}

// ---------------------------------------------------------------- the WebSocket of one viewer
static void putU32(std::string& s, uint32_t x) { for (int i = 0; i < 4; i++) s.push_back((char)((x >> (8 * i)) & 0xFF)); }

void WebApp::webSocket(onair_web::AppWs& ws) {
    const AppRequest& r = ws.request();
    Who w = who(r);
    auto errMsg = [](const std::string& code, const std::string& m) { return "{\"type\":\"error\",\"code\":" + jsonStr(code) + ",\"message\":" + jsonStr(m) + "}"; };
    if (w.role == Role::None) { ws.sendText(errMsg("login", w.why)); ws.close(4001, "login"); return; }
    // the CSRF token travels in the query (?csrf=...): a page of another origin does not know it
    {
        std::string q = r.query, tok;
        const size_t p = q.find("csrf=");
        if (p != std::string::npos) { tok = q.substr(p + 5); const size_t e = tok.find('&'); if (e != std::string::npos) tok.resize(e); }
        bool ok = false;
        const std::string dec = onair_web::urlDecodeStrict(tok, ok);
        if (!ok || !sessions_.csrfOk(w, dec)) { ws.sendText(errMsg("csrf", "missing or wrong csrf")); ws.close(4003, "csrf"); return; }
    }
    const bool viewer = w.role == Role::Viewer;
    if (viewer) {
        const int cap = sessions_.config().publicMax;
        if (viewers_.fetch_add(1) >= cap) { viewers_--; ws.sendText(errMsg("busy", "this server has as many visitors as it allows")); ws.close(4029, "busy"); return; }
    }
    conns_++;
    struct Done { WebApp& a; bool v; ~Done() { a.conns_--; if (v) a.viewers_--; } } done{*this, viewer};
    ws.sendText("{\"type\":\"hello\",\"role\":\"" + std::string(roleName(w.role)) + "\",\"control\":" + (sessions_.mayControl(w) ? "true" : "false") +
                ",\"owner\":" + (w.role == Role::Owner ? "true" : "false") + ",\"csrf\":" + jsonStr(w.csrf) + "}");

    bool subPanel = true, subStatus = true, subSpec = false, subAudio = false;
    int bins = 1024, fps = 10, panelMs = 500, audioRate = 16000, audioId = -1;
    double nextSpec = 0, nextPanel = 0, nextStatus = 0, nextAudio = 0;
    uint64_t specSeq = 0, panelSeq = 0;
    std::string lastStatus;
    while (true) {
        std::string msg;
        const int k = ws.recv(msg, 20);
        if (k < 0) break;
        if (k > 0) {
            std::map<std::string, JsonVal> j;
            if (!dect2::parseFlatJson(msg, j)) { ws.sendText(errMsg("bad", "not a flat JSON object")); continue; }
            std::string type;
            strArg(j, "type", type);
            double v = 0;
            if (type == "ping") { ws.sendText("{\"type\":\"pong\",\"t\":" + (numArg(j, "t", v) ? dect2::jsonNum(v) : "0") + "}"); continue; }
            if (type == "sub") {
                bool b;
                if (j.count("panel") && asBool(j.at("panel"), b)) subPanel = b;
                if (j.count("status") && asBool(j.at("status"), b)) subStatus = b;
                if (j.count("spectrum") && asBool(j.at("spectrum"), b)) subSpec = b;
                if (numArg(j, "bins", v)) bins = (int)std::max(64.0, std::min(4096.0, v));
                if (numArg(j, "fps", v)) fps = (int)std::max(1.0, std::min(30.0, v));
                if (numArg(j, "panelMs", v)) panelMs = (int)std::max(250.0, std::min(5000.0, v));
                if (numArg(j, "rate", v)) { audioRate = 8000; for (int x : {8000, 12000, 16000, 24000, 48000}) if (x <= v) audioRate = x; }   // the host's rates
                if (j.count("audio") && asBool(j.at("audio"), b)) {
                    std::string why;
                    if (b && viewer && !viewerMayWatch(why)) { ws.sendText(errMsg("private", why)); b = false; }
                    if (b && audioId < 0) { audioId = host_.audioOpen(audioRate); if (audioId < 0) ws.sendText(errMsg("audio", "no audio: too many listeners")); }
                    if (!b && audioId >= 0) { host_.audioClose(audioId); audioId = -1; }
                    subAudio = b && audioId >= 0;
                }
                nextSpec = nextPanel = nextStatus = 0;
                continue;
            }
            if (type == "tune" || type == "gain" || type == "mode") {
                if (!sessions_.mayControl(w)) { ws.sendText(errMsg("forbidden", "watching only")); continue; }
                std::string out;
                int st = 200;
                if (type == "tune") { std::map<std::string, JsonVal> b; if (numArg(j, "hz", v)) { b["hz"].kind = JsonVal::Num; b["hz"].num = v; } control("frequency", b, out, st); }
                else if (type == "gain") { std::map<std::string, JsonVal> b; if (numArg(j, "db", v)) { b["db"].kind = JsonVal::Num; b["db"].num = v; } control("gain", b, out, st); }
                else {
                    std::string id;
                    if (viewer && opt_.publicModeAllowed && strArg(j, "id", id) && !opt_.publicModeAllowed(id)) { ws.sendText(errMsg("private", "that mode is private")); continue; }
                    if (viewer && opt_.publicModeAllowed && !strArg(j, "id", id)) { ws.sendText(errMsg("bad", "use the HTTP API for mode changes by family")); continue; }
                    control("mode", j, out, st);
                }
                if (st != 200) ws.sendText(errMsg("refused", out));
                nextStatus = 0;
                continue;
            }
            if (type == "bye") break;
            ws.sendText(errMsg("bad", "unknown message type"));
            continue;
        }
        const double now = onair_web::wallSeconds();
        bool ok = true;
        if (subStatus && now >= nextStatus) {
            nextStatus = now + 1.0;
            const std::string s = statusJson();
            if (s != lastStatus) { lastStatus = s; ok = ws.sendText("{\"type\":\"status\",\"data\":" + s + "}"); }
        }
        if (ok && subPanel && now >= nextPanel) {
            nextPanel = now + panelMs / 1000.0;
            std::string why;
            if (viewer && !viewerMayWatch(why)) {
                if (panelSeq != (uint64_t)-1) { panelSeq = (uint64_t)-1; ok = ws.sendText("{\"type\":\"panel\",\"private\":true,\"why\":" + jsonStr(why) + "}"); }
            } else {
                uint64_t seq = 0;
                const std::string d = host_.panelJson(seq);
                if (seq != panelSeq) { panelSeq = seq; ok = ws.sendText("{\"type\":\"panel\",\"seq\":" + std::to_string(seq) + ",\"data\":" + d + "}"); }
            }
        }
        if (ok && subSpec && now >= nextSpec) {
            nextSpec = now + 1.0 / fps;
            uint64_t seq = 0;
            std::string frame;
            if (host_.spectrumBinary(bins, seq, frame) && seq != specSeq) { specSeq = seq; ok = ws.sendBinary(frame); }
        }
        if (ok && subAudio && audioId >= 0 && now >= nextAudio) {
            nextAudio = now + 0.04;
            std::string pcm;
            if (!host_.audioRead(audioId, pcm)) { subAudio = false; audioId = -1; ws.sendText(errMsg("audio", "the audio stopped")); }
            else if (!pcm.empty()) {
                std::string f;
                f.push_back('\x02');
                putU32(f, (uint32_t)audioRate);
                f += pcm;
                ok = ws.sendBinary(f);
            }
        }
        if (!ok) break;
    }
    if (audioId >= 0) host_.audioClose(audioId);
}

} // namespace onair_app
