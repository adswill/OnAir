#include "web_directory_proto.h"
#include "dect2/mesh/mesh_crypto.h"
#include "web_json.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace onair_web {

namespace mc = dect2::meshcrypto;

std::string dirFreqText(const std::vector<std::pair<long long, long long>>& f) {
    std::string s;
    for (size_t i = 0; i < f.size(); i++) s += (i ? ";" : "") + std::to_string(f[i].first) + "-" + std::to_string(f[i].second);
    return s;
}

std::string dirCanonical(const DirRecord& r) {
    std::string s = "OnAir-Directory-v1\n";
    auto kv = [&](const char* k, const std::string& v) { s += std::string(k) + "=" + v + "\n"; };
    kv("action", r.action);
    kv("id", r.id);
    kv("pub", r.pub);
    kv("ts", std::to_string(r.ts));
    if (r.action == "delete") return s;
    kv("name", r.name);
    kv("url", r.url);
    kv("loc", r.loc);
    kv("freq", dirFreqText(r.freq));
    kv("radio", r.radio);
    kv("cap", std::to_string(r.cap));
    kv("users", std::to_string(r.users));
    kv("interval", std::to_string(r.interval));
    kv("ver", r.ver);
    kv("contact", r.contact);
    kv("desc", r.desc);
    kv("reach", std::to_string(r.reach));
    return s;
}

std::string dirBodyJson(const DirRecord& r, const std::string& sigHex) {
    auto q = [](const std::string& v) { return "\"" + jsonEsc(v) + "\""; };
    std::string s = "{\"v\":1,\"action\":" + q(r.action) + ",\"id\":" + q(r.id) + ",\"pub\":" + q(r.pub) + ",\"ts\":" + std::to_string(r.ts);
    if (r.action != "delete") {
        s += ",\"name\":" + q(r.name) + ",\"url\":" + q(r.url) + ",\"loc\":" + q(r.loc) + ",\"freq\":[";
        for (size_t i = 0; i < r.freq.size(); i++) s += (i ? "," : "") + std::string("[") + std::to_string(r.freq[i].first) + "," + std::to_string(r.freq[i].second) + "]";
        s += "],\"radio\":" + q(r.radio) + ",\"cap\":" + std::to_string(r.cap) + ",\"users\":" + std::to_string(r.users) + ",\"interval\":" + std::to_string(r.interval) +
             ",\"ver\":" + q(r.ver) + ",\"contact\":" + q(r.contact) + ",\"desc\":" + q(r.desc) + ",\"reach\":" + std::to_string(r.reach);
    }
    return s + ",\"sig\":" + q(sigHex) + "}";
}

std::string dirPubHex(const uint8_t seed[32]) {
    uint8_t pub[32];
    mc::ed25519PublicKey(seed, pub);
    return mc::hexEncode(pub, 32);
}

std::string dirSign(const uint8_t seed[32], const DirRecord& r) {
    const std::string t = dirCanonical(r);
    uint8_t sig[64];
    mc::ed25519Sign(seed, (const uint8_t*)t.data(), t.size(), sig);
    return mc::hexEncode(sig, 64);
}

bool dirVerify(const std::string& pubHex, const std::string& sigHex, const DirRecord& r) {
    std::vector<uint8_t> pub, sig;
    if (!mc::hexDecode(pubHex, pub) || pub.size() != 32 || !mc::hexDecode(sigHex, sig) || sig.size() != 64) return false;
    const std::string t = dirCanonical(r);
    return mc::ed25519Verify(sig.data(), (const uint8_t*)t.data(), t.size(), pub.data());
}

std::string dirIdFromPub(const std::string& pubHex) {
    std::vector<uint8_t> pub;
    if (!mc::hexDecode(pubHex, pub) || pub.size() != 32) return "";
    uint8_t h[32];
    mc::sha256(pub.data(), 32, h);
    return "onair-" + mc::hexEncode(h, 8);
}

