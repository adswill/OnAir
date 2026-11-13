// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// OnAir Web 3.9 access: who a request is, and what it may do (docs/WEB-3.9.md, "Access and security").
//
//  * Owner: knows the password (a login session, a cookie), or is on this computer or (app_lan=1, the default) on the private network and
//    not behind the relay or a proxy. Owners tune, change modes and settings, record and transmit.
//  * Viewer: app_public=1 lets anyone open the page without a login, read-only; app_public_tune=1 also lets them tune and change the mode.
//    Never settings, transmit, recording or private modes. At most app_public_max viewers at once.
//  * None: a login is needed (the page shows the login form).
// Passwords are stored as PBKDF2-HMAC-SHA256 (web_policy.h hashPassword); the plain password is on no disk. Sessions are random 256-bit
// tokens in an HttpOnly SameSite=Strict cookie; every POST and the WebSocket need the session's CSRF token in a header as well (a cross-site
// page cannot set one). Five failed logins from one address block it for a minute; the API has a per-address rate limit.
#pragma once
#include "dect2/net/api_policy.h"
#include "web/web_policy.h"
#include <cstdint>
#include <map>
#include <mutex>
#include <string>

namespace onair_app {

enum class Role { None, Viewer, Owner };
const char* roleName(Role r);

struct AccessConfig {
    std::string passwordHash;   // app_password_hash; empty: no password yet (the first owner on the LAN sets one)
    bool lanIsOwner = true;     // app_lan: computers on the private network are owners without a login
    bool publicView = false;    // app_public: anyone may watch without a login
    bool publicTune = false;    // app_public_tune: ... and tune / change the mode
    int publicMax = 20;         // app_public_max: viewers at once
    double sessionHours = 168;  // app_session_hours: a login lasts this long
};

struct Who {
    Role role = Role::None;
    bool loggedIn = false;      // by password (a session cookie)
    bool implicit = false;      // owner by address (this computer, the LAN)
    std::string csrf;           // the token POSTs must repeat in X-OnAir-CSRF ("lan" for an implicit owner)
    std::string why;            // for Role::None: "login" (a password exists) or "setup" (none yet; an owner on the LAN sets one)
};

std::string cookieValue(const std::string& cookieHeader, const std::string& name);   // "" when absent

class Sessions {
public:
    explicit Sessions(AccessConfig c);
    void setConfig(const AccessConfig& c);
    AccessConfig config() const;

    Who identify(const std::string& cookieHeader, const std::string& peer, bool loopback, bool viaProxy, double now);
    // false: why says "wrong password", "too many attempts, wait a minute" or "no password set"
    bool login(const std::string& password, const std::string& peer, double now, std::string& token, std::string& why);
    void logout(const std::string& cookieHeader);
    bool csrfOk(const Who& w, const std::string& header) const;
    bool rateOk(const std::string& peer, double now);          // the API's per-address limit (30 a second, burst 100)
    size_t count() const;
    void prune(double now);
    void setPasswordHash(const std::string& hash);             // after a password change every session but none is kept (the caller may logout all)
    void logoutAll();

    bool mayControl(const Who& w) const;    // tune, mode, gain, rate, start / stop
    bool mayTransmit(const Who& w) const;   // the owner only
    bool maySettings(const Who& w) const;   // the owner only
    bool mayRecord(const Who& w) const;     // the owner only

    static std::string setCookie(const std::string& token, bool tls, double hours);   // the Set-Cookie header line (with \r\n)
    static std::string clearCookie(bool tls);
    static bool passwordAcceptable(const std::string& pw, std::string& why);   // 8 .. 128 characters

private:
    struct S { std::string csrf, peer; double created = 0, last = 0; };
    mutable std::mutex mu_;
    AccessConfig cfg_;
    std::map<std::string, S> s_;
    onair_web::FailLimiter fails_;
    dect2::apipol::RateLimiter rl_{30, 100, 512};
};

} // namespace onair_app
