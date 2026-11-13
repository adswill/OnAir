// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/selfcal/ppmcal.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace dect2::selfcal {

static const double kPi = 3.14159265358979323846;

double ppmFromObserved(double expectedHz, double observedHz) {
    if (!(expectedHz > 0) || !(observedHz > 0)) return 0;
    return (expectedHz / observedHz - 1.0) * 1e6;
}

double parabolicPeak(const float* m, size_t n, size_t k) {
    if (!m || k == 0 || k + 1 >= n) return (double)k;
    if (!(m[k - 1] > 0) || !(m[k] > 0) || !(m[k + 1] > 0)) return (double)k;
    const double a = std::log((double)m[k - 1]), b = std::log((double)m[k]), c = std::log((double)m[k + 1]);
    const double den = a - 2 * b + c;
    if (den >= 0) return (double)k;   // not a maximum
    const double d = 0.5 * (a - c) / den;
    return (double)k + std::max(-0.5, std::min(0.5, d));
}

double peakBin(const float* m, size_t n) {
    if (!m || n == 0) return -1;
    size_t k = 0;
    for (size_t i = 1; i < n; i++) if (m[i] > m[k]) k = i;
    return parabolicPeak(m, n, k);
}

static ToneFit fitStage(const std::complex<float>* x, size_t n, double fs, double approxHz, size_t blockLen);

ToneFit fitToneByPhase(const std::complex<float>* x, size_t n, double fs, double approxHz, size_t blockLen) {
    ToneFit r;
    if (!x || !(fs > 0) || n < 64) return r;
    if (blockLen == 0) blockLen = std::max<size_t>(16, (size_t)(fs * 0.02));
    // coarse: lag-1 phase step over everything (captures +-fs/2), then finer and finer block fits, each inside the last one's capture range
    std::complex<double> acc(0, 0);
    for (size_t i = 1; i < n; i++) acc += std::complex<double>(x[i].real(), x[i].imag()) * std::conj(std::complex<double>(x[i - 1].real(), x[i - 1].imag()));
    double f = std::arg(acc) * fs / (2 * kPi);   // absolute tone offset from the baseband centre
    // stage fits work relative to a mixer frequency; start from the coarse estimate (not approxHz, which only sets the answer's reference)
    double mix = f;
    for (size_t b = 16; b < blockLen; b *= 8) { const ToneFit t = fitStage(x, n, fs, mix, b); if (!t.valid) return r; mix += t.offsetHz; }
    r = fitStage(x, n, fs, mix, blockLen);
    if (r.valid) r.offsetHz += mix - approxHz;
    return r;
}

static ToneFit fitStage(const std::complex<float>* x, size_t n, double fs, double approxHz, size_t blockLen) {
    ToneFit r;
    if (!x || !(fs > 0)) return r;
    if (blockLen == 0) blockLen = std::max<size_t>(16, (size_t)(fs * 0.02));
    const size_t nb = n / blockLen;
    if (nb < 4) return r;
    std::vector<double> ph(nb);
    const double w = -2 * kPi * approxHz / fs;
    for (size_t b = 0; b < nb; b++) {
        std::complex<double> s(0, 0);
        for (size_t i = 0; i < blockLen; i++) {
            const size_t j = b * blockLen + i;
            const double a = w * (double)j;
            s += std::complex<double>((double)x[j].real(), (double)x[j].imag()) * std::complex<double>(std::cos(a), std::sin(a));
        }
        ph[b] = std::arg(s);
    }
    for (size_t b = 1; b < nb; b++) {
        double d = ph[b] - ph[b - 1];
        d -= 2 * kPi * std::round(d / (2 * kPi));
        ph[b] = ph[b - 1] + d;
    }
    // least squares line: phase = p0 + 2 pi f t, t the block index times the block time
    const double dt = (double)blockLen / fs;
    double st = 0, sp = 0, stt = 0, stp = 0;
    for (size_t b = 0; b < nb; b++) { const double t = (double)b * dt; st += t; sp += ph[b]; stt += t * t; stp += t * ph[b]; }
    const double N = (double)nb, den = N * stt - st * st;
    if (!(den > 0)) return r;
    const double slope = (N * stp - st * sp) / den, icpt = (sp - slope * st) / N;
    double res = 0;
    for (size_t b = 0; b < nb; b++) { const double e = ph[b] - (icpt + slope * (double)b * dt); res += e * e; }
    const double s2 = std::max(res / std::max(1.0, N - 2), 1e-14);
    r.valid = true;
    r.offsetHz = slope / (2 * kPi);
    r.sigmaHz = std::sqrt(s2 * N / den) / (2 * kPi);
    r.snrDb = 10 * std::log10(1.0 / (2 * s2));   // phase noise variance of one block ~ 1 / (2 SNR)
    return r;
}

