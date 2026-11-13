// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Receiver frequency self-calibration (selfcal.h, docs/self-calibration.md).
#include "dect2/selfcal/selfcal.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>

namespace dect2::selfcal {

namespace {
const double kPi = 3.14159265358979323846;
typedef std::complex<double> cd;

void fft(std::vector<cd>& a) {
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; i++) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        const cd wl = std::polar(1.0, -2 * kPi / (double)len);
        for (size_t i = 0; i < n; i += len) {
            cd w(1, 0);
            for (size_t k = 0; k < len / 2; k++) {
                const cd u = a[i + k], v = a[i + k + len / 2] * w;
                a[i + k] = u + v; a[i + k + len / 2] = u - v;
                w *= wl;
                if ((k & 127) == 127) w = std::polar(1.0, -2 * kPi * (double)(k + 1) / (double)len);   // keep the twiddle exact
            }
        }
    }
}
double wrapPi(double x) { while (x > kPi) x -= 2 * kPi; while (x < -kPi) x += 2 * kPi; return x; }
}   // namespace

// ---------------------------------------------------------------- ToneMeter

ToneMeter::ToneMeter(double fs, double expectedHz, double centerHz, double searchPpm)
    : fs_(fs), exp_(expectedHz), ctr_(centerHz), nom_(expectedHz + centerHz), searchPpm_(searchPpm) {
    const double win = searchPpm_ * 1e-6 * std::fabs(nom_) * 1.5 + 60.0;   // Hz either side of expectedHz that we must see
    dec_ = std::max(1, (int)std::floor(fs_ / (2.4 * win)));
    fsd_ = fs_ / dec_;
    blockLen_ = std::max<size_t>(1, (size_t)std::llround(blockSec_ * fsd_));
    blockSec_ = (double)blockLen_ / fsd_;
    acqLen_ = std::max<size_t>(64, (size_t)std::ceil(0.5 * fsd_));
    mixStep_ = 2 * kPi * exp_ / fs_;
    stepRot_ = std::polar(1.0, -mixStep_);
    upAcc_ = downAcc_ = prevDown_ = cd(0, 0);
}

void ToneMeter::push(const float* r, size_t n) {
    std::vector<std::complex<float>> t(n);
    for (size_t i = 0; i < n; i++) t[i] = {r[i], 0.f};
    push(t.data(), n);
}

void ToneMeter::push(const std::complex<float>* x, size_t n) {
    // mix by -expectedHz, then a triangular (sinc^2) low-pass of length 2 * dec_ with hop dec_
    for (size_t i = 0; i < n; i++) {
        if ((mixN_ & 1023) == 0) rot_ = std::polar(1.0, -mixStep_ * (double)mixN_);   // exact phase now and then
        const cd m = cd(x[i].real(), x[i].imag()) * rot_;
        rot_ *= stepRot_;
        mixN_++;
        const double up = (double)(decN_ + 1) / (dec_ + 1), down = 1.0 - up;
        upAcc_ += m * up; downAcc_ += m * down;
        if (++decN_ >= dec_) {
            const cd out = (upAcc_ + prevDown_) / (double)dec_;
            const bool first = !haveFirst_;
            prevDown_ = downAcc_; upAcc_ = downAcc_ = cd(0, 0); decN_ = 0; haveFirst_ = true;
            if (!first) process(out);
        }
    }
}

void ToneMeter::process(const cd& d) {
    seconds_ += 1.0 / fsd_;
    if (!acquired_) {
        acq_.push_back(d);
        if (acq_.size() >= acqLen_) acquire();
        return;
    }
    track(d);
}

