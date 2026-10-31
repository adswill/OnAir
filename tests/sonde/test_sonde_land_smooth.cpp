// Smoothed landing prediction: a synthetic parachute descent that spirals (radius 40 m, 8 s period: 31 m/s of swing on top of a 10 to 20 m/s
// wind) in a sheared wind field. The old instantaneous prediction jumps by kilometres; the windowed fit and the ascent-layer wind must cut the
// scatter strongly. Also: GPS noise, a spiral that is not a whole number of samples, a window of 0 equal to the old method, no track.
#include "dect2/sonde/sonde_land.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include "../common/test_util.h"
using namespace dect2;
namespace {
const double kD = 3.14159265358979 / 180, kR = 6371000.0, kLat0 = 54.0, kLon0 = 25.0;
double windE(double h) { return 6 + 14 * std::min(h, 20000.0) / 20000.0; }   // m/s east, increases with height
double windN(double h) { return 2 + 3 * std::min(h, 20000.0) / 20000.0; }
double fallSpeed(double h) { return 40 * std::exp((h - 20000.0) / 14000.0); }   // slower in the dense air
struct Sim {
    std::vector<SondeTrackPoint> tr;
    double e = 0, n = 0, h = 0;
    std::vector<double> E, N, H;   // full descent for the truth
};
double rnd() { return (std::rand() / (double)RAND_MAX) - 0.5; }
}  // namespace
int main() {
    std::srand(7);
    Sim s;
    double t = 1000;
    // ascent to 20 km at 5 m/s, wind per height plus a slow pendulum of 10 m
    for (s.h = 0; s.h < 20000; s.h += 5) {
        s.e += windE(s.h); s.n += windN(s.h); t += 1;
        SondeTrackPoint p; p.lat = (float)(kLat0 + s.n / kR / kD); p.lon = (float)(kLon0 + s.e / (kR * std::cos(kLat0 * kD)) / kD);
        p.altM = (float)s.h; p.unixT = (uint32_t)t; s.tr.push_back(p);
    }
    // descent with spiral; record centre-line drift for the truth
    const size_t nAsc = s.tr.size();
    double ce = s.e, cn = s.n, h = s.h;   // centre-line position
    struct Frame { double lat, lon, alt, vz, hs, hd; size_t idx; double ceE, ceN; };
    std::vector<Frame> frames;
    std::vector<double> cE, cN, cH; cE.push_back(ce); cN.push_back(cn); cH.push_back(h);
    double pe = ce + 40, pn = cn, ph = h;   // previous true position (with spiral)
    for (int k = 1; h > 0; k++) {
        const double v = fallSpeed(h);
        h -= v; ce += windE(h); cn += windN(h); t += 1;
        const double ph0 = 2 * 3.14159265 * k / 8.3;   // 8.3 s period: not a whole number of samples
        const double xe = ce + 40 * std::cos(ph0), xn = cn + 40 * std::sin(ph0);
        const double ne = xe + 3 * rnd() * 2, nn2 = xn + 3 * rnd() * 2;   // GPS noise +-3 m
        const double ve = xe - pe, vn = xn - pn, vzz = (h - ph) + 2.5 * std::sin(ph0 * 1.3);   // swing also in the climb rate
        SondeTrackPoint p; p.lat = (float)(kLat0 + nn2 / kR / kD); p.lon = (float)(kLon0 + ne / (kR * std::cos(kLat0 * kD)) / kD);
        p.altM = (float)h; p.unixT = (uint32_t)t; s.tr.push_back(p);
        Frame f{kLat0 + nn2 / kR / kD, kLon0 + ne / (kR * std::cos(kLat0 * kD)) / kD, h, vzz, std::hypot(ve, vn), std::atan2(ve, vn) / kD, s.tr.size(), 0, 0};
        if (f.hd < 0) f.hd += 360;
        frames.push_back(f);
        pe = xe; pn = xn; ph = h; cE.push_back(ce); cN.push_back(cn); cH.push_back(h);
    }
    // truth: the centre line lands at (ce, cn) with the swing's mean zero
    const double truthLat = kLat0 + cn / kR / kD, truthLon = kLon0 + ce / (kR * std::cos(kLat0 * kD)) / kD;
    auto errKm = [&](const SondeLanding& l) {
        const double de = (l.lon - truthLon) * kD * kR * std::cos(kLat0 * kD), dn = (l.lat - truthLat) * kD * kR;
        return std::hypot(de, dn) / 1000.0;
    };
    double sumOld = 0, sumNew = 0, sumNoLayer = 0, maxNew = 0; int cnt = 0;
    for (const Frame& f : frames) {
        if (f.alt > 16000 || f.alt < 4000) continue;   // the descent from 16 km down to 4 km
        std::vector<SondeTrackPoint> part(s.tr.begin(), s.tr.begin() + f.idx);
        const SondeLanding o = predictSondeLanding(f.lat, f.lon, f.alt, f.vz, f.hs, f.hd);
        SondeLandOpts opt; opt.windowS = 10;
        const SondeLanding nw = predictSondeLandingTrack(part, f.lat, f.lon, f.alt, f.vz, f.hs, f.hd, opt);
        SondeLandOpts nl = opt; nl.layers = false;
        const SondeLanding nolay = predictSondeLandingTrack(part, f.lat, f.lon, f.alt, f.vz, f.hs, f.hd, nl);
        if (!o.valid || !nw.valid || !nolay.valid) continue;
        const double eo = errKm(o), en = errKm(nw), ec = errKm(nolay);
        sumOld += eo * eo; sumNew += en * en; sumNoLayer += ec * ec; maxNew = std::max(maxNew, en); cnt++;
    }
    CHECK(cnt > 50, "samples: %d", cnt);
    const double rmsOld = std::sqrt(sumOld / cnt), rmsNew = std::sqrt(sumNew / cnt), rmsNoL = std::sqrt(sumNoLayer / cnt);
    printf("rms error to the true landing: instantaneous %.2f km, 10 s fit without layers %.2f km, with ascent layers %.2f km (worst %.2f)\n", rmsOld, rmsNoL, rmsNew, maxNew);
    CHECK(rmsOld > 1.0, "the spiral really disturbs the old method: %.2f km", rmsOld);
    CHECK(rmsNew < 0.3 * rmsOld, "scatter dropped strongly: %.2f vs %.2f km", rmsNew, rmsOld);
    CHECK(rmsNew <= rmsNoL + 0.05, "ascent layers do not hurt in shear: %.2f vs %.2f km", rmsNew, rmsNoL);
    CHECK(maxNew < 4.0, "worst smoothed error %.2f km", maxNew);
    // the window setting: 0 equals the old method, 30 s is no worse than 10 s by much
    {
        const Frame& f = frames[frames.size() / 3];
        std::vector<SondeTrackPoint> part(s.tr.begin(), s.tr.begin() + f.idx);
        SondeLandOpts z; z.windowS = 0;
        const SondeLanding o = predictSondeLanding(f.lat, f.lon, f.alt, f.vz, f.hs, f.hd), z0 = predictSondeLandingTrack(part, f.lat, f.lon, f.alt, f.vz, f.hs, f.hd, z);
        CHECK(o.valid && z0.valid && std::fabs(o.lat - z0.lat) < 1e-9 && std::fabs(o.lon - z0.lon) < 1e-9, "window 0 = old method");
        SondeLandOpts w30; w30.windowS = 30;
        const SondeLanding l30 = predictSondeLandingTrack(part, f.lat, f.lon, f.alt, f.vz, f.hs, f.hd, w30);
        CHECK(l30.valid && errKm(l30) < 0.5 * errKm(o) + 0.3, "30 s window %.2f km vs old %.2f km", errKm(l30), errKm(o));
        SondeLandOpts w99; w99.windowS = 999;   // clamped to 60 s
        CHECK(predictSondeLandingTrack(part, f.lat, f.lon, f.alt, f.vz, f.hs, f.hd, w99).valid, "window clamps");
        const SondeLanding none = predictSondeLandingTrack({}, f.lat, f.lon, f.alt, f.vz, f.hs, f.hd);
        CHECK(none.valid && std::fabs(none.lat - o.lat) < 1e-9, "no track: instantaneous fallback");
    }
    // a burst inside the window (still rising points) must not make a rising sonde predict a landing
    {
        std::vector<SondeTrackPoint> part(s.tr.begin(), s.tr.begin() + nAsc);
        CHECK(!predictSondeLandingTrack(part, part.back().lat, part.back().lon, part.back().altM, 5, 12, 80).valid, "rising: none");
    }
    if (!fails) printf("sonde landing smoothing ok\n");
    return fails ? 1 : 0;
}
