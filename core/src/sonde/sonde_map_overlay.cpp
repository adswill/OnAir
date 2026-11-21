// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The radiosonde map's marks and the Tawhiri flight prediction (sonde_map_overlay.h).
#include "dect2/sonde/sonde_map_overlay.h"
#include "dect2/common/thread_name.h"
#include "dect2/common/timecompat.h"
#include "dect2/mcp/mcp.h"
#include "dect2/tv/iptv.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>

#ifndef ONAIR_VERSION
#define ONAIR_VERSION "dev"
#endif

namespace dect2 {

std::vector<SondeMapMark> sondeMapMarks(const SondeMapBox& b, const KnownSondeSites* known, const std::vector<SondeSite>& learned,
                                        const SondeMapStation& st, size_t maxKnown) {
    auto inside = [&](double la, double lo) {
        if (la < b.south || la > b.north) return false;
        return b.west <= b.east ? (lo >= b.west && lo <= b.east) : (lo >= b.west || lo <= b.east);
    };
    std::vector<SondeMapMark> out;
    for (const SondeSite& s : learned) {
        if (!inside(s.lat, s.lon)) continue;
        SondeMapMark m; m.kind = SondeMapMark::LearnedSite; m.lat = s.lat; m.lon = s.lon; m.label = s.name; m.launches = s.launches;
        out.push_back(m);
    }
    if (known) {
        for (const SondeSiteRef* k : known->inBox(b.south, b.west, b.north, b.east, maxKnown)) {
            bool hidden = false;
            for (const SondeSite& s : learned) if (sondeSiteKm(k->lat, k->lon, s.lat, s.lon) < KnownSondeSites::kNameKm) { hidden = true; break; }
            if (hidden) continue;
            SondeMapMark m; m.kind = SondeMapMark::KnownSite; m.lat = k->lat; m.lon = k->lon; m.label = k->name;
            out.push_back(m);
        }
    }
    if (st.set && std::fabs(st.lat) <= 90 && std::fabs(st.lon) <= 180 && inside(st.lat, st.lon)) {
        SondeMapMark m; m.kind = st.live ? SondeMapMark::ChaseCar : SondeMapMark::Station; m.lat = st.lat; m.lon = st.lon;
        m.label = st.label.empty() ? std::string("My station") : st.label;
        out.push_back(m);
    }
    return out;
}

namespace tawhiri {

double seaLevelDescent(double vDownMs, double altM) { return std::fabs(vDownMs) * std::exp(-std::max(0.0, altM) / 14000.0); }

bool requestFor(const SondeInfo& s, double burstM, double descentMs, double wallUtc, Request& r, std::string* why) {
    auto no = [&](const char* w) { if (why) *why = w; return false; };
    if (!s.hasPos || !(std::fabs(s.lat) <= 90) || !(std::fabs(s.lon) <= 180)) return no("no position yet");
    if (!s.hasVel) return no("no climb rate yet");
    r = Request();
    r.lat = s.lat; r.lon = s.lon; r.altM = s.altM;
    r.unixT = s.hasTime && std::fabs(s.unixTime - wallUtc) < 3600 ? s.unixTime : wallUtc;
    const bool falling = s.vSpeed < -1.0 && s.maxAltM > s.altM + 200;
    if (falling) {
        if (s.altM < 100) return no("on the ground");
        r.descending = true;
        r.burstM = s.altM + 1;
        r.ascentMs = 5;
        r.descentMs = std::max(0.5, seaLevelDescent(s.vSpeed, s.altM));
    } else {
        r.ascentMs = std::max(0.1, s.vSpeed);
        r.burstM = s.altM >= burstM ? s.altM + 100 : burstM;
        r.descentMs = std::max(0.5, descentMs);
    }
    return true;
}

std::string url(const std::string& base, const Request& r) {
    std::string b = base;
    while (!b.empty() && b.back() == '/') b.pop_back();
    double lon = r.lon; if (lon < 0) lon += 360;
    struct tm g; gmTime((time_t)std::floor(r.unixT), &g);
    char t[40]; snprintf(t, sizeof t, "%04d-%02d-%02dT%02d:%02d:%02dZ", g.tm_year + 1900, g.tm_mon + 1, g.tm_mday, g.tm_hour, g.tm_min, g.tm_sec);
    char q[400];
    snprintf(q, sizeof q, "/tawhiri?profile=standard_profile&launch_latitude=%.5f&launch_longitude=%.5f&launch_altitude=%.1f&launch_datetime=%s&ascent_rate=%.2f&burst_altitude=%.1f&descent_rate=%.2f",
             r.lat, lon, r.altM, t, r.ascentMs, r.burstM, r.descentMs);
    return b + q;
}

namespace {
double rfc3339(const std::string& s) {   // "2014-08-20T00:39:59.53125Z"
    int y, mo, d, h, mi; double sec;
    if (std::sscanf(s.c_str(), "%d-%d-%dT%d:%d:%lf", &y, &mo, &d, &h, &mi, &sec) != 6) return 0;
    // days from civil (H. Hinnant)
    y -= mo <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400), doy = (unsigned)((153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1), doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    const long long days = (long long)era * 146097 + (long long)doe - 719468;
    return (double)days * 86400.0 + h * 3600.0 + mi * 60.0 + sec;
}
}  // namespace

Result parse(const std::string& json) {
    Result r;
    mcp::Json j; std::string err;
    if (!mcp::parseJson(json, j, err)) { r.error = "the predictor's answer is not JSON (" + err + ")"; return r; }
    if (const mcp::Json* e = j.get("error")) { r.error = "predictor: " + e->strOr("description", e->strOr("type", "error")); return r; }
    const mcp::Json* pr = j.get("prediction");
    if (!pr || !pr->isArr()) { r.error = "the predictor's answer has no prediction"; return r; }
    for (const mcp::Json& stage : pr->arr) {
        const mcp::Json* tr = stage.get("trajectory");
        if (!tr || !tr->isArr()) continue;
        const bool ascent = stage.strOr("stage") == "ascent";
        for (const mcp::Json& p : tr->arr) {
            Point q;
            q.lat = p.numOr("latitude", 999); q.lon = p.numOr("longitude", 999); q.altM = p.numOr("altitude", 0);
            q.unixT = rfc3339(p.strOr("datetime"));
            if (q.lon > 180 && q.lon <= 360) q.lon -= 360;
            if (!(std::fabs(q.lat) <= 90) || !(std::fabs(q.lon) <= 180)) continue;
            r.path.push_back(q);
        }
        if (ascent && !r.path.empty()) { r.hasBurst = true; r.burst = r.path.back(); }
    }
    if (r.path.size() < 2) { r.error = "the prediction is empty"; r.path.clear(); r.hasBurst = false; return r; }
    r.landing = r.path.back();
    r.ok = true;
    return r;
}

bool plausible(const Result& res, const Request& rq, std::string* why) {
    auto no = [&](const char* m) { if (why) *why = m; return false; };
    if (!res.ok || res.path.size() < 2) return no("empty prediction");
    const double la = res.landing.lat, lo = res.landing.lon;
    if (!std::isfinite(la) || !std::isfinite(lo) || std::fabs(la) > 90 || std::fabs(lo) > 180) return no("landing is not a position");
    const double kx = 111.32e3 * std::cos(rq.lat * 3.14159265358979 / 180.0), ky = 110.57e3;
    double dlo = lo - rq.lon; while (dlo > 180) dlo -= 360; while (dlo < -180) dlo += 360;
    const double distM = std::hypot(dlo * kx, (la - rq.lat) * ky);
    const double heightKm = std::max(0.0, std::max(rq.altM, rq.descending ? 0.0 : rq.burstM)) / 1000.0;
    const double limitM = 15e3 + (rq.descending ? 8e3 : 20e3) * heightKm;   // a falling sonde drifts at most ~8 km per km of height
    if (distM > limitM) return no("landing is too far from the sonde");
    if (res.landing.unixT && rq.unixT && res.landing.unixT < rq.unixT - 120) return no("landing is in the past");
    if (rq.descending && res.landing.altM > rq.altM + 500) return no("landing is above the sonde");
    return true;
}

}  // namespace tawhiri

SondePredictor::~SondePredictor() { if (th_.joinable()) th_.join(); }

bool SondePredictor::request(const std::string& serial, const tawhiri::Request& rq, double now) {
    if (busy_.load()) return false;
    {
        std::lock_guard<std::mutex> l(mu_);
        auto it = asked_.find(serial);
        if (it != asked_.end() && now - it->second < minGapS) return false;
        asked_[serial] = now;
        lastUrl_ = tawhiri::url(base, rq);
    }
    if (th_.joinable()) th_.join();
    busy_ = true;
    const std::string u = lastUrl();
    th_ = namedThread("onair-tawhiri", [this, serial, u, now, rq] {
        iptv::HttpOptions o;
        o.userAgent = std::string("OnAir-") + ONAIR_VERSION;
        o.keepErrorBody = true;
        o.connectTimeoutMs = 15000; o.readTimeoutMs = 30000;
        iptv::HttpResult hr;
        std::string body;
        tawhiri::Result res;
        if (u.compare(0, 8, "https://") == 0 && !iptv::httpsAvailable()) res.error = "this build of OnAir has no HTTPS support, so the predictor cannot be reached";
        else if (!iptv::httpRequestAll(u, o, body, hr, 4u << 20)) res.error = hr.error.empty() ? "no answer from the predictor" : hr.error;
        else {
            res = tawhiri::parse(body);
            if (!res.ok && hr.status != 200 && res.error.find("predictor: ") != 0) res.error = "HTTP " + std::to_string(hr.status) + ": " + res.error;
        }
        if (res.ok) { std::string why; if (!tawhiri::plausible(res, rq, &why)) { res = tawhiri::Result(); res.error = "the prediction was not plausible (" + why + ")"; } }
        {
            std::lock_guard<std::mutex> l(mu_);
            auto old = done_.find(serial);
            if (!res.ok && old != done_.end() && old->second.r.ok) lastNote_ = res.error;   // a failed or wrong answer never replaces a good one
            else { done_[serial] = {res, now}; lastNote_.clear(); }
            if (done_.size() > 64) done_.erase(done_.begin());
        }
        busy_ = false;
    });
    return true;
}

bool SondePredictor::result(const std::string& serial, tawhiri::Result& out, double& made) const {
    std::lock_guard<std::mutex> l(mu_);
    auto it = done_.find(serial);
    if (it == done_.end()) return false;
    out = it->second.r; made = it->second.t;
    return true;
}

std::string SondePredictor::note() const { std::lock_guard<std::mutex> l(mu_); return lastNote_; }
std::string SondePredictor::lastUrl() const { std::lock_guard<std::mutex> l(mu_); return lastUrl_; }

}  // namespace dect2