void ToneMeter::acquire() {
    size_t n = 1;
    while (n < acq_.size() * 4) n <<= 1;
    std::vector<cd> a(n, cd(0, 0));
    const size_t m = acq_.size();
    for (size_t i = 0; i < m; i++) a[i] = acq_[i] * (0.5 - 0.5 * std::cos(2 * kPi * (double)i / (double)m));
    fft(a);
    const double bin = fsd_ / (double)n;
    const double lim = std::min(fsd_ / 2 - bin, searchPpm_ * 1e-6 * std::fabs(nom_) * 1.5 + 40.0);
    double best = -1; long bk = 0;
    const long kl = (long)std::floor(lim / bin);
    for (long k = -kl; k <= kl; k++) {
        const double p = std::norm(a[(size_t)((k + (long)n) % (long)n)]);
        if (p > best) { best = p; bk = k; }
    }
    double frac = 0;
    {
        const double l = std::log(std::norm(a[(size_t)((bk - 1 + (long)n) % (long)n)]) + 1e-300), c = std::log(best + 1e-300),
                     r = std::log(std::norm(a[(size_t)((bk + 1 + (long)n) % (long)n)]) + 1e-300);
        const double den = l - 2 * c + r;
        if (den < 0) frac = 0.5 * (l - r) / den;
        frac = std::max(-0.5, std::min(0.5, frac));
    }
    fine_ = ((double)bk + frac) * bin;
    acquired_ = true;
    ph_ = 0;
    std::vector<cd> keep;
    keep.swap(acq_);
    for (const cd& d : keep) track(d);
}

void ToneMeter::track(const cd& d) {
    blk_ += d * std::polar(1.0, -ph_);
    ph_ += 2 * kPi * fine_ / fsd_;
    if (ph_ > 2 * kPi) ph_ -= 2 * kPi; else if (ph_ < -2 * kPi) ph_ += 2 * kPi;
    if (++blkN_ >= blockLen_) { z_.push_back(blk_); blk_ = cd(0, 0); blkN_ = 0; }
}

Estimate ToneMeter::estimate() const {
    Estimate e;
    e.seconds = seconds_;
    const size_t K = z_.size();
    if (!acquired_ || K < 8) return e;
    // the phase of the block sums grows by 2 pi dF blockSec: unwrap and fit a line
    std::vector<double> ph(K);
    ph[0] = std::arg(z_[0]);
    for (size_t k = 1; k < K; k++) ph[k] = ph[k - 1] + wrapPi(std::arg(z_[k]) - std::arg(z_[k - 1]));
    double sx = 0, sy = 0;
    for (size_t k = 0; k < K; k++) { sx += (double)k; sy += ph[k]; }
    const double mx = sx / (double)K, my = sy / (double)K;
    double sxx = 0, sxy = 0;
    for (size_t k = 0; k < K; k++) { sxx += ((double)k - mx) * ((double)k - mx); sxy += ((double)k - mx) * (ph[k] - my); }
    const double slope = sxy / sxx;                      // rad per block
    double ss = 0;
    for (size_t k = 0; k < K; k++) { const double r = ph[k] - my - slope * ((double)k - mx); ss += r * r; }
    const double sphi = std::sqrt(ss / (double)(K - 2));
    if (sphi > 1.0) return e;                            // no steady tone
    const double dF = slope / (2 * kPi * blockSec_);     // Hz, on top of the acquisition's fine_
    const double sigF = (sphi / std::sqrt(sxx)) / (2 * kPi * blockSec_);
    const double meas = ctr_ + exp_ + fine_ + dF;
    if (meas <= 0 || nom_ <= 0) return e;
    e.valid = true;
    e.ppm = (nom_ / meas - 1.0) * 1e6;
    e.sigmaPpm = sigF / nom_ * 1e6;
    e.snrDb = 10 * std::log10(1.0 / (2 * std::max(sphi * sphi, 1e-9)));
    return e;
}

// ---------------------------------------------------------------- FM pilot

FmPilotMeter::FmPilotMeter(double fs) : tone_(fs, 19000.0, 0.0, 100.0), fs_(fs) {}

void FmPilotMeter::push(const std::complex<float>* x, size_t n) {
    std::vector<float> y(n);
    for (size_t i = 0; i < n; i++) {
        const std::complex<float> p = x[i] * std::conj(prev_);
        y[i] = (float)(std::atan2(p.imag(), p.real()) * fs_ / (2 * kPi));   // Hz of deviation: the MPX
        prev_ = x[i];
    }
    tone_.push(y.data(), n);
}

Estimate FmPilotMeter::estimate() const {
    Estimate e = tone_.estimate();
    e.source = "FM stereo pilot";
    return e;
}

// ---------------------------------------------------------------- other references

Estimate fromFreqError(const std::string& source, double nominalHz, double measuredHz, double sigmaHz, double seconds) {
    Estimate e;
    e.source = source;
    e.seconds = seconds;
    if (nominalHz <= 0 || measuredHz <= 0) return e;
    e.valid = true;
    e.ppm = (nominalHz / measuredHz - 1.0) * 1e6;
    e.sigmaPpm = std::fabs(sigmaHz) / nominalHz * 1e6;
    return e;
}

