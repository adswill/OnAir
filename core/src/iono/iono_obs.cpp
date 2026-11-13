// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/iono/iono_obs.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

#include "dect2/prop/prop.h"

namespace dect2::ionoobs {
namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr double kRad = kPi / 180.0;
constexpr double kDistEdges[kDistBins + 1] = {0, 500, 1000, 2000, 3000, 4500, 6500, 9000, 12000, 1e9};
const char* kDistNames[kDistBins] = {"0-500 km", "500-1000 km", "1000-2000 km", "2000-3000 km", "3000-4500 km",
                                     "4500-6500 km", "6500-9000 km", "9000-12000 km", "12000+ km"};
struct BandDef { const char* name; double loMHz, hiMHz; };
constexpr BandDef kBandDef[kBands] = {{"160m", 1.8, 2.0}, {"80m", 3.5, 4.0}, {"60m", 5.25, 5.45}, {"40m", 7.0, 7.3}, {"30m", 10.1, 10.15},
                                      {"20m", 14.0, 14.35}, {"17m", 18.068, 18.168}, {"15m", 21.0, 21.45}, {"12m", 24.89, 24.99},
                                      {"10m", 28.0, 29.7}};
const char* kSrc[] = {"FT8", "FT4", "WSPR", "HFDL", "BEACON", "TIME", "OTHER"};
}  // namespace

const char* sourceName(Source s) { return kSrc[std::min<int>(static_cast<int>(s), 6)]; }
bool sourceFromName(const std::string& s, Source& out) {
    for (int i = 0; i < 7; ++i) if (s == kSrc[i]) { out = static_cast<Source>(i); return true; }
    return false;
}

double distanceKm(double a, double b, double c, double d) { return prop::greatCircleKm(a, b, c, d); }
double bearingDeg(double lat1, double lon1, double lat2, double lon2) {
    const double p1 = lat1 * kRad, p2 = lat2 * kRad, dl = (lon2 - lon1) * kRad;
    const double y = std::sin(dl) * std::cos(p2), x = std::cos(p1) * std::sin(p2) - std::sin(p1) * std::cos(p2) * std::cos(dl);
    double b = std::atan2(y, x) / kRad;
    if (b < 0) b += 360;
    return b >= 360 ? b - 360 : b;
}
double pathKm(const Obs& o) { return distanceKm(o.rxLat, o.rxLon, o.txLat, o.txLon); }

int distBin(double km) {
    if (!(km >= 0)) return -1;
    for (int i = 0; i < kDistBins; ++i) if (km < kDistEdges[i + 1]) return i;
    return kDistBins - 1;
}
const char* distBinName(int b) { return kDistNames[std::clamp(b, 0, kDistBins - 1)]; }
double distBinMidKm(int b) {
    b = std::clamp(b, 0, kDistBins - 1);
    return b == kDistBins - 1 ? 14000.0 : 0.5 * (kDistEdges[b] + kDistEdges[b + 1]);
}
int bearingBin(double deg) {
    double d = std::fmod(deg, 360.0);
    if (d < 0) d += 360;
    return static_cast<int>(std::floor((d + 22.5) / 45.0)) % 8;
}

int bandOf(double fHz) {
    const double m = fHz / 1e6;
    for (int i = 0; i < kBands; ++i) if (m >= kBandDef[i].loMHz && m <= kBandDef[i].hiMHz) return i;
    return -1;
}
const char* bandName(int b) { return kBandDef[std::clamp(b, 0, kBands - 1)].name; }
double bandCentreMHz(int b) { b = std::clamp(b, 0, kBands - 1); return 0.5 * (kBandDef[b].loMHz + kBandDef[b].hiMHz); }

int64_t windowSeconds(Window w) { return w == Window::H24 ? 86400 : w == Window::D7 ? 7 * 86400 : 30 * 86400; }
std::vector<Obs> inWindow(const std::vector<Obs>& all, int64_t now, Window w) {
    std::vector<Obs> r;
    const int64_t lo = now - windowSeconds(w);
    for (const Obs& o : all) if (o.utc >= lo && o.utc <= now) r.push_back(o);
    return r;
}

int utcHourOf(int64_t utc) { return static_cast<int>(((utc % 86400) + 86400) % 86400 / 3600); }
int HeardGrid::total() const { int t = 0; for (auto& r : count) for (int v : r) t += v; return t; }
HeardGrid heardGrid(const std::vector<Obs>& obs) {
    HeardGrid g;
    for (const Obs& o : obs) {
        const int b = bandOf(o.fHz);
        if (b < 0) continue;
        const int h = utcHourOf(o.utc);
        ++g.count[b][h];
        g.bestSnr[b][h] = std::max(g.bestSnr[b][h], o.snrDb);
    }
    return g;
}

