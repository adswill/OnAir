// 4.0 AM broadcast / short wave helpers (pure, no GUI). Tests: tests/d40/test_am_d40.cpp
//  1. carrierOffset      measures the carrier of a broadcast against the nearest 9 or 10 kHz raster channel (the raster that fits better wins): the offset in Hz is what
//                        a synchronous detector has to pull in, and a constant 50-200 Hz offset is how pirates / off-raster transmitters are told from normal stations.
//  2. findHeterodynes    the audio spectrum of a demodulated AM channel: narrow lines between 50 Hz and 8 kHz are the beat notes of another carrier (tone = carrier spacing);
//                        returns each tone and the frequency of the interfering carrier relative to the wanted one (above or below is not known from audio: both given).
//  3. bestSideband       for synchronous detection with selectable sideband: which sideband is cleaner (lower noise floor and fewer interferer peaks); DSB when equal.
//  4. AmDrmSwitch        AM or DRM in the same channel? A carrier standing out of the flat top says AM; a flat ~10 kHz top without a carrier says DRM. Needs 3 agreeing
//                        frames to switch so a fade does not flip it.
//  5. ScheduleTable      station identification by frequency and time from a user-supplied CSV (khz,start HHMM,end HHMM,days 1-7 or 0=daily,name,country): the app never
//                        bundles a third party schedule, the user loads his own (e.g. a EiBi export).
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>

