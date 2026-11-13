// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Passive coherent location chain; see passive.h and docs/passive-radar.md.
#include "dect2/radar/passive.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace dect2::radar {

namespace {

constexpr double kPi = 3.14159265358979323846;

double power(const std::vector<cd>& x) {
    double s = 0;
    for (const cd& v : x) s += v.real() * v.real() + v.imag() * v.imag();
    return x.empty() ? 0 : s / x.size();
}

// complex Gaussian elimination with partial pivoting; false when singular
bool solve(std::vector<std::vector<cd>>& a, std::vector<cd>& b) {
    const int n = (int)b.size();
    for (int c = 0; c < n; c++) {
        int p = c;
        for (int r = c + 1; r < n; r++) if (std::abs(a[r][c]) > std::abs(a[p][c])) p = r;
        if (std::abs(a[p][c]) < 1e-300) return false;
        std::swap(a[p], a[c]); std::swap(b[p], b[c]);
        for (int r = c + 1; r < n; r++) {
            const cd f = a[r][c] / a[c][c];
            for (int k = c; k < n; k++) a[r][k] -= f * a[c][k];
            b[r] -= f * b[c];
        }
    }
    for (int r = n - 1; r >= 0; r--) {
        cd s = b[r];
        for (int k = r + 1; k < n; k++) s -= a[r][k] * b[k];
        b[r] = s / a[r][r];
    }
    return true;
}

// in-place radix-2 FFT (forward, exp(-j...)) on split arrays
void fft(std::vector<double>& re, std::vector<double>& im) {
    const size_t n = re.size();
    for (size_t i = 1, j = 0; i < n; i++) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) { std::swap(re[i], re[j]); std::swap(im[i], im[j]); }
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        const double ang = -2 * kPi / (double)len;
        const double wr = std::cos(ang), wi = std::sin(ang);
        for (size_t i = 0; i < n; i += len) {
            double cr = 1, ci = 0;
            for (size_t k = 0; k < len / 2; k++) {
                const size_t a = i + k, b = i + k + len / 2;
                const double tr = re[b] * cr - im[b] * ci, ti = re[b] * ci + im[b] * cr;
                re[b] = re[a] - tr; im[b] = im[a] - ti;
                re[a] += tr; im[a] += ti;
                const double nr = cr * wr - ci * wi; ci = cr * wi + ci * wr; cr = nr;
            }
        }
    }
}

double dbOf(double x) { return 10 * std::log10(std::max(x, 1e-300)); }

}   // namespace

// ---------------------------------------------------------------- cancellation
double cancelEca(const std::vector<cd>& ref, std::vector<cd>& surv, int taps) {
    const size_t n = std::min(ref.size(), surv.size());
    if (taps < 1 || n < (size_t)taps * 4) return 0;
    const double before = power(surv);
    std::vector<cd> c(taps);   // c[l] = sum ref[t] conj(ref[t-l])
    for (int l = 0; l < taps; l++) { cd s = 0; for (size_t t = l; t < n; t++) s += ref[t] * std::conj(ref[t - l]); c[l] = s; }
    std::vector<std::vector<cd>> a(taps, std::vector<cd>(taps));
    std::vector<cd> b(taps);
    for (int i = 0; i < taps; i++) {
        for (int j = 0; j < taps; j++) a[i][j] = i >= j ? c[i - j] : std::conj(c[j - i]);
        a[i][i] *= 1.0 + 1e-9;
        cd s = 0; for (size_t t = i; t < n; t++) s += surv[t] * std::conj(ref[t - i]);
        b[i] = s;
    }
    if (!solve(a, b)) return 0;
    for (size_t t = 0; t < n; t++) {
        cd y = 0;
        for (int i = 0; i < taps && (size_t)i <= t; i++) y += b[i] * ref[t - i];
        surv[t] -= y;
    }
    return dbOf(before) - dbOf(power(surv));
}

