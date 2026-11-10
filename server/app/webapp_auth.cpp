#include "webapp_auth.h"
#include "web/web_server.h"
#include <algorithm>

namespace onair_app {

const char* roleName(Role r) { return r == Role::Owner ? "owner" : r == Role::Viewer ? "viewer" : "none"; }

std::string cookieValue(const std::string& h, const std::string& name) {
    size_t i = 0;
    while (i < h.size()) {
        while (i < h.size() && (h[i] == ' ' || h[i] == ';')) i++;
        const size_t e = h.find(';', i);
        const std::string part = h.substr(i, e == std::string::npos ? std::string::npos : e - i);
        const size_t q = part.find('=');
        if (q != std::string::npos && part.substr(0, q) == name) return part.substr(q + 1);
        if (e == std::string::npos) break;
        i = e + 1;
    }
    return std::string();
}

Sessions::Sessions(AccessConfig c) : cfg_(std::move(c)) {}
void Sessions::setConfig(const AccessConfig& c) { std::lock_guard<std::mutex> g(mu_); cfg_ = c; }
AccessConfig Sessions::config() const { std::lock_guard<std::mutex> g(mu_); return cfg_; }
size_t Sessions::count() const { std::lock_guard<std::mutex> g(mu_); return s_.size(); }

static bool validToken(const std::string& t) {
    if (t.size() != 64) return false;
    for (char c : t) if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    return true;
}

Who Sessions::identify(const std::string& cookieHeader, const std::string& peer, bool loopback, bool viaProxy, double now) {
    std::lock_guard<std::mutex> g(mu_);
    Who w;
    const std::string tok = cookieValue(cookieHeader, "onair_session");
    if (validToken(tok)) {
        auto it = s_.find(tok);
        if (it != s_.end()) {
            if (now - it->second.created > cfg_.sessionHours * 3600) s_.erase(it);
            else {
                it->second.last = now;
                w.role = Role::Owner; w.loggedIn = true; w.csrf = it->second.csrf;
                return w;
            }
        }
    }
    const bool lan = !viaProxy && (loopback || (cfg_.lanIsOwner && onair_web::privateAddress(peer)));
    if (lan) { w.role = Role::Owner; w.implicit = true; w.csrf = "lan"; return w; }
    if (cfg_.publicView) { w.role = Role::Viewer; w.csrf = "public"; return w; }
    w.why = cfg_.passwordHash.empty() ? "setup" : "login";
    return w;
}

bool Sessions::login(const std::string& password, const std::string& peer, double now, std::string& token, std::string& why) {
    std::lock_guard<std::mutex> g(mu_);
    if (fails_.blocked(peer, now)) { why = "too many attempts, wait a minute"; return false; }
    if (cfg_.passwordHash.empty()) { why = "no password set"; return false; }
    if (!onair_web::passwordMatchesHash(password, cfg_.passwordHash)) { fails_.fail(peer, now); why = "wrong password"; return false; }
    fails_.clear(peer);
    if (s_.size() >= 256) {   // the oldest goes
        auto oldest = s_.begin();
        for (auto it = s_.begin(); it != s_.end(); ++it) if (it->second.last < oldest->second.last) oldest = it;
        s_.erase(oldest);
    }
    token = onair_web::newToken(32);
    S s;
    s.csrf = onair_web::newToken(16); s.peer = peer; s.created = now; s.last = now;
    s_[token] = s;
    return true;
}

void Sessions::logout(const std::string& cookieHeader) {
    std::lock_guard<std::mutex> g(mu_);
    s_.erase(cookieValue(cookieHeader, "onair_session"));
}
void Sessions::logoutAll() { std::lock_guard<std::mutex> g(mu_); s_.clear(); }
void Sessions::setPasswordHash(const std::string& hash) { std::lock_guard<std::mutex> g(mu_); cfg_.passwordHash = hash; }

bool Sessions::csrfOk(const Who& w, const std::string& header) const {
    if (w.role == Role::None || w.csrf.empty() || header.empty()) return false;
    return dect2::apipol::tokenEqual(w.csrf, header);
}

bool Sessions::rateOk(const std::string& peer, double now) { std::lock_guard<std::mutex> g(mu_); return rl_.allow(peer, now); }

void Sessions::prune(double now) {
    std::lock_guard<std::mutex> g(mu_);
    for (auto it = s_.begin(); it != s_.end();) {
        if (now - it->second.created > cfg_.sessionHours * 3600) it = s_.erase(it);
        else ++it;
    }
}

bool Sessions::mayControl(const Who& w) const {
    if (w.role == Role::Owner) return true;
    if (w.role == Role::Viewer) { std::lock_guard<std::mutex> g(mu_); return cfg_.publicTune; }
    return false;
}
bool Sessions::mayTransmit(const Who& w) const { return w.role == Role::Owner; }
bool Sessions::maySettings(const Who& w) const { return w.role == Role::Owner; }
bool Sessions::mayRecord(const Who& w) const { return w.role == Role::Owner; }

std::string Sessions::setCookie(const std::string& token, bool tls, double hours) {
    return "Set-Cookie: onair_session=" + token + "; Path=/; HttpOnly; SameSite=Strict; Max-Age=" + std::to_string((long long)(hours * 3600)) + (tls ? "; Secure" : "") + "\r\n";
}
std::string Sessions::clearCookie(bool tls) { return std::string("Set-Cookie: onair_session=; Path=/; HttpOnly; SameSite=Strict; Max-Age=0") + (tls ? "; Secure" : "") + "\r\n"; }

bool Sessions::passwordAcceptable(const std::string& pw, std::string& why) {
    if (pw.size() < 8) { why = "the password needs 8 characters or more"; return false; }
    if (pw.size() > 128) { why = "the password is too long (128 characters at most)"; return false; }
    for (unsigned char c : pw) if (c < 0x20 || c == 0x7F) { why = "the password has a control character"; return false; }
    return true;
}

} // namespace onair_app
