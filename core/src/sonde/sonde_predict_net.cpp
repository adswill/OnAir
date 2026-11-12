// Wind forecasts for the landing prediction (sonde_predict.h): Open-Meteo pressure-level winds of the GFS model (https://open-meteo.com,
// data CC BY 4.0, GFS from NOAA, public domain), parsed into ForecastWinds, fetched in the background and cached as files.
#include "dect2/sonde/sonde_predict.h"
#include "dect2/common/thread_name.h"
#include "dect2/common/timecompat.h"
#include "dect2/mcp/mcp.h"
#include "dect2/tv/iptv.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <thread>

#ifndef ONAIR_VERSION
#define ONAIR_VERSION "dev"
#endif

namespace dect2 {
namespace sondepred {

std::vector<int> openMeteoLevelsHpa() {
    return {1000, 975, 950, 925, 900, 850, 800, 750, 700, 650, 600, 550, 500, 450, 400, 350, 300, 250, 200, 150, 100, 70, 50, 40, 30, 20, 15, 10};
}

namespace {
std::string hourText(double t) {
    struct tm g; gmTime((time_t)t, &g);
    char b[32]; std::snprintf(b, sizeof b, "%04d-%02d-%02dT%02d:00", g.tm_year + 1900, g.tm_mon + 1, g.tm_mday, g.tm_hour);
    return b;
}
double slotStart(double t) { return std::floor(t / 10800.0) * 10800.0; }
}  // namespace

std::string openMeteoUrl(const std::vector<std::pair<double, double>>& pts, double unixT, const std::string& base) {
    std::string la, lo, v;
    char b[48];
    for (const auto& p : pts) {
        std::snprintf(b, sizeof b, "%s%.2f", la.empty() ? "" : ",", p.first); la += b;
        std::snprintf(b, sizeof b, "%s%.2f", lo.empty() ? "" : ",", p.second); lo += b;
    }
    for (int l : openMeteoLevelsHpa()) {
        std::snprintf(b, sizeof b, "%swind_speed_%dhPa,wind_direction_%dhPa,geopotential_height_%dhPa", v.empty() ? "" : ",", l, l, l);
        v += b;
    }
    const double s = slotStart(unixT);
    return base + "/v1/gfs?latitude=" + la + "&longitude=" + lo + "&hourly=" + v + "&wind_speed_unit=ms&timeformat=unixtime&start_hour=" +
           hourText(s - 3600) + "&end_hour=" + hourText(s + 6 * 3600);
}

bool parseOpenMeteo(const std::string& json, ForecastWinds& out, std::string& err) {
    out = ForecastWinds(); out.source = "Open-Meteo GFS";
    mcp::Json j;
    if (!mcp::parseJson(json, j, err)) { err = "the forecast is not JSON (" + err + ")"; return false; }
    std::vector<const mcp::Json*> objs;
    if (j.isArr()) for (const auto& x : j.arr) objs.push_back(&x);
    else objs.push_back(&j);
    const std::vector<int> lev = openMeteoLevelsHpa();
    for (const mcp::Json* o : objs) {
        if (!o->isObj()) continue;
        if (o->boolOr("error", false)) { err = "Open-Meteo: " + o->strOr("reason", "error"); return false; }
        const mcp::Json* h = o->get("hourly");
        if (!h) continue;
        const mcp::Json* tm = h->get("time");
        if (!tm || !tm->isArr() || tm->arr.empty()) continue;
        ForecastWinds::Column c;
        c.lat = o->numOr("latitude", 999); c.lon = o->numOr("longitude", 999);
        if (!(std::fabs(c.lat) <= 90) || !(std::fabs(c.lon) <= 180)) continue;
        const size_t nt = tm->arr.size();
        c.times.resize(nt); c.levels.assign(nt, {});
        for (size_t i = 0; i < nt; i++) c.times[i] = tm->arr[i].isNum() ? tm->arr[i].num : 0;
        char k1[48], k2[48], k3[48];
        for (int l : lev) {
            std::snprintf(k1, sizeof k1, "wind_speed_%dhPa", l); std::snprintf(k2, sizeof k2, "wind_direction_%dhPa", l);
            std::snprintf(k3, sizeof k3, "geopotential_height_%dhPa", l);
            const mcp::Json *s = h->get(k1), *d = h->get(k2), *z = h->get(k3);
            if (!s || !d || !z || !s->isArr() || !d->isArr() || !z->isArr()) continue;
            for (size_t i = 0; i < nt && i < s->arr.size() && i < d->arr.size() && i < z->arr.size(); i++) {
                if (!s->arr[i].isNum() || !d->arr[i].isNum() || !z->arr[i].isNum()) continue;
                const double sp = s->arr[i].num, dir = d->arr[i].num * 3.14159265358979323846 / 180.0;
                ForecastWinds::Level L; L.zM = z->arr[i].num; L.ve = -sp * std::sin(dir); L.vn = -sp * std::cos(dir);   // "from" direction
                c.levels[i].push_back(L);
            }
        }
        bool any = false;
        for (auto& L : c.levels) {
            std::sort(L.begin(), L.end(), [](const ForecastWinds::Level& a, const ForecastWinds::Level& b) { return a.zM < b.zM; });
            any = any || L.size() >= 2;
        }
        // times without levels go
        ForecastWinds::Column k; k.lat = c.lat; k.lon = c.lon;
        for (size_t i = 0; i < nt; i++) if (c.levels[i].size() >= 2 && c.times[i] > 0) { k.times.push_back(c.times[i]); k.levels.push_back(c.levels[i]); }
        if (any && !k.times.empty()) out.cols.push_back(k);
    }
    if (out.cols.empty()) { if (err.empty()) err = "the forecast has no pressure-level winds"; return false; }
    return true;
}

std::vector<std::pair<double, double>> forecastPoints(double lat, double lon, double landLat, double landLon) {
    auto q = [](double x) { return std::round(x * 4.0) / 4.0; };
    std::vector<std::pair<double, double>> p;
    auto add = [&](double a, double b) {
        a = q(a); b = q(b);
        for (const auto& x : p) if (std::fabs(x.first - a) < 1e-6 && std::fabs(x.second - b) < 1e-6) return;
        p.push_back({a, b});
    };
    add(lat, lon);
    if (std::isfinite(landLat) && std::isfinite(landLon)) { add(0.5 * (lat + landLat), 0.5 * (lon + landLon)); add(landLat, landLon); }
    return p;
}

}  // namespace sondepred

// ---- the fetcher ------------------------------------------------------------------------------------------------------------------
struct SondeWindFetcher::Impl {
    mutable std::mutex mu;
    std::map<std::string, sondepred::ForecastWinds> done;
    std::string status, lastUrl;
    double lastAsk = -1e9;
    std::atomic<bool> busy{false};
    std::thread th;
};

SondeWindFetcher::SondeWindFetcher() : d_(new Impl) {}
SondeWindFetcher::~SondeWindFetcher() { if (d_->th.joinable()) d_->th.join(); delete d_; }

std::string SondeWindFetcher::cacheKey(const std::vector<std::pair<double, double>>& pts, double unixT) {
    std::string k = "om";
    char b[64];
    for (const auto& p : pts) { std::snprintf(b, sizeof b, "_%.2f_%.2f", p.first, p.second); k += b; }
    std::snprintf(b, sizeof b, "_%lld", (long long)(std::floor(unixT / 10800.0)));
    k += b;
    return k;
}

namespace {
std::filesystem::path u8p(const std::string& s) { return std::filesystem::path(reinterpret_cast<const char8_t*>(s.c_str())); }
}

bool SondeWindFetcher::request(const std::vector<std::pair<double, double>>& pts, double unixT, double now) {
    if (pts.empty()) return false;
    const std::string key = cacheKey(pts, unixT);
    {
        std::lock_guard<std::mutex> l(d_->mu);
        if (d_->done.count(key)) return true;
    }
    if (!cacheDir.empty()) {   // a file from an earlier run
        std::ifstream in(u8p(cacheDir + "/" + key + ".json"), std::ios::binary);
        if (in) {
            std::stringstream ss; ss << in.rdbuf();
            sondepred::ForecastWinds w; std::string err;
            if (sondepred::parseOpenMeteo(ss.str(), w, err)) {
                std::lock_guard<std::mutex> l(d_->mu);
                d_->done[key] = w; d_->status = "wind forecast: Open-Meteo GFS (cached)";
                return true;
            }
        }
    }
    if (d_->busy.load()) return false;
    {
        std::lock_guard<std::mutex> l(d_->mu);
        if (now - d_->lastAsk < minGapS) return false;
        d_->lastAsk = now;
        d_->lastUrl = sondepred::openMeteoUrl(pts, unixT, base);
    }
    if (d_->th.joinable()) d_->th.join();
    d_->busy = true;
    const std::string u = lastUrl(), dir = cacheDir;
    d_->th = namedThread("onair-sondewind", [this, key, u, dir] {
        iptv::HttpOptions o;
        o.userAgent = std::string("OnAir-") + ONAIR_VERSION;
        o.keepErrorBody = true;
        o.connectTimeoutMs = 15000; o.readTimeoutMs = 30000;
        iptv::HttpResult hr;
        std::string body, msg;
        sondepred::ForecastWinds w;
        bool ok = false;
        if (u.compare(0, 8, "https://") == 0 && !iptv::httpsAvailable()) msg = "this build has no HTTPS support, so the wind forecast cannot be fetched";
        else if (!iptv::httpRequestAll(u, o, body, hr, 4u << 20)) msg = hr.error.empty() ? "no answer from Open-Meteo" : hr.error;
        else {
            std::string err;
            ok = sondepred::parseOpenMeteo(body, w, err);
            msg = ok ? "wind forecast: Open-Meteo GFS" : (hr.status != 200 ? "HTTP " + std::to_string(hr.status) + ": " : std::string()) + err;
        }
        if (ok && !dir.empty()) {
            std::error_code ec;
            std::filesystem::create_directories(u8p(dir), ec);
            std::ofstream of(u8p(dir + "/" + key + ".json"), std::ios::binary);
            if (of) of << body;
        }
        w.fetchedUnix = systemNowSec();
        {
            std::lock_guard<std::mutex> l(d_->mu);
            if (ok) { d_->done[key] = w; if (d_->done.size() > 32) d_->done.erase(d_->done.begin()); }
            d_->status = msg;
        }
        d_->busy = false;
    });
    return true;
}

bool SondeWindFetcher::get(const std::vector<std::pair<double, double>>& pts, double unixT, sondepred::ForecastWinds& out) const {
    std::lock_guard<std::mutex> l(d_->mu);
    auto it = d_->done.find(cacheKey(pts, unixT));
    if (it == d_->done.end()) return false;
    out = it->second;
    return true;
}
bool SondeWindFetcher::busy() const { return d_->busy.load(); }
std::string SondeWindFetcher::status() const { std::lock_guard<std::mutex> l(d_->mu); return d_->status; }
std::string SondeWindFetcher::lastUrl() const { std::lock_guard<std::mutex> l(d_->mu); return d_->lastUrl; }

}  // namespace dect2