std::string dirClean(const std::string& s, size_t maxBytes) {
    std::string t;
    for (size_t i = 0; i < s.size(); i++) {
        const unsigned char c = (unsigned char)s[i];
        if (c < 0x20 || c == 0x7f) continue;
        if (c == 0xC2 && i + 1 < s.size() && (unsigned char)s[i + 1] >= 0x80 && (unsigned char)s[i + 1] <= 0x9F) { i++; continue; }   // C1 controls U+0080..U+009F
        if (c == 0xE2 && i + 2 < s.size() && (unsigned char)s[i + 1] == 0x80 && ((unsigned char)s[i + 2] == 0xA8 || (unsigned char)s[i + 2] == 0xA9)) { i += 2; continue; }   // U+2028 / U+2029
        t += (char)c;
    }
    size_t a = 0, b = t.size();
    while (a < b && t[a] == ' ') a++;
    while (b > a && t[b - 1] == ' ') b--;
    t = t.substr(a, b - a);
    if (t.size() > maxBytes) {
        size_t n = maxBytes;
        while (n > 0 && ((unsigned char)t[n] & 0xC0) == 0x80) n--;   // do not cut inside a multi-byte character
        t = t.substr(0, n);
        while (!t.empty() && t.back() == ' ') t.pop_back();
    }
    return t;
}

static bool hostChars(const std::string& h) {
    if (h.empty()) return false;
    for (char c : h) if (!(isalnum((unsigned char)c) || c == '.' || c == '-' || c == ':' || c == '[' || c == ']')) return false;
    return true;
}

bool dirNormalizeUrl(const std::string& in, std::string& out, std::string& err) {
    if (in.size() > 200) { err = "the address is longer than 200 characters"; return false; }
    for (unsigned char c : in) if (c <= 0x20 || c == 0x7f) { err = "the address has spaces or control characters"; return false; }
    std::string s = in;
    std::string scheme;
    if (s.rfind("https://", 0) == 0 || s.rfind("HTTPS://", 0) == 0) scheme = "https";
    else if (s.rfind("http://", 0) == 0 || s.rfind("HTTP://", 0) == 0) { err = "the address must start with https:// (the directory lists HTTPS servers)"; return false; }
    else { err = "the address must start with https://"; return false; }
    s = s.substr(8);
    if (s.find_first_of("?#@\\") != std::string::npos) { err = "the address must not have a user name, query or fragment"; return false; }
    const size_t slash = s.find('/');
    std::string host = s.substr(0, slash), path = slash == std::string::npos ? "/" : s.substr(slash);
    std::transform(host.begin(), host.end(), host.begin(), [](unsigned char c) { return (char)tolower(c); });
    if (!hostChars(host)) { err = "the host name has odd characters"; return false; }
    if (host.size() > 4 && host.compare(host.size() - 4, 4, ":443") == 0 && host.find(']') == std::string::npos) host.resize(host.size() - 4);
    for (char c : path) if (!(isalnum((unsigned char)c) || c == '.' || c == '_' || c == '~' || c == '-' || c == '/')) { err = "the path has odd characters"; return false; }
    if (path.back() != '/') path += '/';
    out = scheme + "://" + host + path;
    return true;
}

bool dirNormalizeEndpoint(const std::string& in, std::string& out, std::string& err) {
    if (in.rfind("http://", 0) == 0 || in.rfind("HTTP://", 0) == 0) {
        std::string rest = in.substr(7);
        std::string host = rest.substr(0, rest.find_first_of("/:"));
        std::transform(host.begin(), host.end(), host.begin(), [](unsigned char c) { return (char)tolower(c); });
        if (host != "127.0.0.1" && host != "localhost" && host != "[::1]") { err = "the address must start with https:// (plain http is only accepted for this computer, for tests)"; return false; }
        if (rest.find_first_of("?#@\\ ") != std::string::npos) { err = "the address must not have a user name, query, fragment or spaces"; return false; }
        const size_t slash = rest.find('/');
        std::string path = slash == std::string::npos ? "/" : rest.substr(slash);
        if (path.back() != '/') path += '/';
        out = "http://" + rest.substr(0, slash) + path;
        return true;
    }
    return dirNormalizeUrl(in, out, err);
}