void PpmAverager::reset() { n_ = 0; sw_ = swx_ = swxx_ = sw2_ = 0; }
void PpmAverager::add(double ppm, double sigmaPpm) {
    const double s = std::max(sigmaPpm, 1e-6), w = 1.0 / (s * s);
    n_++; sw_ += w; swx_ += w * ppm; swxx_ += w * ppm * ppm; sw2_ += w * w;
}
double PpmAverager::mean() const { return sw_ > 0 ? swx_ / sw_ : 0; }
double PpmAverager::sigma() const {
    if (n_ == 0) return 1e9;
    const double formal = std::sqrt(1.0 / sw_);
    if (n_ < 2) return formal;
    const double m = mean();
    // weighted sample variance (reliability weights) and the effective count -> standard error of the mean
    const double denom = sw_ - sw2_ / sw_;
    if (!(denom > 0)) return formal;
    const double v = std::max(0.0, swxx_ - 2 * m * swx_ + m * m * sw_) / denom;
    const double neff = sw_ * sw_ / sw2_;
    return std::max(formal, std::sqrt(v / std::max(1.0, neff)));
}

Sanity checkPpm(double ppm, Tcxo tcxo, bool subCarrier, double ci95) {
    Sanity s;
    const double a = std::fabs(ppm);
    char b[200];
    if (tcxo == Tcxo::Yes && a > 50) {
        s.accepted = false;
        snprintf(b, sizeof b, "%.1f ppm is too much for a radio with a TCXO (a few ppm at most): the wrong channel or reference?", ppm);
        s.notes.push_back(b);
    } else if (tcxo != Tcxo::Unknown && a > 300) {
        s.accepted = false;
        snprintf(b, sizeof b, "%.0f ppm is beyond any crystal: the wrong channel or reference?", ppm);
        s.notes.push_back(b);
    } else if (tcxo == Tcxo::Unknown && a > 300) {
        snprintf(b, sizeof b, "%.0f ppm is very large and it is not known whether this radio has a TCXO: check the reference before applying it.", ppm);
        s.notes.push_back(b);
    }
    if (tcxo == Tcxo::No && a > 100 && a <= 300) s.notes.push_back("Over 100 ppm is high even for a plain crystal: check the reference.");
    if (subCarrier) s.notes.push_back("This signal is a sub-carrier (a pilot or an offset tone): the station's own error adds to the result. Repeat on two or three stations.");
    if (ci95 > 0.5) s.notes.push_back("The measurement is still wide: let it run longer.");
    return s;
}

QuickResult quickCheck(double residual, double sigma) {
    QuickResult q;
    q.deltaPpm = residual;
    const double lim = std::max(0.3, 3 * sigma), a = std::fabs(residual);
    char b[200];
    if (a <= lim) { q.verdict = QuickVerdict::Good; snprintf(b, sizeof b, "The correction still holds: %+.2f ppm left (+-%.2f).", residual, sigma); }
    else if (a <= 2.0) { q.verdict = QuickVerdict::Warm; snprintf(b, sizeof b, "%+.2f ppm left: the radio has drifted (warm-up?). Re-measure after it has been on for 10 minutes.", residual); }
    else { q.verdict = QuickVerdict::Off; snprintf(b, sizeof b, "%+.2f ppm left: the correction is off, run the full calibration.", residual); }
    q.text = b;
    return q;
}

void DriftTrack::add(double t, double ppm, double sigma) {
    if (!t_.empty() && t <= t_.back()) return;
    t_.push_back(t); p_.push_back(ppm); s_.push_back(std::max(sigma, 1e-6));
}
bool DriftTrack::slope(double& out) const {
    if (t_.size() < 3) return false;
    double sw = 0, st = 0, sp = 0, stt = 0, stp = 0;
    for (size_t i = 0; i < t_.size(); i++) {
        const double w = 1.0 / (s_[i] * s_[i]), t = (t_[i] - t_.front()) / 60.0;
        sw += w; st += w * t; sp += w * p_[i]; stt += w * t * t; stp += w * t * p_[i];
    }
    const double den = sw * stt - st * st;
    if (!(den > 0)) return false;
    out = (sw * stp - st * sp) / den;
    return true;
}
std::string DriftTrack::note() const {
    double m;
    if (!slope(m)) return "Not enough points yet.";
    char b[200];
    if (std::fabs(m) < 0.02) snprintf(b, sizeof b, "Steady (%+.3f ppm per minute): the radio has settled.", m);
    else snprintf(b, sizeof b, "Drifting %+.3f ppm per minute: the radio is still warming up. Measure again when this is near zero.", m);
    return b;
}

