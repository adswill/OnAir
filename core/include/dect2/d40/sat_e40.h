// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// 4.0 satellite and space helpers, group E (pure, no GUI). Tests: tests/d40/test_sat_e40.cpp
//  1. stitchLag        where does the next pass image start inside the previous one: normalised cross-correlation of the row-mean profiles of the overlapping rows.
//  2. passLinkBudget   slant range, free-space loss, C/N0 and SNR at an elevation, so a pass can be judged (and the minimum decodable elevation found) before it happens.
//  3. decoderHint      satellite name (any spelling) -> decoder, usual downlink frequency and baud. A hint only: the operator confirms.
//  4. ObsLog           SatNOGS-style observation log: good / bad / failed verdict per pass, CSV export, success rate and lowest elevation that ever decoded per satellite.
//  5. sunlit           cylindrical Earth-shadow test with a low-precision Sun position: visible-light products and solar-powered beacons.
//  6. planRetunes      Doppler curve -> the shortest list of retunes that keeps the residual offset inside a fraction of the channel width.
#pragma once
#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace dect2::d40 {

// 1. a, b: row-mean profiles (a = the earlier image's last rows, b = the new image's first rows). Returns the lag = index in a where b[0] sits (a negative score means no match).
struct StitchResult { int lag = -1; double score = -1; bool ok = false; };
inline StitchResult stitchLag(const std::vector<float>& a, const std::vector<float>& b, int minOverlap = 16, double minScore = 0.6) {
    StitchResult r; const int na = (int)a.size(), nb = (int)b.size();
    for (int lag = 0; lag + minOverlap <= na; lag++) {
        const int n = std::min(nb, na - lag); if (n < minOverlap) break;
        double ma = 0, mb = 0; for (int i = 0; i < n; i++) { ma += a[(size_t)(lag + i)]; mb += b[(size_t)i]; } ma /= n; mb /= n;
        double sab = 0, saa = 0, sbb = 0;
        for (int i = 0; i < n; i++) { const double x = a[(size_t)(lag + i)] - ma, y = b[(size_t)i] - mb; sab += x * y; saa += x * x; sbb += y * y; }
        const double c = saa > 0 && sbb > 0 ? sab / std::sqrt(saa * sbb) : 0;
        if (c > r.score) { r.score = c; r.lag = lag; }
    }
    r.ok = r.score >= minScore; return r;
}

// 2. circular orbit over a spherical Earth
struct LinkBudget { double rangeKm = 0, fsplDb = 0, cn0DbHz = 0, snrDb = 0; };
inline LinkBudget passLinkBudget(double freqHz, double eirpDbw, double rxGainDbi, double systemTempK, double bwHz, double altKm, double elDeg) {
    const double Re = 6371.0, e = elDeg * M_PI / 180.0, r = Re + altKm;
    LinkBudget o; o.rangeKm = std::sqrt(r * r - Re * Re * std::cos(e) * std::cos(e)) - Re * std::sin(e);
    o.fsplDb = 20 * std::log10(o.rangeKm * 1e3) + 20 * std::log10(freqHz) - 147.5522;
    o.cn0DbHz = eirpDbw + rxGainDbi - o.fsplDb - 10 * std::log10(1.380649e-23 * systemTempK);
    o.snrDb = o.cn0DbHz - 10 * std::log10(bwHz); return o;
}
// lowest elevation (degrees) at which the SNR reaches reqDb, or -1 if even the zenith is too weak, 0 if the horizon is enough
inline double minElevationFor(double reqDb, double freqHz, double eirpDbw, double rxGainDbi, double tK, double bwHz, double altKm) {
    if (passLinkBudget(freqHz, eirpDbw, rxGainDbi, tK, bwHz, altKm, 90).snrDb < reqDb) return -1;
    if (passLinkBudget(freqHz, eirpDbw, rxGainDbi, tK, bwHz, altKm, 0).snrDb >= reqDb) return 0;
    double lo = 0, hi = 90; for (int i = 0; i < 40; i++) { const double m = 0.5 * (lo + hi); (passLinkBudget(freqHz, eirpDbw, rxGainDbi, tK, bwHz, altKm, m).snrDb >= reqDb ? hi : lo) = m; }
    return hi;
}