double cancelNlms(const std::vector<cd>& ref, std::vector<cd>& surv, int taps, double mu, int passes) {
    const size_t n = std::min(ref.size(), surv.size());
    if (taps < 1 || n < (size_t)taps * 4) return 0;
    const double before = power(surv);
    std::vector<cd> w(taps, cd(0));
    std::vector<cd> out(surv);
    for (int pass = 0; pass < std::max(1, passes); pass++) {
        for (size_t t = 0; t < n; t++) {
            cd y = 0; double e2 = 1e-12;
            for (int i = 0; i < taps; i++) {
                const cd x = t >= (size_t)i ? ref[t - i] : cd(0);
                y += w[i] * x; e2 += std::norm(x);
            }
            const cd e = surv[t] - y;
            out[t] = e;
            const cd g = mu * e / e2;
            for (int i = 0; i < taps; i++) if (t >= (size_t)i) w[i] += g * std::conj(ref[t - i]);
        }
    }
    surv = out;
    return dbOf(before) - dbOf(power(surv));
}

// ---------------------------------------------------------------- cross-ambiguity
Caf crossAmbiguity(const std::vector<cd>& ref, const std::vector<cd>& resid, const CafParams& p) {
    Caf c;
    c.p = p; c.nr = p.rangeBins; c.nd = p.dopplerBins;
    const size_t N = p.cpiSamples();
    c.pwr.assign((size_t)c.nr * c.nd, 0.f);
    if (ref.size() < N || resid.size() < N) return c;
    std::vector<double> rr(N), ri(N), sr(N), si(N);
    for (size_t t = 0; t < N; t++) { rr[t] = ref[t].real(); ri[t] = ref[t].imag(); sr[t] = resid[t].real(); si[t] = resid[t].imag(); }
    std::vector<double> win(c.nd);
    for (int m = 0; m < c.nd; m++) win[m] = 0.5 - 0.5 * std::cos(2 * kPi * (m + 0.5) / c.nd);
    std::vector<double> xr(c.nd), xi(c.nd);
    for (int d = 0; d < c.nr; d++) {
        for (int m = 0; m < c.nd; m++) {
            double ar = 0, ai = 0;
            const size_t t0 = (size_t)m * p.decim, t1 = t0 + p.decim;
            for (size_t t = std::max(t0, (size_t)d); t < t1; t++) {
                const double ur = rr[t - d], ui = ri[t - d];   // surv * conj(ref delayed)
                ar += sr[t] * ur + si[t] * ui;
                ai += si[t] * ur - sr[t] * ui;
            }
            xr[m] = ar * win[m]; xi[m] = ai * win[m];
        }
        fft(xr, xi);
        for (int k = 0; k < c.nd; k++) {
            const int s = (k + c.nd / 2) % c.nd;   // fft-shift: bin k -> row (k + nd/2) mod nd
            c.pwr[(size_t)s * c.nr + d] = (float)(xr[k] * xr[k] + xi[k] * xi[k]);
        }
    }
    return c;
}

// ---------------------------------------------------------------- CFAR
std::vector<Detection> cfarDetect(const Caf& c, const CfarParams& cp) {
    std::vector<Detection> out;
    if (c.nr == 0 || c.nd == 0) return out;
    const int w = cp.guard + cp.train;
    for (int d = 0; d < c.nd; d++) {
        const double fd = c.dopplerHz(d);
        if (std::fabs(fd) < cp.zeroDopplerHz) continue;
        for (int r = std::max(cp.minRangeBin, 0); r < c.nr; r++) {
            const float v = c.at(d, r);
            double sum = 0; int cnt = 0;
            for (int dd = -w; dd <= w; dd++) {
                const int dr = ((d + dd) % c.nd + c.nd) % c.nd;
                for (int rr = -w; rr <= w; rr++) {
                    if (std::abs(dd) <= cp.guard && std::abs(rr) <= cp.guard) continue;
                    const int r2 = r + rr;
                    if (r2 < cp.minRangeBin || r2 >= c.nr) continue;
                    sum += c.at(dr, r2); cnt++;
                }
            }
            if (cnt < 8) continue;
            const double alpha = cnt * (std::pow(cp.pfa, -1.0 / cnt) - 1.0);
            const double noise = sum / cnt;
            if (v <= alpha * noise) continue;
            bool peak = true;   // local maximum in 3 x 3
            for (int dd = -1; dd <= 1 && peak; dd++) for (int rr = -1; rr <= 1; rr++) {
                if (!dd && !rr) continue;
                const int r2 = r + rr; if (r2 < 0 || r2 >= c.nr) continue;
                if (c.at(((d + dd) % c.nd + c.nd) % c.nd, r2) > v) { peak = false; break; }
            }
            if (!peak) continue;
            // parabolic refinement on the log power
            auto lg = [&](int dd, int rr) { return std::log(std::max((double)c.at(((d + dd) % c.nd + c.nd) % c.nd, std::clamp(r + rr, 0, c.nr - 1)), 1e-30)); };
            auto refine = [](double a, double b, double cc) { const double den = a - 2 * b + cc; return std::fabs(den) < 1e-12 ? 0.0 : std::clamp(0.5 * (a - cc) / den, -0.5, 0.5); };
            Detection det;
            det.rBin = r; det.dBin = d;
            det.rangeM = c.rangeM(r + (r > 0 && r < c.nr - 1 ? refine(lg(0, -1), lg(0, 0), lg(0, 1)) : 0.0));
            det.dopplerHz = c.dopplerHz(d + refine(lg(-1, 0), lg(0, 0), lg(1, 0)));
            det.snrDb = dbOf(v / noise);
            out.push_back(det);
        }
    }
    std::sort(out.begin(), out.end(), [](const Detection& a, const Detection& b) { return a.snrDb > b.snrDb; });
    return out;
}