// ---- references
static RefChannel mk(const char* g, const std::string& l, double hz, RefKind k, bool sub = false, const char* note = "") {
    RefChannel r; r.group = g; r.label = l; r.nominalHz = hz; r.kind = k; r.subCarrier = sub; r.note = note; return r;
}
std::vector<RefChannel> fmChannels() {
    std::vector<RefChannel> v;
    for (int k = 875; k <= 1080; k++) {
        char b[32]; snprintf(b, sizeof b, "FM %.1f MHz", k / 10.0);
        v.push_back(mk("FM", b, k * 1e5, RefKind::FmPilot, true, "19 kHz stereo pilot of the station"));
    }
    return v;
}
std::vector<RefChannel> atscChannels() {
    std::vector<RefChannel> v;
    for (int ch = 2; ch <= 36; ch++) {
        double lo;
        if (ch <= 4) lo = 54e6 + (ch - 2) * 6e6;
        else if (ch == 5) lo = 76e6;
        else if (ch == 6) lo = 82e6;
        else if (ch <= 13) lo = 174e6 + (ch - 7) * 6e6;
        else lo = 470e6 + (ch - 14) * 6e6;
        char b[48]; snprintf(b, sizeof b, "ATSC ch %d pilot", ch);
        v.push_back(mk("ATSC", b, lo + 309440.559, RefKind::Carrier, false, "pilot 309.44 kHz above the lower channel edge"));
    }
    return v;
}
std::vector<RefChannel> dvbtChannels() {
    std::vector<RefChannel> v;
    for (int ch = 5; ch <= 12; ch++) { char b[40]; snprintf(b, sizeof b, "DVB-T VHF ch %d (7 MHz)", ch); v.push_back(mk("DVB-T", b, 177.5e6 + (ch - 5) * 7e6, RefKind::DecoderOffset)); }
    for (int ch = 21; ch <= 60; ch++) { char b[40]; snprintf(b, sizeof b, "DVB-T UHF ch %d (8 MHz)", ch); v.push_back(mk("DVB-T", b, 474e6 + (ch - 21) * 8e6, RefKind::DecoderOffset)); }
    return v;
}
std::vector<RefChannel> dabBlocks() {
    static const double a[] = {174.928, 181.936, 188.928, 195.936, 202.928, 209.936, 216.928, 223.936};   // 5A .. 12A (MHz)
    static const char* L = "ABCD";
    std::vector<RefChannel> v;
    for (int i = 0; i < 8; i++)
        for (int l = 0; l < 4; l++) {
            char b[16]; snprintf(b, sizeof b, "DAB %d%c", 5 + i, L[l]);
            v.push_back(mk("DAB", b, (a[i] + l * 1.712) * 1e6, RefKind::DecoderOffset));
        }
    static const double f13[] = {230.784, 232.496, 234.208, 235.776, 237.488, 239.200};
    for (int l = 0; l < 6; l++) { char b[16]; snprintf(b, sizeof b, "DAB 13%c", 'A' + l); v.push_back(mk("DAB", b, f13[l] * 1e6, RefKind::DecoderOffset)); }
    return v;
}
std::vector<RefChannel> gsmChannels() {
    const double fcch = 1625000.0 / 24.0;   // 67.708333 kHz above the carrier
    std::vector<RefChannel> v;
    for (int n = 1; n <= 124; n++) { char b[40]; snprintf(b, sizeof b, "GSM 900 ARFCN %d FCCH", n); v.push_back(mk("GSM", b, 935e6 + 0.2e6 * n + fcch, RefKind::GsmFcch, false, "FCCH burst tone of the base station")); }
    for (int n = 512; n <= 885; n++) { char b[40]; snprintf(b, sizeof b, "GSM 1800 ARFCN %d FCCH", n); v.push_back(mk("GSM", b, 1805.2e6 + 0.2e6 * (n - 512) + fcch, RefKind::GsmFcch, false, "FCCH burst tone of the base station")); }
    return v;
}
std::vector<RefChannel> timeCarriers() {
    std::vector<RefChannel> v;
    for (double f : {2.5, 5.0, 10.0, 15.0, 20.0}) { char b[40]; snprintf(b, sizeof b, "WWV/WWVH %.1f MHz", f); v.push_back(mk("Time", b, f * 1e6, RefKind::Carrier)); }
    for (double f : {3.33, 7.85, 14.67}) { char b[40]; snprintf(b, sizeof b, "CHU %.2f MHz", f); v.push_back(mk("Time", b, f * 1e6, RefKind::Carrier)); }
    v.push_back(mk("Time", "BPM 10 MHz", 10e6, RefKind::Carrier));
    v.push_back(mk("Time", "DCF77 77.5 kHz", 77500, RefKind::Carrier));
    v.push_back(mk("Time", "MSF 60 kHz", 60000, RefKind::Carrier));
    v.push_back(mk("Time", "WWVB 60 kHz", 60000, RefKind::Carrier));
    v.push_back(mk("Time", "JJY 40 kHz", 40000, RefKind::Carrier));
    v.push_back(mk("Time", "JJY 60 kHz", 60000, RefKind::Carrier));
    v.push_back(mk("Time", "BPC 68.5 kHz", 68500, RefKind::Carrier));
    v.push_back(mk("Time", "RBU 66.67 kHz", 200000.0 / 3.0, RefKind::Carrier));
    return v;
}
RefChannel amCarrier(double hz) {
    char b[48]; snprintf(b, sizeof b, "AM carrier %.3f kHz", hz / 1e3);
    return mk("AM", b, hz, RefKind::Carrier, false, "a broadcast carrier of known frequency");
}
std::vector<RefChannel> allReferences() {
    std::vector<RefChannel> v;
    for (const auto& g : {fmChannels(), atscChannels(), dvbtChannels(), dabBlocks(), gsmChannels(), timeCarriers()}) v.insert(v.end(), g.begin(), g.end());
    return v;
}

}   // namespace dect2::selfcal
