// Landing prediction for a falling radiosonde: integrates the descent from the last position, vertical speed and horizontal drift.
// The parachute falls faster in thin air: speed(h) = speed(now) * sqrt(rho(now) / rho(h)), with an exponential atmosphere (scale height
// 7 km). The horizontal speed is kept at the last measured value, so the answer is a guide, good to a few km from 10 km up.
#pragma once
#include <algorithm>
#include <cmath>
#include <vector>
#include "dect2/common/mathconst.h"
#include "dect2/sonde/sonde_tel.h"

namespace dect2 {

struct SondeLanding {
    bool valid = false;
    double lat = 0, lon = 0;   // where it should come down
    double secondsLeft = 0;    // until then
};

inline SondeLanding predictSondeLanding(double lat, double lon, double altM, double vSpeedMs, double hSpeedMs, double headingDeg, double groundM = 0) {
    SondeLanding r;
    if (!(vSpeedMs < -1.0) || altM <= groundM + 10) return r;
    const double kPi = dect2::math::kPi, kR = 6371000.0, kScale = 7000.0;
    const double vE = hSpeedMs * std::sin(headingDeg * kPi / 180.0), vN = hSpeedMs * std::cos(headingDeg * kPi / 180.0);
    double h = altM, t = 0, la = lat, lo = lon;
    const double v0 = -vSpeedMs, h0 = altM;
    while (h > groundM && t < 36000) {
        const double v = v0 * std::exp((h - h0) / (2 * kScale));   // slower in the denser air below
        const double dt = 1.0;
        h -= v * dt; t += dt;
        la += vN * dt / kR * 180.0 / kPi;
        lo += vE * dt / (kR * std::cos(la * kPi / 180.0)) * 180.0 / kPi;
    }
    r.valid = true; r.lat = la; r.lon = lo; r.secondsLeft = t;
    return r;
}

// ---- Smoothed prediction (3.5) --------------------------------------------------------------------------------------------------
// Under a parachute a sonde swings and spirals, so the instantaneous ground speed and heading jump by more than the wind itself and the
// landing point jumps with them. Here the drift is the least squares slope of position against time over the last `windowS` seconds of
// the track (a spiral averages out of a position slope), the vertical speed likewise, and the wind of the layers still below is taken
// from the way up (the ascent track passed through every one of them), blended with the current drift. windowS <= 0 (or too few track
// points) gives the old instantaneous prediction.
struct SondeLandOpts {
    double windowS = 10;     // smoothing window, 0 .. 60 s
    bool layers = true;      // use the ascent wind of the layers below
    double layerM = 500;     // layer thickness
    double ascentWeight = 0.6;   // weight of the ascent wind against the current drift in a layer below
};

namespace sondeland_detail {
struct Fit { bool ok = false; double ve = 0, vn = 0, vz = 0; };
// Least squares slope of east / north / altitude against time over points [i0, i1) of the track, in metres around (refLat, refLon).
inline Fit slope(const std::vector<SondeTrackPoint>& tr, size_t i0, size_t i1, double refLat, double refLon, double minSpanS, bool hann = false) {
    Fit f;
    if (i1 <= i0 + 2) return f;
    const double kR = 6371000.0, kD = dect2::math::kPi / 180.0, cl = std::cos(refLat * kD);
    // optional Hann taper (off: it lowers the leverage of the window ends and made the slope worse in tests)
    const double t0 = tr[i0].unixT, t1 = tr[i1 - 1].unixT, len = std::max(1.0, t1 - t0);
    auto wt = [&](size_t i) { const double x = (tr[i].unixT - t0) / len; return hann ? std::sin(kD * 180.0 * x) * std::sin(kD * 180.0 * x) + 1e-3 : 1.0; };
    double sw = 0, st = 0;
    for (size_t i = i0; i < i1; i++) { sw += wt(i); st += wt(i) * tr[i].unixT; }
    const double tm = st / sw;
    double stt = 0, ste = 0, stn = 0, stz = 0;
    for (size_t i = i0; i < i1; i++) {
        const double w = wt(i), t = tr[i].unixT - tm, e = (tr[i].lon - refLon) * kD * kR * cl, nn = (tr[i].lat - refLat) * kD * kR;
        stt += w * t * t; ste += w * t * e; stn += w * t * nn; stz += w * t * tr[i].altM;
    }
    const double span = (double)tr[i1 - 1].unixT - (double)tr[i0].unixT;
    if (stt <= 0 || span < minSpanS) return f;
    f.ve = ste / stt; f.vn = stn / stt; f.vz = stz / stt; f.ok = true;
    return f;
}
}  // namespace sondeland_detail

inline SondeLanding predictSondeLandingTrack(const std::vector<SondeTrackPoint>& tr, double lat, double lon, double altM, double vSpeedMs,
                                            double hSpeedMs, double headingDeg, const SondeLandOpts& o = SondeLandOpts(), double groundM = 0) {
    using namespace sondeland_detail;
    const double kPi = dect2::math::kPi, kR = 6371000.0, kScale = 7000.0;
    double vE = hSpeedMs * std::sin(headingDeg * kPi / 180.0), vN = hSpeedMs * std::cos(headingDeg * kPi / 180.0), vz = vSpeedMs;
    const double win = std::min(60.0, std::max(0.0, o.windowS));
    std::vector<Fit> layerFit;   // wind of each layer from the ascent, indexed by floor((alt - ground) / layerM)
    int curLayer = 0;
    if (win > 0 && tr.size() >= 4 && tr.back().unixT > 0) {
        const double tEnd = tr.back().unixT;
        size_t i0 = tr.size();
        while (i0 > 0 && tr[i0 - 1].unixT > 0 && tEnd - tr[i0 - 1].unixT <= win) i0--;
        const Fit w = slope(tr, i0, tr.size(), tr.back().lat, tr.back().lon, win * 0.5);
        if (w.ok && w.vz < -1.0) { vE = w.ve; vN = w.vn; vz = w.vz; }   // only while falling through the whole window (no burst in it)
        if (o.layers && o.layerM > 0 && vz < -1.0) {
            size_t top = 0;   // the burst: highest point; layers are taken from the ascent before it
            for (size_t i = 0; i < tr.size(); i++) if (tr[i].altM > tr[top].altM) top = i;
            curLayer = (int)std::floor((altM - groundM) / o.layerM);
            layerFit.assign(std::max(curLayer, 0), Fit());
            for (int L = 0; L < curLayer; L++) {
                const double lo = groundM + L * o.layerM, hi = lo + o.layerM;
                size_t a = tr.size(), b = 0;
                for (size_t i = 0; i <= top && i < tr.size(); i++) if (tr[i].altM >= lo && tr[i].altM < hi) { a = std::min(a, i); b = i + 1; }
                if (a < b) layerFit[L] = slope(tr, a, b, tr[a].lat, tr[a].lon, 20.0);
            }
        }
    }
    SondeLanding r;
    if (!(vz < -1.0) || altM <= groundM + 10) return r;
    double h = altM, t = 0, la = lat, lo = lon;
    const double v0 = -vz, h0 = altM;
    while (h > groundM && t < 36000) {
        const double v = v0 * std::exp((h - h0) / (2 * kScale));
        double we = vE, wn = vN;
        const int L = o.layerM > 0 ? (int)std::floor((h - groundM) / o.layerM) : -1;
        if (L >= 0 && L < (int)layerFit.size() && layerFit[L].ok) {
            we = o.ascentWeight * layerFit[L].ve + (1 - o.ascentWeight) * vE;
            wn = o.ascentWeight * layerFit[L].vn + (1 - o.ascentWeight) * vN;
        }
        h -= v; t += 1;
        la += wn / kR * 180.0 / kPi;
        lo += we / (kR * std::cos(la * kPi / 180.0)) * 180.0 / kPi;
    }
    r.valid = true; r.lat = la; r.lon = lo; r.secondsLeft = t;
    return r;
}

}  // namespace dect2