MufRow mufEstimate(const std::vector<Obs>& obs, int minDecodes) {
    MufRow r{};
    for (double& v : r.mufMHz) v = -1;
    for (const Obs& o : obs) {
        const int b = bandOf(o.fHz), d = distBin(pathKm(o));
        if (b >= 0 && d >= 0) ++r.bandCount[d][b];
    }
    for (int d = 0; d < kDistBins; ++d)
        for (int b = kBands - 1; b >= 0; --b)
            if (r.bandCount[d][b] >= std::max(1, minDecodes)) { r.mufMHz[d] = bandCentreMHz(b); break; }
    return r;
}

const char* verdictName(Verdict v) {
    switch (v) {
        case Verdict::BothOpen: return "predicted open, heard";
        case Verdict::BothClosed: return "predicted closed, not heard";
        case Verdict::ObservedOnly: return "heard, not predicted";
        case Verdict::PredictedOnly: return "predicted open, not heard";
        default: return "no data";
    }
}
PredObs compare(const std::vector<Obs>& obs, const std::function<double(double, int)>& mufAt, int utcHour, int minListen) {
    PredObs r{};
    int perBand[kBands] = {};
    int heard[kBands][kDistBins] = {};
    for (const Obs& o : obs) {
        const int b = bandOf(o.fHz), d = distBin(pathKm(o));
        if (b < 0 || d < 0) continue;
        if (utcHour >= 0 && utcHourOf(o.utc) != utcHour) continue;
        ++perBand[b];
        ++heard[b][d];
    }
    const int hour = utcHour >= 0 ? utcHour : 12;
    for (int b = 0; b < kBands; ++b)
        for (int d = 0; d < kDistBins; ++d) {
            Verdict& v = r.v[b][d];
            v = Verdict::NoData;
            if (!mufAt) { if (heard[b][d]) v = Verdict::ObservedOnly; continue; }
            const bool pred = bandCentreMHz(b) <= mufAt(distBinMidKm(d), hour);
            if (heard[b][d]) v = pred ? Verdict::BothOpen : Verdict::ObservedOnly;
            else if (perBand[b] >= minListen) v = pred ? Verdict::PredictedOnly : Verdict::BothClosed;
            if (v == Verdict::BothOpen || v == Verdict::BothClosed) ++r.agree;
            else if (v != Verdict::NoData) ++r.disagree;
        }
    return r;
}

// ---- sun (low precision, good to a fraction of a degree: enough for a terminator line)
void subSolarPoint(int64_t utc, double& lat, double& lon) {
    const double jd = utc / 86400.0 + 2440587.5, n = jd - 2451545.0;
    const double L = std::fmod(280.460 + 0.9856474 * n, 360.0), g = std::fmod(357.528 + 0.9856003 * n, 360.0) * kRad;
    const double lam = (L + 1.915 * std::sin(g) + 0.020 * std::sin(2 * g)) * kRad, eps = (23.439 - 0.0000004 * n) * kRad;
    const double ra = std::atan2(std::cos(eps) * std::sin(lam), std::cos(lam));
    lat = std::asin(std::sin(eps) * std::sin(lam)) / kRad;
    const double gmst = std::fmod(280.46061837 + 360.98564736629 * n, 360.0);
    lon = std::fmod(std::fmod(ra / kRad - gmst, 360.0) + 540.0, 360.0) - 180.0;
}
bool isNight(double lat, double lon, int64_t utc) {
    double sl, so;
    subSolarPoint(utc, sl, so);
    const double c = std::sin(lat * kRad) * std::sin(sl * kRad) + std::cos(lat * kRad) * std::cos(sl * kRad) * std::cos((lon - so) * kRad);
    return c < 0;
}
int terminatorLons(double lat, int64_t utc, double out[2]) {
    double sl, so;
    subSolarPoint(utc, sl, so);
    const double t = -std::tan(lat * kRad) * std::tan(sl * kRad);
    if (!(t > -1 && t < 1)) return 0;
    const double a = std::acos(t) / kRad;   // hour angle of sunrise / sunset
    auto wrap = [](double x) { return std::fmod(std::fmod(x, 360.0) + 540.0, 360.0) - 180.0; };
    out[0] = wrap(so - a);   // sunrise side (the sun is east of it)
    out[1] = wrap(so + a);   // sunset side
    return 2;
}
std::vector<std::pair<double, double>> terminatorLine(int64_t utc, int steps) {
    std::vector<std::pair<double, double>> v;
    for (int i = 0; i < steps; ++i) {
        const double lat = -90.0 + 180.0 * i / std::max(1, steps - 1);
        double l[2];
        if (terminatorLons(lat, utc, l)) v.emplace_back(lat, l[1]);
    }
    return v;
}

uint32_t snrColor(float snr) {
    const float t = std::clamp((snr + 25.f) / 30.f, 0.f, 1.f);   // -25 dB .. +5 dB
    float r, g;
    if (t < 0.5f) { r = 1; g = t * 2; } else { r = 1 - (t - 0.5f) * 2; g = 1; }
    return 0xFF000000u | (uint32_t(0.15f * 255) << 16) | (uint32_t(g * 220) << 8) | uint32_t(r * 255);
}