// 3.
struct DecoderHint { std::string decoder; double freqHz = 0; double baud = 0; std::string note; };
inline std::string normSatName(const std::string& s) { std::string o; for (char c : s) if (std::isalnum((unsigned char)c)) o += (char)std::toupper((unsigned char)c); return o; }
inline bool decoderHint(const std::string& name, DecoderHint& out) {
    struct E { const char* key; DecoderHint h; };
    static const E tab[] = {
        {"NOAA15", {"APT", 137.620e6, 4160, "2 lines/s, 2080 Hz subcarrier"}},
        {"NOAA18", {"APT", 137.9125e6, 4160, "2 lines/s, 2080 Hz subcarrier"}},
        {"NOAA19", {"APT", 137.100e6, 4160, "2 lines/s, 2080 Hz subcarrier"}},
        {"METEORM23", {"LRPT", 137.900e6, 72000, "QPSK 72 kbit/s, 144 ksym/s"}},
        {"METEORM24", {"LRPT", 137.900e6, 72000, "QPSK 72 kbit/s, 144 ksym/s"}},
        {"ISSZARYA", {"AFSK1200 AX.25 (APRS)", 145.825e6, 1200, "also SSTV on 145.800 when active"}},
        {"ISS", {"AFSK1200 AX.25 (APRS)", 145.825e6, 1200, "also SSTV on 145.800 when active"}},
        {"AO73", {"BPSK telemetry (FUNcube)", 145.935e6, 1200, "beacon; transponder 435.150-435.130 up"}},
        {"FUNCUBE1", {"BPSK telemetry (FUNcube)", 145.935e6, 1200, "same as AO-73"}},
        {"METOPB", {"AHRPT", 1701.3e6, 0, "QPSK 3.5 Msym/s"}},
        {"METOPC", {"AHRPT", 1701.3e6, 0, "QPSK 3.5 Msym/s"}},
        {"GOES16", {"HRIT", 1694.1e6, 927000, "BPSK 927 ksym/s"}},
        {"GOES18", {"HRIT", 1694.1e6, 927000, "BPSK 927 ksym/s"}},
    };
    const std::string k = normSatName(name);
    for (const E& e : tab) if (k == e.key) { out = e.h; return true; }
    for (const E& e : tab) if (k.size() >= 5 && (k.find(e.key) != std::string::npos)) { out = e.h; return true; }
    return false;
}

// 4.
struct Obs { std::string sat; double aos = 0, los = 0, maxElDeg = 0, minElDecodedDeg = -1; int frames = 0; double peakSnrDb = -99; };
enum class ObsVerdict { Good, Bad, Failed };
class ObsLog {
public:
    static ObsVerdict verdict(const Obs& o, double signalSnrDb = 6.0) { return o.frames > 0 ? ObsVerdict::Good : (o.peakSnrDb >= signalSnrDb ? ObsVerdict::Bad : ObsVerdict::Failed); }
    void add(const Obs& o) { v_.push_back(o); }
    size_t size() const { return v_.size(); }
    double successRate(const std::string& sat) const { int n = 0, g = 0; for (auto& o : v_) if (o.sat == sat) { n++; g += verdict(o) == ObsVerdict::Good; } return n ? (double)g / n : 0; }
    double lowestDecodedElevation(const std::string& sat) const { double m = 91; for (auto& o : v_) if (o.sat == sat && o.frames > 0 && o.minElDecodedDeg >= 0) m = std::min(m, o.minElDecodedDeg); return m > 90 ? -1 : m; }
    std::string csv() const {
        std::ostringstream s; s << "sat,aos,los,max_el_deg,frames,peak_snr_db,verdict\n";
        for (auto& o : v_) s << o.sat << ',' << (long long)o.aos << ',' << (long long)o.los << ',' << o.maxElDeg << ',' << o.frames << ',' << o.peakSnrDb << ',' << (verdict(o) == ObsVerdict::Good ? "good" : verdict(o) == ObsVerdict::Bad ? "bad" : "failed") << '\n';
        return s.str();
    }
private: std::vector<Obs> v_;
};

// 5. sun: low-precision (about 0.01 deg) unit vector in an Earth-centred inertial frame for a Julian date; sat: position in km in the same frame
inline void sunUnit(double jd, double v[3]) {
    const double n = jd - 2451545.0, L = std::fmod(280.460 + 0.9856474 * n, 360.0), g = std::fmod(357.528 + 0.9856003 * n, 360.0) * M_PI / 180.0;
    const double lam = (L + 1.915 * std::sin(g) + 0.020 * std::sin(2 * g)) * M_PI / 180.0, eps = (23.439 - 0.0000004 * n) * M_PI / 180.0;
    v[0] = std::cos(lam); v[1] = std::cos(eps) * std::sin(lam); v[2] = std::sin(eps) * std::sin(lam);
}
inline bool sunlit(const double sat[3], const double sun[3]) {
    const double d = sat[0] * sun[0] + sat[1] * sun[1] + sat[2] * sun[2];
    if (d >= 0) return true;
    const double r2 = sat[0] * sat[0] + sat[1] * sat[1] + sat[2] * sat[2];
    return r2 - d * d > 6378.137 * 6378.137;      // outside the cylinder of the shadow
}

// 6. samples: (time s, Doppler Hz) ascending in time. Retune whenever the held offset is off by more than frac * bwHz / 2.
struct Retune { double t = 0, offsetHz = 0; };
inline std::vector<Retune> planRetunes(const std::vector<std::pair<double, double>>& dop, double bwHz, double frac = 0.25) {
    std::vector<Retune> out; if (dop.empty()) return out;
    const double lim = frac * bwHz / 2; double held = dop[0].second; out.push_back({dop[0].first, held});
    for (size_t i = 1; i < dop.size(); i++) if (std::fabs(dop[i].second - held) > lim) { held = dop[i].second; out.push_back({dop[i].first, held}); }
    return out;
}

}  // namespace dect2::d40