namespace dect2::d40 {

namespace detail {
inline double medianOf(std::vector<double> v) { if (v.empty()) return 0; std::sort(v.begin(), v.end()); return v[v.size() / 2]; }
inline double pctOf(std::vector<double> v, double p) { if (v.empty()) return 0; std::sort(v.begin(), v.end()); return v[std::min(v.size() - 1, (size_t)(p * (double)(v.size() - 1) + 0.5))]; }
}

struct CarrierOffset { bool ok = false; double carrierKhz = 0; int rasterKhz = 0; double offsetHz = 0; double nearestKhz = 0; };
// db: spectrum in dB, centre bin = centreKhz, binHz. Parabolic interpolation of the strongest bin.
inline CarrierOffset carrierOffset(const std::vector<float>& db, double binHz, double centreKhz) {
    CarrierOffset r;
    const int n = (int)db.size();
    if (n < 8 || !(binHz > 0)) return r;
    int pk = 1;
    for (int i = 1; i < n - 1; i++) if (db[(size_t)i] > db[(size_t)pk]) pk = i;
    const double a = db[(size_t)pk - 1], b = db[(size_t)pk], c = db[(size_t)pk + 1];
    const double den = a - 2 * b + c;
    const double d = std::fabs(den) > 1e-9 ? 0.5 * (a - c) / den : 0.0;
    std::vector<double> all(db.begin(), db.end());
    if (b - detail::medianOf(all) < 15) return r;
    r.ok = true;
    r.carrierKhz = centreKhz + ((pk + d) - n / 2) * binHz / 1000.0;
    double best = 1e9;
    for (int ras : {9, 10}) {
        const double nearest = std::round(r.carrierKhz / ras) * ras;
        const double off = (r.carrierKhz - nearest) * 1000.0;
        // the 9 kHz raster only exists below 1.7 MHz in the ITU regions that use it; above that only 5 kHz multiples (10 kHz works as a subset)
        if (std::fabs(off) < std::fabs(best)) { best = off; r.rasterKhz = ras; r.nearestKhz = nearest; r.offsetHz = off; }
    }
    if (r.carrierKhz > 1710) { r.rasterKhz = 5; r.nearestKhz = std::round(r.carrierKhz / 5) * 5; r.offsetHz = (r.carrierKhz - r.nearestKhz) * 1000.0; }
    return r;
}

struct Tone { double hz = 0; double prominenceDb = 0; };
inline std::vector<Tone> findHeterodynes(const std::vector<float>& audioDb, double binHz, double minHz = 50, double maxHz = 8000, double minProminenceDb = 15) {
    std::vector<Tone> out;
    const int n = (int)audioDb.size();
    const int lo = std::max(3, (int)(minHz / binHz)), hi = std::min(n - 4, (int)(maxHz / binHz));
    for (int i = lo; i <= hi; i++) {
        const double v = audioDb[(size_t)i];
        if (!(v > audioDb[(size_t)i - 1] && v >= audioDb[(size_t)i + 1])) continue;
        std::vector<double> nb;
        for (int k = -40; k <= 40; k++) if (std::abs(k) > 3 && i + k >= 0 && i + k < n) nb.push_back(audioDb[(size_t)(i + k)]);
        const double prom = v - detail::medianOf(nb);
        if (prom < minProminenceDb) continue;
        // narrow: 3 bins away must be well below (speech harmonics and music are wider)
        if (v - std::max<double>(audioDb[(size_t)i - 3], audioDb[(size_t)i + 3]) < 10) continue;
        out.push_back({i * binHz, prom});
    }
    std::sort(out.begin(), out.end(), [](const Tone& a, const Tone& b) { return a.prominenceDb > b.prominenceDb; });
    return out;
}

struct SidebandChoice { enum Kind { Dsb, Usb, Lsb } kind = Dsb; double lsbDb = 0, usbDb = 0; double advantageDb = 0; };
// db centred on the carrier; the sidebands are measured from guardHz to bwHz. Cleaner = lower "dirty level" = mean of the strongest quartile (interferers raise it, plain noise barely does).
inline SidebandChoice bestSideband(const std::vector<float>& db, double binHz, double bwHz = 4500, double guardHz = 300, double minAdvDb = 3) {
    SidebandChoice r;
    const int n = (int)db.size(), c = n / 2;
    const int g = std::max(1, (int)(guardHz / binHz)), w = (int)(bwHz / binHz);
    if (c - w < 0 || c + w >= n) return r;
    auto dirty = [&](int sgn) {
        std::vector<double> v;
        for (int k = g; k <= w; k++) v.push_back(db[(size_t)(c + sgn * k)]);
        std::sort(v.begin(), v.end());
        double s = 0; int m = 0;
        for (size_t i = v.size() * 3 / 4; i < v.size(); i++) { s += v[i]; m++; }
        return m ? s / m : 0.0;
    };
    r.usbDb = dirty(+1); r.lsbDb = dirty(-1);
    r.advantageDb = std::fabs(r.usbDb - r.lsbDb);
    if (r.advantageDb >= minAdvDb) r.kind = r.usbDb < r.lsbDb ? SidebandChoice::Usb : SidebandChoice::Lsb;
    return r;
}

class AmDrmSwitch {
public:
    int votesNeeded = 3;
    enum Kind { Unknown, Am, Drm } kind = Unknown;
    // db centred on the channel, binHz small enough to resolve a carrier (<= 100 Hz); channel width ~ 10 kHz
    Kind feed(const std::vector<float>& db, double binHz, double chanHz = 10000) {
        const int n = (int)db.size(), c = n / 2, w = (int)(chanHz / 2 / binHz);
        if (c - w < 0 || c + w >= n || w < 8) return kind;
        // smooth the power over ~300 Hz: single OFDM carriers and speech lines are resolved by a fine FFT, the flat top is only seen after averaging
        const int h = std::max(1, (int)(150.0 / binHz));
        std::vector<double> lin((size_t)n), sm((size_t)n);
        for (int i = 0; i < n; i++) lin[(size_t)i] = std::pow(10.0, (double)db[(size_t)i] / 10.0);
        for (int i = 0; i < n; i++) { double s2 = 0; int m = 0; for (int k = -h; k <= h; k++) if (i + k >= 0 && i + k < n) { s2 += lin[(size_t)(i + k)]; m++; } sm[(size_t)i] = 10 * std::log10(s2 / m + 1e-30); }
        std::vector<double> in, edge, raw;
        const int wi = w * 8 / 10;
        for (int k = -wi; k <= wi; k++) { in.push_back(sm[(size_t)(c + k)]); raw.push_back(db[(size_t)(c + k)]); }
        for (int k = w + w / 2; k < w + w; k++) if (c + k < n) edge.push_back(sm[(size_t)(c + k)]);
        const double top = detail::pctOf(raw, 1.0), med = detail::medianOf(in);
        const double p10 = detail::pctOf(in, 0.1), p90 = detail::pctOf(in, 0.9);
        const double floorDb = edge.empty() ? p10 : detail::medianOf(edge);
        Kind v = Unknown;
        if (med - floorDb < 6) return kind;   // nothing there
        if (top - med > 20) v = Am;           // a carrier line
        else if (p90 - p10 < 6) v = Drm;      // flat top, no line
        if (v == Unknown) return kind;
        if (v == vote_) streak_++; else { vote_ = v; streak_ = 1; }
        if (streak_ >= votesNeeded) kind = v;
        return kind;
    }
private:
    Kind vote_ = Unknown; int streak_ = 0;
};

struct SchedEntry { double khz = 0; int startMin = 0, endMin = 0, days = 0; std::string name, country; };
class ScheduleTable {
public:
    // lines "khz,HHMM,HHMM,days,name,country"; days: 0 = every day, else digits 1-7 (1 = Monday). Returns the number of rows accepted.
    int load(const std::string& csv) {
        std::istringstream in(csv); std::string line; int ok = 0;
        while (std::getline(in, line)) {
            if (line.empty() || line[0] == '#') continue;
            std::vector<std::string> f; std::string cur; std::istringstream ls(line);
            while (std::getline(ls, cur, ',')) f.push_back(cur);
            if (f.size() < 5) continue;
            char* e = nullptr;
            SchedEntry s; s.khz = std::strtod(f[0].c_str(), &e); if (e == f[0].c_str() || s.khz <= 0) continue;
            const int a = std::atoi(f[1].c_str()), b = std::atoi(f[2].c_str());
            if (a < 0 || a > 2400 || b < 0 || b > 2400 || a % 100 > 59 || b % 100 > 59) continue;
            s.startMin = a / 100 * 60 + a % 100; s.endMin = b / 100 * 60 + b % 100;
            for (char ch : f[3]) if (ch >= '1' && ch <= '7') s.days |= 1 << (ch - '1');
            if (f[3] == "0" || f[3].empty()) s.days = 0x7f;
            s.name = f[4]; if (f.size() > 5) s.country = f[5];
            rows_.push_back(s); ok++;
        }
        return ok;
    }
    // weekday 1..7, minute of the UTC day; rows on air within tolKhz, nearest first
    std::vector<SchedEntry> lookup(double khz, int weekday, int utcMin, double tolKhz = 1.0) const {
        std::vector<SchedEntry> out;
        for (const auto& s : rows_) {
            if (std::fabs(s.khz - khz) > tolKhz) continue;
            if (!(s.days & (1 << (weekday - 1)))) continue;
            const bool on = s.endMin > s.startMin ? (utcMin >= s.startMin && utcMin < s.endMin) : (utcMin >= s.startMin || utcMin < s.endMin);   // past midnight
            if (on) out.push_back(s);
        }
        std::sort(out.begin(), out.end(), [&](const SchedEntry& a, const SchedEntry& b) { return std::fabs(a.khz - khz) < std::fabs(b.khz - khz); });
        return out;
    }
    size_t size() const { return rows_.size(); }
private:
    std::vector<SchedEntry> rows_;
};

}  // namespace dect2::d40