// ---- history
std::string toLine(const Obs& o) {
    char b[256];
    std::snprintf(b, sizeof b, "%lld\t%.0f\t%s\t%.1f\t%.3f\t%.3f\t%.3f\t%.3f\t", static_cast<long long>(o.utc), o.fHz, sourceName(o.src),
                  static_cast<double>(o.snrDb), o.rxLat, o.rxLon, o.txLat, o.txLon);
    std::string id = o.id;
    for (char& c : id) if (c == '\t' || c == '\n' || c == '\r') c = ' ';
    return std::string(b) + id;
}
bool fromLine(const std::string& line, Obs& o) {
    std::vector<std::string> f;
    std::stringstream ss(line);
    std::string t;
    while (std::getline(ss, t, '\t')) f.push_back(t);
    if (f.size() < 8) return false;
    char* e = nullptr;
    o.utc = std::strtoll(f[0].c_str(), &e, 10);
    if (e == f[0].c_str() || o.utc <= 0) return false;
    o.fHz = std::strtod(f[1].c_str(), nullptr);
    if (!sourceFromName(f[2], o.src)) return false;
    o.snrDb = static_cast<float>(std::strtod(f[3].c_str(), nullptr));
    o.rxLat = std::strtod(f[4].c_str(), nullptr); o.rxLon = std::strtod(f[5].c_str(), nullptr);
    o.txLat = std::strtod(f[6].c_str(), nullptr); o.txLon = std::strtod(f[7].c_str(), nullptr);
    if (std::fabs(o.rxLat) > 90 || std::fabs(o.txLat) > 90 || std::fabs(o.rxLon) > 360 || std::fabs(o.txLon) > 360 || !(o.fHz > 0)) return false;
    o.id = f.size() > 8 ? f[8] : std::string();
    return true;
}
bool addObs(std::vector<Obs>& v, const Obs& o) {
    const int b = bandOf(o.fHz);
    size_t looked = 0;
    for (size_t i = v.size(); i-- > 0 && looked < 256; ++looked)
        if (v[i].id == o.id && v[i].src == o.src && bandOf(v[i].fHz) == b && std::llabs(v[i].utc - o.utc) <= 30) return false;
    v.push_back(o);
    return true;
}
void trim(std::vector<Obs>& v, int64_t now, size_t maxBytes, size_t maxCount) {
    const int64_t lo = now - windowSeconds(Window::D30);
    v.erase(std::remove_if(v.begin(), v.end(), [&](const Obs& o) { return o.utc < lo; }), v.end());
    std::stable_sort(v.begin(), v.end(), [](const Obs& a, const Obs& b) { return a.utc < b.utc; });
    size_t bytes = 0, keepFrom = v.size();
    while (keepFrom > 0 && v.size() - keepFrom < maxCount) {
        const size_t n = toLine(v[keepFrom - 1]).size() + 1;
        if (bytes + n > maxBytes) break;
        bytes += n;
        --keepFrom;
    }
    v.erase(v.begin(), v.begin() + static_cast<long>(keepFrom));
}
bool loadFile(const std::string& path, std::vector<Obs>& out) {
    std::ifstream f(path);
    if (!f) return false;
    std::string l;
    while (std::getline(f, l)) { Obs o; if (fromLine(l, o)) out.push_back(o); }
    return true;
}
bool saveFile(const std::string& path, const std::vector<Obs>& v) {
    const std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::trunc);
        if (!f) return false;
        for (const Obs& o : v) f << toLine(o) << '\n';
        if (!f) return false;
    }
    return std::rename(tmp.c_str(), path.c_str()) == 0;
}
std::string exportCsv(const std::vector<Obs>& v) {
    std::string s = "utc,freq_hz,band,source,snr_db,rx_lat,rx_lon,tx_lat,tx_lon,distance_km,bearing_deg,id\n";
    char b[320];
    for (const Obs& o : v) {
        const int bd = bandOf(o.fHz);
        std::snprintf(b, sizeof b, "%lld,%.0f,%s,%s,%.1f,%.3f,%.3f,%.3f,%.3f,%.0f,%.0f,", static_cast<long long>(o.utc), o.fHz,
                      bd >= 0 ? bandName(bd) : "", sourceName(o.src), static_cast<double>(o.snrDb), o.rxLat, o.rxLon, o.txLat, o.txLon,
                      pathKm(o), bearingDeg(o.rxLat, o.rxLon, o.txLat, o.txLon));
        std::string id = o.id;
        for (char& c : id) if (c == ',' || c == '\n' || c == '\r') c = ' ';
        s += b + id + "\n";
    }
    return s;
}

}  // namespace dect2::ionoobs