Estimate fromCell(double centerHz, double cfoHz, double sigmaHz) {
    Estimate e = fromFreqError("cell carrier", centerHz, centerHz + cfoHz, std::max(sigmaHz, 0.05e-6 * centerHz));
    if (e.valid && std::fabs(e.ppm) > 60.0) e.valid = false;
    return e;
}

Estimate fromGpsCarrier(double residualHz, double sigmaHz) {
    const double l1 = 1575.42e6;
    Estimate e = fromFreqError("GPS L1", l1, l1 + residualHz, std::max(sigmaHz, 0.05e-6 * l1));
    if (e.valid && std::fabs(e.ppm) > 60.0) e.valid = false;
    return e;
}

Estimate combine(const std::vector<Estimate>& v) {
    std::vector<Estimate> ok;
    for (const auto& e : v) if (e.valid && e.sigmaPpm >= 0) ok.push_back(e);
    Estimate r;
    if (ok.empty()) return r;
    std::vector<double> p;
    for (const auto& e : ok) p.push_back(e.ppm);
    std::sort(p.begin(), p.end());
    const double med = p.size() % 2 ? p[p.size() / 2] : 0.5 * (p[p.size() / 2 - 1] + p[p.size() / 2]);
    double sw = 0, swp = 0, secs = 0, snr = -1e9;
    int used = 0;
    std::string src;
    for (const auto& e : ok) {
        if (ok.size() >= 3 && std::fabs(e.ppm - med) > std::max(3 * e.sigmaPpm, 0.3)) continue;   // a reference that is off itself
        const double w = 1.0 / std::max(e.sigmaPpm * e.sigmaPpm, 1e-6);
        sw += w; swp += w * e.ppm; secs += e.seconds; snr = std::max(snr, e.snrDb); used++;
        if (!e.source.empty() && src.find(e.source) == std::string::npos) src += (src.empty() ? "" : ", ") + e.source;
    }
    if (!used) return r;
    r.valid = true; r.ppm = swp / sw; r.sigmaPpm = std::sqrt(1.0 / sw); r.seconds = secs; r.snrDb = snr; r.source = src;
    return r;
}

double atscPilotHz(int ch) {
    double lo = 0;
    if (ch >= 2 && ch <= 4) lo = 54e6 + (ch - 2) * 6e6;
    else if (ch == 5) lo = 76e6;
    else if (ch == 6) lo = 82e6;
    else if (ch >= 7 && ch <= 13) lo = 174e6 + (ch - 7) * 6e6;
    else if (ch >= 14 && ch <= 36) lo = 470e6 + (ch - 14) * 6e6;
    return lo > 0 ? lo + 309440.559 : 0;
}

// ---------------------------------------------------------------- Store

void Store::add(const Entry& e) { e_.push_back(e); }

std::vector<Entry> Store::history(const std::string& serial) const {
    std::vector<Entry> r;
    for (const auto& e : e_) if (e.serial == serial) r.push_back(e);
    std::stable_sort(r.begin(), r.end(), [](const Entry& a, const Entry& b) { return a.utc < b.utc; });
    return r;
}

bool Store::latest(const std::string& serial, Entry& out) const {
    const auto h = history(serial);
    if (h.empty()) return false;
    out = h.back();
    return true;
}

bool Store::drift(const std::string& serial, double windowSec, double& ppmPerMin) const {
    const auto h = history(serial);
    if (h.size() < 3) return false;
    const double t1 = h.back().utc;
    std::vector<Entry> w;
    for (const auto& e : h) if (e.utc >= t1 - windowSec) w.push_back(e);
    if (w.size() < 3 || w.back().utc - w.front().utc < 30) return false;
    double mt = 0, mp = 0;
    for (const auto& e : w) { mt += e.utc; mp += e.ppm; }
    mt /= (double)w.size(); mp /= (double)w.size();
    double sxx = 0, sxy = 0;
    for (const auto& e : w) { sxx += (e.utc - mt) * (e.utc - mt); sxy += (e.utc - mt) * (e.ppm - mp); }
    if (sxx <= 0) return false;
    ppmPerMin = sxy / sxx * 60.0;
    return true;
}