// ---------------------------------------------------------------- tracker
void Tracker::update(const std::vector<Detection>& dets, double dt) {
    t_ += dt;
    const double q = tp_.accelSigma * tp_.accelSigma;
    for (Track& k : tr_) {   // predict
        k.r += k.rdot * dt;
        const double p00 = k.p00 + 2 * dt * k.p01 + dt * dt * k.p11 + q * dt * dt * dt * dt / 4;
        const double p01 = k.p01 + dt * k.p11 + q * dt * dt * dt / 2;
        const double p11 = k.p11 + q * dt * dt;
        k.p00 = p00; k.p01 = p01; k.p11 = p11;
    }
    const double rR = tp_.rangeSigma * tp_.rangeSigma, rV = tp_.rateSigma * tp_.rateSigma;
    struct Pair { double d2; size_t t, m; };
    std::vector<Pair> pairs;
    for (size_t i = 0; i < tr_.size(); i++) for (size_t j = 0; j < dets.size(); j++) {
        const Track& k = tr_[i];
        const double y0 = dets[j].rangeM - k.r, y1 = -lambda_ * dets[j].dopplerHz - k.rdot;
        const double s00 = k.p00 + rR, s01 = k.p01, s11 = k.p11 + rV, det = s00 * s11 - s01 * s01;
        if (det <= 0) continue;
        const double d2 = (s11 * y0 * y0 - 2 * s01 * y0 * y1 + s00 * y1 * y1) / det;
        if (d2 < tp_.gate) pairs.push_back({d2, i, j});
    }
    std::sort(pairs.begin(), pairs.end(), [](const Pair& a, const Pair& b) { return a.d2 < b.d2; });
    std::vector<char> tUsed(tr_.size(), 0), mUsed(dets.size(), 0);
    for (const Pair& pr : pairs) {
        if (tUsed[pr.t] || mUsed[pr.m]) continue;
        tUsed[pr.t] = mUsed[pr.m] = 1;
        Track& k = tr_[pr.t];
        const Detection& m = dets[pr.m];
        const double y0 = m.rangeM - k.r, y1 = -lambda_ * m.dopplerHz - k.rdot;
        const double s00 = k.p00 + rR, s01 = k.p01, s11 = k.p11 + rV, det = s00 * s11 - s01 * s01;
        const double i00 = s11 / det, i01 = -s01 / det, i11 = s00 / det;
        const double k00 = k.p00 * i00 + k.p01 * i01, k01 = k.p00 * i01 + k.p01 * i11;
        const double k10 = k.p01 * i00 + k.p11 * i01, k11 = k.p01 * i01 + k.p11 * i11;
        k.r += k00 * y0 + k01 * y1; k.rdot += k10 * y0 + k11 * y1;
        const double p00 = (1 - k00) * k.p00 - k01 * k.p01, p01 = (1 - k00) * k.p01 - k01 * k.p11;
        const double p10 = -k10 * k.p00 + (1 - k11) * k.p01, p11 = -k10 * k.p01 + (1 - k11) * k.p11;
        k.p00 = p00; k.p01 = 0.5 * (p01 + p10); k.p11 = p11;
        k.hits++; k.misses = 0; k.snrDb = m.snrDb;
        if (k.hits >= tp_.confirmHits) k.confirmed = true;
        k.hist.push_back({t_, k.r});
    }
    for (size_t i = 0; i < tr_.size(); i++) if (!tUsed[i]) tr_[i].misses++;
    tr_.erase(std::remove_if(tr_.begin(), tr_.end(), [&](const Track& k) { return k.misses > (k.confirmed ? tp_.dropMisses : 0); }), tr_.end());
    for (size_t j = 0; j < dets.size(); j++) {
        if (mUsed[j]) continue;
        Track k;
        k.id = nextId_++; k.r = dets[j].rangeM; k.rdot = -lambda_ * dets[j].dopplerHz;
        k.p00 = rR; k.p11 = rV; k.hits = 1; k.snrDb = dets[j].snrDb;
        k.hist.push_back({t_, k.r});
        tr_.push_back(k);
    }
}

