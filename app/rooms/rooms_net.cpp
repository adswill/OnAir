// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "rooms_net.h"

namespace roomsnet {

Fail classify(const plat::HttpResult& r) {
    if (r.status == 0) {
        switch (r.net) {
            case 1: return Fail::Dns;
            case 2: return Fail::Offline;
            case 3: return Fail::Timeout;
            default: return Fail::Other;
        }
    }
    if (r.status >= 200 && r.status < 400) return Fail::None;
    if (r.status == 404 || r.status == 410) return Fail::NotFound;
    if (r.status == 429) return Fail::RateLimited;
    if (r.status == 503 && r.body.find("\"full\"") != std::string::npos) return Fail::Full;
    if (r.status >= 500) return Fail::Unavailable;
    return Fail::Refused;
}

bool serverMissing(Fail f) { return f == Fail::Dns || f == Fail::NotFound || f == Fail::Unavailable; }

std::string describe(const std::string& url, const plat::HttpResult& r) {
    std::string s = url + ": ";
    if (r.status) s += "HTTP " + std::to_string(r.status);
    else s += r.detail.empty() ? std::string("no answer") : r.detail;
    return s;
}

Avail checkAvailable(const std::string& origin, const Transport& t, int timeoutSec) {
    const std::string url = origin + "/";
    const plat::HttpResult r = t("GET", url, "", timeoutSec);
    Avail a;
    a.why = classify(r);
    a.ok = a.why == Fail::None;
    a.detail = describe(url, r);
    return a;
}

bool AvailCache::fresh(const std::string& origin, Clock::time_point now) const {
    if (!have_ || origin != origin_) return false;
    if (serverMissing(v_.why)) return true;
    return now - at_ < std::chrono::minutes(10);
}

void AvailCache::store(const std::string& origin, const Avail& a, Clock::time_point now) {
    if (!a.ok && !serverMissing(a.why)) { have_ = false; v_ = a; return; }
    have_ = true; origin_ = origin; v_ = a; at_ = now;
}

}  // namespace roomsnet