bool dirNormalizeLoc(const std::string& in, int decimals, std::string& out, std::string& err) {
    std::string s = dirClean(in, 64);
    out.clear();
    if (s.empty()) return true;
    decimals = std::clamp(decimals, 0, 5);
    const size_t comma = s.find(',');
    if (comma != std::string::npos) {
        char *e1 = nullptr, *e2 = nullptr;
        const std::string a = s.substr(0, comma), b = s.substr(comma + 1);
        const double lat = strtod(a.c_str(), &e1), lon = strtod(b.c_str(), &e2);
        if (a.empty() || b.empty() || *e1 || *e2 || !std::isfinite(lat) || !std::isfinite(lon) || lat < -90 || lat > 90 || lon < -180 || lon > 180) {
            err = "the location must be \"latitude,longitude\" in decimal degrees or a Maidenhead locator";
            return false;
        }
        char buf[64];
        snprintf(buf, sizeof buf, "%.*f,%.*f", decimals, lat, decimals, lon);
        out = buf;
        return true;
    }
    // Maidenhead: AA00, AA00aa or AA00aa00
    if (s.size() != 4 && s.size() != 6 && s.size() != 8) { err = "a locator has 4, 6 or 8 characters (e.g. KO24 or KO24pq)"; return false; }
    for (size_t i = 0; i < s.size(); i++) {
        const char c = (char)toupper((unsigned char)s[i]);
        const bool ok = (i < 2) ? (c >= 'A' && c <= 'R') : (i < 4) ? (c >= '0' && c <= '9') : (i < 6) ? (c >= 'A' && c <= 'X') : (c >= '0' && c <= '9');
        if (!ok) { err = "that is not a Maidenhead locator"; return false; }
    }
    out = s;
    return true;
}

bool dirParseFreqMHz(const std::string& in, std::vector<std::pair<long long, long long>>& out, std::string& err) {
    out.clear();
    size_t pos = 0;
    while (pos < in.size()) {
        size_t end = in.find(',', pos);
        if (end == std::string::npos) end = in.size();
        const std::string part = in.substr(pos, end - pos);
        pos = end + 1;
        if (part.find_first_not_of(' ') == std::string::npos) continue;
        const size_t dash = part.find('-');
        char *e1 = nullptr, *e2 = nullptr;
        const std::string sa = dash == std::string::npos ? "" : part.substr(0, dash), sb = dash == std::string::npos ? "" : part.substr(dash + 1);
        const double a = sa.empty() ? NAN : strtod(sa.c_str(), &e1);
        const double b = sb.empty() ? NAN : strtod(sb.c_str(), &e2);
        if (dash == std::string::npos || (e1 && *e1 && *e1 != ' ') || (e2 && *e2 && *e2 != ' ') || !std::isfinite(a) || !std::isfinite(b) || a < 0 || b < a || b > 300000) { err = "web_directory_freq is a list like 88-108,1-30 (MHz)"; return false; }
        out.push_back({std::llround(a * 1e6), std::llround(b * 1e6)});
        if (out.size() > 8) { err = "web_directory_freq has more than 8 ranges"; return false; }
    }
    return true;
}

bool dirBuildRecord(const DirectoryConfig& c, const std::string& infoJson, long long ts, const std::string& id, const std::string& pubHex, DirRecord& out, std::string& err) {
    Json j;
    std::string e;
    if (!jsonParse(infoJson, j, e)) { err = "own info is not valid JSON: " + e; return false; }
    DirRecord r;
    r.id = id;
    r.pub = pubHex;
    r.ts = ts;
    const Json* n = j.get("name");
    r.name = dirClean(c.name.empty() ? (n && n->kind == Json::Str ? n->str : std::string()) : c.name, 48);
    if (r.name.empty()) { err = "web_name is empty"; return false; }
    if (!dirNormalizeUrl(c.publicUrl, r.url, err)) { err = "web_public_url: " + err; return false; }
    if (!dirNormalizeLoc(c.loc, c.locDecimals, r.loc, err)) { err = "web_directory_loc: " + err; return false; }
    if (!c.freq.empty()) {
        if (!dirParseFreqMHz(c.freq, r.freq, err)) return false;
    } else if (const Json* cap = j.get("capture")) {
        const Json* cf = cap->get("centre_hz");
        const Json* rt = cap->get("rate_hz");
        if (cf && rt && cf->kind == Json::Num && rt->kind == Json::Num && rt->num > 0 && cf->num > 0) {
            const long long lo = std::max(0LL, std::llround(cf->num - rt->num / 2)), hi = std::llround(cf->num + rt->num / 2);
            r.freq.push_back({lo, hi});
        }
    }
    r.radio = dirClean(c.radio, 48);
    const Json* mu = j.get("max_users");
    const Json* us = j.get("users");
    r.cap = mu && mu->kind == Json::Num ? std::clamp<long long>((long long)mu->num, 0, 100000) : 0;
    r.users = us && us->kind == Json::Num ? std::clamp<long long>((long long)us->num, 0, 100000) : 0;
    if (r.cap > 0 && r.users > r.cap) r.users = r.cap;
    r.interval = std::clamp(c.intervalSec, 60, 3600);
    r.ver = dirClean(c.version, 24);
    r.contact = dirClean(c.contact, 80);
    r.desc = dirClean(c.desc, 280);
    r.reach = c.reach ? 1 : 0;
    out = r;
    return true;
}