// ---------------------------------------------------------------- geometry
namespace {
constexpr double kEarth = 6371008.8;
double rad(double d) { return d * kPi / 180; }
}
double distanceM(LatLon a, LatLon b) {
    const double dl = rad(b.lat - a.lat), dn = rad(b.lon - a.lon);
    const double h = std::sin(dl / 2) * std::sin(dl / 2) + std::cos(rad(a.lat)) * std::cos(rad(b.lat)) * std::sin(dn / 2) * std::sin(dn / 2);
    return 2 * kEarth * std::asin(std::min(1.0, std::sqrt(h)));
}
double bearingDeg(LatLon f, LatLon t) {
    const double dn = rad(t.lon - f.lon);
    const double y = std::sin(dn) * std::cos(rad(t.lat)), x = std::cos(rad(f.lat)) * std::sin(rad(t.lat)) - std::sin(rad(f.lat)) * std::cos(rad(t.lat)) * std::cos(dn);
    double b = std::atan2(y, x) * 180 / kPi;
    return b < 0 ? b + 360 : b;
}
std::vector<LatLon> bistaticEllipse(LatLon tx, LatLon rx, double excessM, int points) {
    std::vector<LatLon> out;
    const double L = distanceM(tx, rx);
    if (excessM <= 0 || L <= 0 || points < 8) return out;
    // local tangent plane at the mid point (east, north), good to ~0.1 % at 100 km
    double dlon = rx.lon - tx.lon; while (dlon > 180) dlon -= 360; while (dlon < -180) dlon += 360;
    LatLon mid{(tx.lat + rx.lat) / 2, tx.lon + dlon / 2};
    const double mN = kEarth * kPi / 180, mE = mN * std::cos(rad(mid.lat));
    const double ex = dlon * mE, ey = (rx.lat - tx.lat) * mN;
    const double phi = std::atan2(ey, ex), cF = std::hypot(ex, ey) / 2;
    const double a = (std::hypot(ex, ey) + excessM) / 2, b = std::sqrt(std::max(0.0, a * a - cF * cF));
    for (int i = 0; i < points; i++) {
        const double th = 2 * kPi * i / points, u = a * std::cos(th), v = b * std::sin(th);
        const double e = u * std::cos(phi) - v * std::sin(phi), n = u * std::sin(phi) + v * std::cos(phi);
        out.push_back({mid.lat + n / mN, mid.lon + e / mE});
    }
    return out;
}

// ---------------------------------------------------------------- chain
RadarResult PassiveRadar::process(const std::vector<cd>& ref, std::vector<cd> surv, double dt) {
    RadarResult r;
    r.cancelDb = cfg_.useNlms ? cancelNlms(ref, surv, cfg_.ecaTaps) : cancelEca(ref, surv, cfg_.ecaTaps);
    r.caf = crossAmbiguity(ref, surv, cfg_.caf);
    r.dets = cfarDetect(r.caf, cfg_.cfar);
    if (dt <= 0) dt = (double)cfg_.caf.cpiSamples() / cfg_.caf.sampleRate;
    trk_.update(r.dets, dt);
    r.dopplerRow.assign(r.caf.nd, -200.f);
    for (int d = 0; d < r.caf.nd; d++) {
        float m = 0;
        for (int k = std::max(cfg_.cfar.minRangeBin, 0); k < r.caf.nr; k++) m = std::max(m, r.caf.at(d, k));
        r.dopplerRow[d] = (float)dbOf(m);
    }
    return r;
}