bool Store::value(const std::string& serial, double& ppm) const {
    const auto h = history(serial);
    if (h.empty()) return false;
    double ppmPerMin = 0;
    const bool settled = drift(serial, 3600, ppmPerMin) && std::fabs(ppmPerMin) < 0.01;   // under 0.6 ppm an hour
    if (!settled) { ppm = h.back().ppm; return true; }
    double sw = 0, swp = 0;
    for (const auto& e : h) {
        if (e.utc < h.back().utc - 3600) continue;
        const double w = 1.0 / std::max(e.sigmaPpm * e.sigmaPpm, 1e-6);
        sw += w; swp += w * e.ppm;
    }
    ppm = swp / sw;
    return true;
}

namespace {
std::string clean(std::string s) { for (char& c : s) if (c == '\t' || c == '\n' || c == '\r') c = ' '; return s; }
}

std::string Store::toText() const {
    std::string r = "# serial\tutc\tppm\tsigmaPpm\ttempC\tsource\n";
    for (const auto& e : e_) {
        char b[160];
        snprintf(b, sizeof b, "\t%.0f\t%.4f\t%.4f\t%.1f\t", e.utc, e.ppm, e.sigmaPpm, e.tempC);
        r += clean(e.serial) + b + clean(e.source) + "\n";
    }
    return r;
}

bool Store::save(const std::string& path) const {
    std::ofstream f(path, std::ios::trunc);
    if (!f) return false;
    f << toText();
    return (bool)f;
}

bool Store::load(const std::string& path) {
    std::ifstream f(path);
    if (!f) return false;
    std::stringstream ss;
    ss << f.rdbuf();
    fromText(ss.str());
    return true;
}

void Store::fromText(const std::string& text) {
    e_.clear();
    std::stringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::vector<std::string> c;
        std::stringstream ss(line);
        std::string t;
        while (std::getline(ss, t, '\t')) c.push_back(t);
        if (c.size() < 4) continue;
        Entry e;
        e.serial = c[0]; e.utc = atof(c[1].c_str()); e.ppm = atof(c[2].c_str()); e.sigmaPpm = atof(c[3].c_str());
        if (c.size() > 4) e.tempC = atof(c[4].c_str());
        if (c.size() > 5) e.source = c[5];
        e_.push_back(e);
    }
}

void applyPpm(std::map<std::string, std::string>& radio, double ppm) {
    if (!(ppm > -1000 && ppm < 1000) || std::fabs(ppm) < 0.0005) { radio.erase("ppm"); return; }
    char b[48];
    snprintf(b, sizeof b, "%.4g", ppm);
    radio["ppm"] = b;
}

// ---------------------------------------------------------------- LiveCal

struct LiveCal::Impl {
    mutable std::mutex mu;
    double hz = 0, rate = 0, center = 0;
    // FM: mixer, low-pass FIR (about 100 kHz cutoff), decimation
    std::unique_ptr<FmPilotMeter> fm;
    std::unique_ptr<ToneMeter> tone;
    std::vector<float> taps;
    std::vector<std::complex<float>> hist, out;
    int dec = 1;
    long phase = 0;            // samples until the next output
    cd rot{1, 0}, step{1, 0};
    unsigned long long n = 0;
    double seconds = 0;
};

LiveCal::LiveCal(Kind k, double hz) : kind_(k), d_(new Impl) { d_->hz = hz; }
LiveCal::~LiveCal() { delete d_; }

double LiveCal::seconds() const { std::lock_guard<std::mutex> lk(d_->mu); return d_->seconds; }