std::string dirAck(const DirRecord& r, const std::string& directoryUrl) {
    DirRecord a = r;
    a.ts = 0;
    a.users = 0;
    a.freq.clear();
    a.ver.clear();
    const std::string t = dirCanonical(a) + "directory=" + directoryUrl + "\n";
    uint8_t h[32];
    mc::sha256((const uint8_t*)t.data(), t.size(), h);
    return mc::hexEncode(h, 6);   // 12 characters: short enough to type, long enough that it is not guessed by accident
}

std::string dirPreview(const DirRecord& r, const std::string& directoryUrl) {
    std::string s = "OnAir Web directory: every " + std::to_string(r.interval) + " s this server will send an HTTPS POST to\n  " + directoryUrl +
                    (directoryUrl.empty() || directoryUrl.back() != '/' ? "/" : "") + "api/v1/heartbeat\nwith exactly this JSON (time, users and capture coverage change at each beat):\n  ";
    s += dirBodyJson(r, "<signature made with this server's key>") + "\n";
    s += "Nothing else is sent: no listener addresses, names or what anyone listens to, only the number of users.\n";
    if (r.reach) s += "web_directory_reach=1: the directory may fetch " + r.url + "web/v1/info about once an hour to check that the server answers.\n";
    return s;
}

} // namespace onair_web