// ---------------------------------------------------------------- simulator
PassiveSimSource::PassiveSimSource(PassiveSimConfig c) : cfg_(std::move(c)), rng_(cfg_.seed * 0x9E3779B97F4A7C15ull ^ 0xC2B2AE3D27D4EB4Full), hist_(512, cd(0)) {}

bool PassiveSimSource::read(df::Frame& out, size_t n) {
    const size_t H = hist_.size();
    std::vector<cd> tx(H + n);
    std::copy(hist_.begin(), hist_.end(), tx.begin());
    const double a = std::exp(-2 * kPi * 15e3 / cfg_.sampleRate), g = 1.0 / std::sqrt((1 - a) / (1 + a));
    const double devRms = 40e3;
    for (size_t i = 0; i < n; i++) {
        if (cfg_.fm) {
            mA_ = a * mA_ + (1 - a) * rng_.gauss();
            ph_ += 2 * kPi * devRms * (mA_ * g) / cfg_.sampleRate;
            ph_ = std::fmod(ph_, 2 * kPi);
            tx[H + i] = std::polar(1.0, ph_);
        } else {
            tx[H + i] = cd(rng_.gauss(), rng_.gauss()) * std::sqrt(0.5);
        }
    }
    out.assign(2, std::vector<cd>(n));
    const double nr = std::sqrt(std::pow(10.0, -cfg_.refSnrDb / 10) / 2), ns = std::sqrt(std::pow(10.0, cfg_.noiseDb / 10) / 2);
    for (size_t i = 0; i < n; i++) {
        const double ts = (double)(t_ + i) / cfg_.sampleRate;
        out[0][i] = tx[H + i] + cd(rng_.gauss(), rng_.gauss()) * nr;
        cd s = cd(rng_.gauss(), rng_.gauss()) * ns;
        for (const SimClutter& c : cfg_.clutter) {
            if ((size_t)c.delaySamples > H) continue;
            s += std::polar(std::pow(10.0, c.levelDb / 20), c.phase) * tx[H + i - c.delaySamples];
        }
        for (const SimTarget& t : cfg_.targets) {
            const double tau = (t.rangeM + t.rateMps * ts) / kC;
            const long d = std::lround(tau * cfg_.sampleRate);
            if (d < 0 || (size_t)d > H) continue;
            const double cyc = std::fmod(cfg_.carrierHz * tau, 1.0);
            s += std::polar(std::pow(10.0, t.levelDb / 20), -2 * kPi * cyc) * tx[H + i - d];
        }
        out[1][i] = s;
    }
    std::copy(tx.end() - H, tx.end(), hist_.begin());
    t_ += n;
    return true;
}

// ---------------------------------------------------------------- two files
TwoFileSource::TwoFileSource(const std::string& rp, const std::string& sp, double fs, double fc) : fs_(fs), fc_(fc) {
    a_ = fopen(rp.c_str(), "rb"); b_ = fopen(sp.c_str(), "rb");
    if (!a_ || !b_) { if (a_) fclose((FILE*)a_); if (b_) fclose((FILE*)b_); a_ = b_ = nullptr; }
}
TwoFileSource::~TwoFileSource() { if (a_) fclose((FILE*)a_); if (b_) fclose((FILE*)b_); }
bool TwoFileSource::read(df::Frame& out, size_t n) {
    if (!ok()) return false;
    std::vector<float> ba(2 * n), bb(2 * n);
    const size_t ga = fread(ba.data(), 8, n, (FILE*)a_), gb = fread(bb.data(), 8, n, (FILE*)b_);
    const size_t g = std::min(ga, gb);
    out.assign(2, std::vector<cd>(g));
    for (size_t i = 0; i < g; i++) { out[0][i] = cd(ba[2 * i], ba[2 * i + 1]); out[1][i] = cd(bb[2 * i], bb[2 * i + 1]); }
    return g > 0;
}

}   // namespace dect2::radar