void LiveCal::push(const std::complex<float>* x, size_t n, double rate, double center) {
    Impl& d = *d_;
    std::lock_guard<std::mutex> lk(d.mu);
    if (rate != d.rate || center != d.center || (!d.fm && !d.tone)) {
        d.rate = rate; d.center = center; d.seconds = 0; d.fm.reset(); d.tone.reset();
        const double b = d.hz - center;
        if (kind_ == Carrier) d.tone.reset(new ToneMeter(rate, b, center, 100.0));
        else {
            d.dec = std::max(1, (int)std::floor(rate / 240000.0));
            const double fo = rate / d.dec;
            if (fo < 128000.0) return;              // too slow for an FM channel
            d.fm.reset(new FmPilotMeter(fo));
            const int nt = 8 * d.dec + 1;           // windowed sinc, cutoff 95 kHz
            d.taps.assign((size_t)nt, 0.f);
            const double fc = 95000.0 / rate;
            double sum = 0;
            for (int i = 0; i < nt; i++) {
                const double t = i - (nt - 1) / 2.0;
                const double h = (t == 0 ? 2 * fc : std::sin(2 * kPi * fc * t) / (kPi * t)) * (0.54 - 0.46 * std::cos(2 * kPi * i / (nt - 1)));
                d.taps[(size_t)i] = (float)h; sum += h;
            }
            for (auto& v : d.taps) v = (float)(v / sum);
            d.hist.assign((size_t)nt - 1, {0.f, 0.f});
            d.phase = 0; d.n = 0;
            d.step = std::polar(1.0, -2 * kPi * b / rate);
        }
    }
    d.seconds += (double)n / rate;
    if (kind_ == Carrier) { if (d.tone) d.tone->push(x, n); return; }
    if (!d.fm) return;
    // mix the station to DC, filter and decimate
    std::vector<std::complex<float>> buf(d.hist);
    buf.reserve(d.hist.size() + n);
    for (size_t i = 0; i < n; i++) {
        if ((d.n & 4095) == 0) d.rot = std::polar(1.0, -2 * kPi * (d.hz - d.center) / rate * (double)d.n);
        const cd m = cd(x[i].real(), x[i].imag()) * d.rot;
        d.rot *= d.step; d.n++;
        buf.push_back({(float)m.real(), (float)m.imag()});
    }
    const size_t nt = d.taps.size();
    d.out.clear();
    size_t i = (size_t)d.phase;
    for (; i + nt <= buf.size(); i += (size_t)d.dec) {
        float re = 0, im = 0;
        for (size_t k = 0; k < nt; k++) { re += d.taps[k] * buf[i + k].real(); im += d.taps[k] * buf[i + k].imag(); }
        d.out.push_back({re, im});
    }
    d.phase = (long)(i - (buf.size() - (nt - 1)));   // may be negative-free: i >= buf.size() - nt + 1 here
    if (d.phase < 0) d.phase = 0;
    d.hist.assign(buf.end() - (long)(nt - 1), buf.end());
    if (!d.out.empty()) d.fm->push(d.out.data(), d.out.size());
}

Estimate LiveCal::estimate() const {
    std::lock_guard<std::mutex> lk(d_->mu);
    Estimate e;
    if (d_->fm) e = d_->fm->estimate();
    else if (d_->tone) { e = d_->tone->estimate(); e.source = "carrier"; }
    return e;
}

// ---------------------------------------------------------------- Wizard

Wizard::Wizard(double targetSigmaPpm, double giveUpSec) : target_(targetSigmaPpm), giveUp_(giveUpSec) {}

void Wizard::start() { stage_ = Searching; progress_ = 0; res_ = Estimate(); }

Wizard::Stage Wizard::update(const Estimate& e, double sec) {
    if (stage_ == Idle || stage_ == Done || stage_ == Failed) return stage_;
    if (!e.valid) {
        stage_ = Searching;
        progress_ = 0;
        if (sec > giveUp_) stage_ = Failed;
        return stage_;
    }
    stage_ = Integrating;
    res_ = e;
    // progress: the uncertainty falls with time^1.5, so use the ratio of the target to the current sigma
    progress_ = std::min(1.0, std::max(0.0, std::pow(target_ / std::max(e.sigmaPpm, 1e-9), 0.5)));
    if (e.sigmaPpm <= target_) { stage_ = Done; progress_ = 1; }
    else if (sec > giveUp_) {
        // a result worse than asked for is still given when it is under 1 ppm; otherwise it failed
        stage_ = e.sigmaPpm < 1.0 ? Done : Failed;
        if (stage_ == Done) progress_ = 1;
    }
    return stage_;
}

std::string Wizard::status() const {
    char b[200];
    switch (stage_) {
    case Idle: return "Ready.";
    case Searching: return "Looking for a reference signal.";
    case Integrating: snprintf(b, sizeof b, "Measuring: %+.2f ppm, uncertainty %.2f ppm.", res_.ppm, res_.sigmaPpm); return b;
    case Done: snprintf(b, sizeof b, "Done: %+.2f ppm (uncertainty %.2f ppm).", res_.ppm, res_.sigmaPpm); return b;
    case Failed: return "No usable reference signal was found.";
    }
    return "";
}

}   // namespace dect2::selfcal