namespace onair_web {

static std::string lowerStr(std::string s) { for (char& c : s) c = (char)tolower((unsigned char)c); return s; }

DirOutcome dirClassifyHttp(int status, const std::string& body, long long skewSec, bool haveSkew) {
    DirOutcome o;
    o.http = status;
    Json j;
    std::string e, why;
    if (jsonParse(body.substr(0, 4000), j, e)) {
        if (const Json* er = j.get("error")) if (er->kind == Json::Str) why = er->str;
        if (const Json* d = j.get("details")) if (d->kind == Json::Arr) for (const Json& x : d->arr) if (x.kind == Json::Str) why += (why.empty() ? "" : "; ") + x.str;
    }
    const std::string lw = lowerStr(why);
    const bool skewBig = haveSkew && (skewSec > 240 || skewSec < -240);
    if (status == 200) { o.ok = true; o.code = "ok"; o.message = "The directory accepted the listing."; return o; }
    if (status == 400 && (lw.find("ts is too far") != std::string::npos || skewBig)) {
        o.code = "clock";
        const long long m = (haveSkew ? (skewSec < 0 ? -skewSec : skewSec) : 0) / 60;
        o.message = "The directory says this computer's clock is wrong" + (haveSkew ? std::string(" (about ") + std::to_string(m) + " min off)" : std::string()) + ".";
        o.advice = "Turn on automatic date and time in your system settings (and the right time zone), then try again. The directory accepts a clock that is at most 5 minutes off.";
    } else if (status == 400) {
        o.code = "invalid";
        o.message = "The directory refused the listing as not valid" + (why.empty() ? std::string(".") : ": " + why + ".");
        o.advice = lw.find("url") != std::string::npos
            ? "The public address is the problem: it must be a public https:// address (not a home-network or localhost address) on a normal port. Use the built-in tunnel, or check the address you typed."
            : "Change the field named above (name, location, radio, description) and list again.";
    } else if (status == 401) {
        o.code = "key";
        o.message = "The directory could not verify this server's signature.";
        o.advice = "The key file in OnAir's webshare folder (web_directory.key) may be damaged. Use 'Remove from directory', delete that key file and list again.";
    } else if (status == 403) {
        o.code = "blocked";
        o.message = "The directory refused this server" + (why.empty() ? std::string(".") : ": " + why + ".");
        o.advice = lw.find("another key") != std::string::npos ? "This id is already listed with another key. Restore the old key file, or wait about 15 minutes for the old entry to expire and list again."
                                                               : "This server or its address is on the directory's block list. If you think that is a mistake, ask on the OnAir Discord.";
    } else if (status == 404) {
        o.code = "not_found";
        o.message = "The directory address answered 'not found'.";
        o.advice = "Check the directory address (the default is https://web.on-air.dev). It must be the address of an OnAir directory.";
    } else if (status == 409) {
        o.code = "conflict";
        o.message = "The directory refused a duplicate" + (why.empty() ? std::string(".") : ": " + why + ".");
        o.retrySoon = lw.find("ts is not newer") != std::string::npos;
        o.advice = o.retrySoon ? "Two copies of this server may be sending at once. OnAir tries again by itself." : "Another server is listed with this address. Stop the other one, or share again to get a new public address.";
    } else if (status == 422) {
        o.code = "resolving";
        o.retrySoon = true;
        o.message = "The directory cannot find your public address in DNS yet.";
        o.advice = "A new tunnel address can take up to a minute to appear. OnAir keeps trying by itself; if it still fails after 5 minutes, stop sharing and share again.";
    } else if (status == 429) {
        o.code = "rate";
        o.message = "The directory says there were too many tries" + (why.empty() ? std::string(".") : ": " + why + ".");
        o.advice = "Wait a few minutes. OnAir will try again by itself; do not press the button repeatedly.";
    } else if (status == 503 && lw.find("full") != std::string::npos) {
        o.code = "full";
        o.message = "The directory is full right now.";
        o.advice = "Try again later. Your server keeps working without a listing.";
    } else if (status >= 500) {
        o.code = "server";
        o.message = "The directory has a problem on its side (error " + std::to_string(status) + ").";
        o.advice = "Nothing is wrong with your setup. OnAir tries again by itself.";
    } else {
        o.code = "other";
        o.message = "The directory answered " + std::to_string(status) + (why.empty() ? std::string(".") : ": " + why + ".");
        o.advice = "Try again; if it keeps happening, copy this message into a bug report.";
    }
    return o;
}

DirOutcome dirClassifyNet(const std::string& cls, const std::string& detail, const std::string& host) {
    DirOutcome o;
    o.code = cls;
    const std::string d = detail.size() > 160 ? detail.substr(0, 160) : detail;
    if (cls == "dns") { o.message = "This computer cannot look up " + host + " (DNS)."; o.advice = "Check that the internet works (open a web page). If it does, a VPN, firewall or DNS filter may block " + host + "; try another network."; o.retrySoon = true; }
    else if (cls == "refused") { o.message = "The connection to " + host + " was refused."; o.advice = "A firewall or proxy is blocking outgoing HTTPS (port 443). Allow OnAir, or try another network."; }
    else if (cls == "timeout") { o.message = "No answer from " + host + " in time."; o.advice = "The network is slow or blocks the connection. Check the internet; OnAir tries again by itself."; o.retrySoon = true; }
    else if (cls == "tls_cert") { o.message = "The certificate of " + host + " is not trusted by this computer."; o.advice = "A proxy or antivirus that inspects HTTPS, a wrong system clock, or a wrong directory address can cause this. Check the date and time, and the directory address."; }
    else if (cls == "tls") { o.message = "The secure (TLS) connection to " + host + " failed."; o.advice = "Try again. If it keeps failing, a proxy or firewall is interfering with HTTPS."; }
    else if (cls == "no_curl") { o.message = "This build has no TLS library and the 'curl' program was not found."; o.advice = "Install curl (Windows 10 and later, macOS and most Linux systems have it), or use a OnAir build with TLS."; }
    else if (cls == "no_tls") { o.message = "This build has no TLS support."; o.advice = "Install curl, or use an OnAir build with TLS."; }
    else if (cls == "proto") { o.message = host + " did not answer like a web server."; o.advice = "Check the directory address."; }
    else { o.code = "other"; o.message = "Could not reach " + host + (d.empty() ? std::string(".") : ": " + d + "."); o.advice = "Check the internet connection and try again."; }
    return o;
}

DirOutcome dirClassifyPublic(int status, bool bodyOk, const std::string& cls, const std::string& url) {
    DirOutcome o;
    o.http = status;
    if (status == 200 && bodyOk) { o.ok = true; o.code = "ok"; o.message = "Your public address " + url + " answers."; return o; }
    o.code = "public";
    if (status == 0) {
        o.message = "Your public address " + url + " does not answer" + (cls.empty() ? std::string(".") : " (" + cls + ").");
        o.advice = "If you use the built-in tunnel: stop sharing and share again to get a fresh address. If you use your own address: your router or firewall blocks the port. Use the built-in tunnel, UPnP, or forward the port by hand (docs/ONAIR-WEB.md, 'Port forwarding').";
    } else if (status == 502 || status == 503 || status == 530 || status == 1033) {
        o.message = "Your public address " + url + " is up, but the OnAir web server behind it does not answer (error " + std::to_string(status) + ").";
        o.advice = "The web server may still be starting or has stopped. Wait a few seconds and test again; if it stays, stop sharing and start again.";
    } else {
        o.message = "Your public address " + url + " answered " + std::to_string(status) + (status == 200 ? ", but not like an OnAir Web server." : ".");
        o.advice = "Check that this address leads to the OnAir web server, not to another program on the same port.";
    }
    return o;
}

std::string dirStatusJson(const DirStatus& s) {
    auto q = [](const std::string& v) { return "\"" + jsonEsc(v) + "\""; };
    std::string o = "{\"state\":" + q(s.state) + ",\"ok\":" + (s.last.ok ? "true" : "false") + ",\"http\":" + std::to_string(s.last.http) + ",\"code\":" + q(s.last.code) +
                    ",\"message\":" + q(s.last.message) + ",\"advice\":" + q(s.last.advice) + ",\"retry_soon\":" + (s.last.retrySoon ? "true" : "false") +
                    ",\"attempts\":" + std::to_string(s.attempts) + ",\"fails\":" + std::to_string(s.fails) + ",\"last_try\":" + std::to_string(s.lastTry) +
                    ",\"last_ok\":" + std::to_string(s.lastOk) + ",\"next_try\":" + std::to_string(s.nextTry) + ",\"have_skew\":" + (s.haveSkew ? "true" : "false") +
                    ",\"skew_sec\":" + std::to_string(s.skewSec) + ",\"id\":" + q(s.id) + ",\"url\":" + q(s.url) + ",\"directory\":" + q(s.directory) + "}";
    return o;
}

bool dirStatusParse(const std::string& text, DirStatus& out) {
    Json j;
    std::string e;
    if (!jsonParse(text, j, e) || j.kind != Json::Obj) return false;
    auto str = [&](const char* k) { const Json* x = j.get(k); return x && x->kind == Json::Str ? x->str : std::string(); };
    auto num = [&](const char* k) { const Json* x = j.get(k); return x && x->kind == Json::Num ? (long long)x->num : 0LL; };
    auto flag = [&](const char* k) { const Json* x = j.get(k); return x && x->kind == Json::Bool && x->b; };
    DirStatus s;
    s.state = str("state");
    if (s.state.empty()) return false;
    s.last.ok = flag("ok"); s.last.http = (int)num("http"); s.last.code = str("code"); s.last.message = str("message"); s.last.advice = str("advice"); s.last.retrySoon = flag("retry_soon");
    s.attempts = num("attempts"); s.fails = num("fails"); s.lastTry = num("last_try"); s.lastOk = num("last_ok"); s.nextTry = num("next_try");
    s.haveSkew = flag("have_skew"); s.skewSec = num("skew_sec");
    s.id = str("id"); s.url = str("url"); s.directory = str("directory");
    out = s;
    return true;
}

} // namespace onair_web

