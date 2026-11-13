// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Live rooms: why a request to the rooms server failed, and the cheap "is the server there" check. No UI, so tests can drive it with a fake server.
#pragma once
#include "../platform/platform.h"
#include <chrono>
#include <functional>
#include <string>

namespace roomsnet {

enum class Fail {
    None,
    Dns,           // the host name does not exist (the server is not deployed yet)
    Offline,       // no connection at all
    Timeout,       // no answer in time
    NotFound,      // HTTP 404 / 410
    Unavailable,   // HTTP 5xx other than "full" (not configured, crashed)
    Full,          // the server has its room limit
    RateLimited,   // HTTP 429
    Refused,       // another 4xx
    BadReply,      // an answer that is not what the server sends
    Other          // TLS or anything else
};

using Transport = std::function<plat::HttpResult(const std::string& method, const std::string& url, const std::string& body, int timeoutSec)>;

Fail classify(const plat::HttpResult& r);                 // None for 2xx/3xx
bool serverMissing(Fail f);                               // Dns, NotFound, Unavailable: the server is not there (yet)
std::string describe(const std::string& url, const plat::HttpResult& r);   // "GET https://... : HTTP 404" for the log and tooltips

struct Avail { bool ok = false; Fail why = Fail::None; std::string detail; };
// One GET of the origin's root with a short timeout.
Avail checkAvailable(const std::string& origin, const Transport& t, int timeoutSec = 3);

// The last check. A good answer is trusted for 10 minutes; "the server is missing" stays until reset() (next start or the retry button);
// other failures (offline, timeout) are not kept, so the next click checks again.
class AvailCache {
public:
    using Clock = std::chrono::steady_clock;
    bool fresh(const std::string& origin, Clock::time_point now) const;
    const Avail& value() const { return v_; }
    void store(const std::string& origin, const Avail& a, Clock::time_point now);
    void reset() { have_ = false; }
private:
    bool have_ = false;
    std::string origin_;
    Avail v_;
    Clock::time_point at_{};
};

}  // namespace roomsnet