namespace onair_web {

int dirRetryDelay(int fails, bool soon, int intervalSec) {
    static const int kSoon[] = {5, 10, 15, 20, 30, 45, 60};
    static const int kSlow[] = {20, 40, 80, 160, 300};
    const int i = std::max(0, fails - 1);
    int d = soon ? kSoon[std::min(i, 6)] : kSlow[std::min(i, 4)];
    d = std::min(d, std::max(5, intervalSec));
    return std::max(5, d);
}

bool dirParseHttpDate(const std::string& s, long long& out) {
    static const char* mon[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    char wd[8] = {}, mo[8] = {};
    int d = 0, y = 0, hh = 0, mm = 0, ss = 0;
    if (sscanf(s.c_str(), "%3s, %d %3s %d %d:%d:%d", wd, &d, mo, &y, &hh, &mm, &ss) != 7) return false;
    int m = -1;
    for (int i = 0; i < 12; i++) if (strncmp(mo, mon[i], 3) == 0) m = i + 1;
    if (m < 0 || d < 1 || d > 31 || y < 1970 || y > 2200 || hh > 23 || mm > 59 || ss > 60) return false;
    // days from civil (H. Hinnant)
    const int yy = y - (m <= 2 ? 1 : 0);
    const long long era = (yy >= 0 ? yy : yy - 399) / 400;
    const long long yoe = yy - era * 400;
    const long long doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const long long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    const long long days = era * 146097 + doe - 719468;
    out = days * 86400 + hh * 3600 + mm * 60 + ss;
    return true;
}

} // namespace onair_web
