// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Detection only: pulse radars (marine and weather) by their pulses and the beam passing the receiver; OFDM systems (LDACS, AeroMACS, C-V2X) by the cyclic prefix.
#include "avroad_int.h"
#include <algorithm>
#include <cstring>
#include <deque>
#include <map>

namespace dect2::avroad {

namespace {

float median(std::vector<float> v) { if (v.empty()) return 0; std::nth_element(v.begin(), v.begin() + (long)v.size() / 2, v.end()); return v[v.size() / 2]; }

// ---- Pulse radar. One antenna sees the beam go past once per rotation: the crossings give the rotation period and, with their width, the beamwidth.
// The true bearing of the radar cannot be measured with one antenna; docs/AVIA-ROAD-3.8.md says how to get it (direction finding with two coherent channels).
class RadarDec : public Decoder {
public:
    explicit RadarDec(bool weather) : weather_(weather) {}
    bool configure(double fs) override { fs_ = fs; bucket_ = (int64_t)(fs * 0.002); reset(); return true; }
    void reset() override {
        n_ = 0; noise_ = 0; inPulse_ = false; lastStart_ = -1; bucketAt_ = 0; bucketPeak_ = 0;
        buckets_.clear(); crossings_.clear(); inCross_ = false; prevSweepLevel_ = 0; sweeps_ = 0; volumes_ = 0; volSweeps_ = 0; sweepN_ = 0; pris_.clear(); widths_.clear();
    }
    void feed(const cf32* x, size_t n) override {
        for (size_t i = 0; i < n; i++) {
            const float p = std::norm(x[i]);
            if (n_ < 2000) { noise_ += p; if (n_ == 1999) noise_ /= 2000; n_++; continue; }
            if (!inPulse_ && p > noise_ * kOn) {
                inPulse_ = true; pStart_ = n_; pPeak_ = p;
            } else if (inPulse_) {
                pPeak_ = std::max(pPeak_, p);
                if (p < noise_ * kOff || n_ - pStart_ > (int64_t)(fs_ * 200e-6)) { inPulse_ = false; pulse(pStart_, n_ - pStart_, pPeak_); }
            }
            if (!inPulse_ && p < noise_ * 4) noise_ += (p - noise_) * (float)(1 / (fs_ * 0.2));
            n_++;
            if (n_ - bucketAt_ >= bucket_) closeBucket();
        }
    }
    void fill(AvRoadTelemetry& t) const override {
        const bool have = lastRotation_ > 0;
        t.headline = have ? fmt("rotation %.2f s (%.1f rpm), PRF %.0f Hz", lastRotation_, 60 / lastRotation_, prfNow_)
                           : pulses_ ? fmt("%llu pulses, PRF %.0f Hz, waiting for the beam to pass again", (unsigned long long)pulses_, prfNow_)
                                     : std::string(weather_ ? "listening for weather radar pulses" : "listening for marine radar pulses");
        t.facts.push_back({"pulse width", fmt("%.2f us", widthNow_)});
        t.facts.push_back({"PRF", prfText_.empty() ? std::string("-") : prfText_});
        t.facts.push_back({"beamwidth", beamDeg_ > 0 ? fmt("%.1f deg (from the crossing width and the rotation)", beamDeg_) : std::string("-")});
        t.facts.push_back({"crossings", fmt("%zu", crossings_.size())});
        if (weather_) t.facts.push_back({"scan pattern", fmt("%d sweeps in this volume, %d volumes, last volume %.0f s", volSweeps_, volumes_, lastVolumeSec_)});
        t.facts.push_back({"bearing", "needs direction finding (two coherent channels); one antenna only gives the rotation"});
    }
private:
    static constexpr float kOn = 20, kOff = 5;        // above the noise, in power
    struct Cross { double t; float peakDb, widthSec; };
    void pulse(int64_t start, int64_t len, float peak) {
        pulses_++;
        if (lastStart_ >= 0) {
            const double pri = (double)(start - lastStart_) / fs_;
            if (pri > 50e-6 && pri < 20e-3) { pris_.push_back((float)pri); if (pris_.size() > 400) pris_.pop_front(); }
        }
        lastStart_ = start;
        widths_.push_back((float)(len / fs_ * 1e6)); if (widths_.size() > 200) widths_.pop_front();
        bucketPeak_ = std::max(bucketPeak_, peak);
        out->energy();
    }
    void closeBucket() {
        const float lv = toDb(bucketPeak_ / std::max(noise_, 1e-12f));
        const double t = (double)bucketAt_ / fs_;
        bucketAt_ = n_; bucketPeak_ = 0;
        buckets_.push_back({t, lv});
        if (buckets_.size() > 8000) buckets_.pop_front();       // 16 s
        float mx = 0;
        for (size_t i = buckets_.size() > 3500 ? buckets_.size() - 3500 : 0; i < buckets_.size(); i++) mx = std::max(mx, buckets_[i].second);
        if (mx < 13) { if (inCross_) closeCross(); return; }
        const float thr = mx - 6;
        if (lv >= thr) {
            if (!inCross_) { inCross_ = true; ct_ = 0; cw_ = 0; cp_ = 0; cFrom_ = t; ct2_ = t; gap_ = 0; }
            const float w = std::pow(10.f, lv / 10);
            ct_ += w * t; cw_ += w; cp_ = std::max(cp_, lv); ct2_ = t; gap_ = 0;
        } else if (inCross_) {
            if (++gap_ > 6) closeCross();
        }
    }
    void closeCross() {
        inCross_ = false;
        Cross c{ct_ / cw_, cp_, (float)(ct2_ - cFrom_ + 0.002)};
        crossings_.push_back(c);
        if (crossings_.size() > 80) crossings_.pop_front();
        update();
    }
    void update() {
        std::map<int, int> bins;
        for (float p : pris_) bins[(int)std::lround(p * 2e5)]++;      // 5 us bins
        std::vector<std::pair<int, int>> v(bins.begin(), bins.end());
        std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.second > b.second; });
        prfText_.clear();
        if (!v.empty()) {
            prfNow_ = (float)(1 / (v[0].first / 2e5));
            prfText_ = fmt("%.0f Hz", prfNow_);
            if (v.size() > 1 && v[1].second * 3 > v[0].second && std::abs(v[0].first - v[1].first) > 3) {
                const float f2 = (float)(1 / (v[1].first / 2e5));
                const float lo = std::min(prfNow_, f2), hi = std::max(prfNow_, f2);
                prfText_ = fmt("dual %.0f / %.0f Hz, ratio %.2f", hi, lo, hi / lo);
            }
        }
        widthNow_ = median(std::vector<float>(widths_.begin(), widths_.end()));
        if (crossings_.size() < 3) return;
        std::vector<float> d;
        for (size_t i = 1; i < crossings_.size(); i++) d.push_back((float)(crossings_[i].t - crossings_[i - 1].t));
        const float m = median(d);
        if (m < 0.5f || m > 60) return;
        int good = 0; for (float q : d) good += std::fabs(q - m) < 0.2f * m;
        if (good * 2 < (int)d.size()) return;
        const float prev = lastRotation_;
        lastRotation_ = m;
        beamDeg_ = crossings_.back().widthSec * 360 / m;
        out->good(crossings_.back().peakDb);
        const Cross& c = crossings_.back();
        if (weather_) {
            sweepN_++;
            bool newVol = prevSweepLevel_ != 0 && c.peakDb - prevSweepLevel_ > kNewVolumeDb;
            if (newVol) { volumes_++; lastVolumeSec_ = (float)(c.t - volumeStart_); volumeStart_ = c.t; volSweeps_ = 0; sweepN_ = 1; }
            volSweeps_++;
            prevSweepLevel_ = c.peakDb;
            out->row("Sweep", fmt("%d", sweepN_), fmt("period %.2f s, %s, pulse %.2f us, peak %.1f dB%s", m, prfText_.c_str(), widthNow_, c.peakDb, newVol ? ", new volume" : ""), c.peakDb);
        } else if (prev <= 0 || std::fabs(prev - m) > 0.05f * m || c.t - lastRow_ > 30) {
            lastRow_ = c.t;
            out->row("Radar", fmt("%.0f Hz", prfNow_), fmt("%.1f rpm, %s, pulse %.2f us, beamwidth %.1f deg", 60 / m, prfText_.c_str(), widthNow_, beamDeg_), c.peakDb);
        }
    }
    static constexpr float kNewVolumeDb = 2.5f;
    bool weather_;
    double fs_ = 0;
    int64_t bucket_ = 0, n_ = 0, bucketAt_ = 0, pStart_ = 0, lastStart_ = -1;
    float noise_ = 0, pPeak_ = 0, bucketPeak_ = 0, widthNow_ = 0, prfNow_ = 0, beamDeg_ = 0, lastRotation_ = 0, prevSweepLevel_ = 0, lastVolumeSec_ = 0;
    bool inPulse_ = false, inCross_ = false;
    std::deque<std::pair<double, float>> buckets_;
    double ct_ = 0, cFrom_ = 0, ct2_ = 0, lastRow_ = -1e9, volumeStart_ = 0; float cw_ = 0, cp_ = 0; int gap_ = 0;
    std::deque<Cross> crossings_;
    std::deque<float> pris_, widths_;
    std::string prfText_;
    int sweeps_ = 0, volumes_ = 0, volSweeps_ = 0, sweepN_ = 0;
    uint64_t pulses_ = 0;
};

// ---- OFDM by its cyclic prefix: the last Ncp samples of each symbol repeat Nfft samples earlier. The metric folded over one symbol period peaks once when the numerology fits.
struct Numerology { const char* name; double workRate; int nfft, ncp, ncp2 /* 0: one CP length */; int perSlot; double framePeriodSec; };
const Numerology kNum[3] = {
    {"LDACS", 625000, 64, 11, 0, 0, 240e-3},
    {"AeroMACS", 5600000, 512, 64, 0, 0, 5e-3},
    {"C-V2X", 15360000, 1024, 72, 80, 7, 1e-3},
};

class OfdmDetect : public Decoder {
public:
    explicit OfdmDetect(int k) : k_(k), nu_(kNum[k]) {}
    bool configure(double fs) override {
        fs_ = fs;
        const double wr = nu_.workRate;
        if (!ch_.configure(fs, wr, wr * 0.42, wr * 0.5)) return false;
        sym_ = nu_.nfft + nu_.ncp;
        reset();
        return true;
    }
    void reset() override {
        ch_.reset(); line_.assign((size_t)nu_.nfft + nu_.ncp + 2, cf32(0, 0)); w_ = 0; sum_ = cf32(0, 0); sp_ = 0;
        n_ = 0; fold_.assign((size_t)sym_, 0.f); foldPw_.assign((size_t)sym_, 0.f); foldC_.assign((size_t)sym_, cf32(0, 0)); foldN_ = 0; foldP_ = 0; env_.assign(kEnvBins, 0.f); envN_ = 0;
    }
    void feed(const cf32* x, size_t n) override {
        ch_.process(x, n, y_);
        const int N = nu_.nfft, cp = nu_.ncp;
        const size_t L = line_.size();
        for (const cf32& v : y_) {
            // x[t] x*[t-N] summed over the last cp samples, updated one sample at a time
            const cf32 now = v * std::conj(line_[(w_ + L - (size_t)N) % L]);
            const cf32 oldTerm = line_[(w_ + L - (size_t)cp) % L] * std::conj(line_[(w_ + L - (size_t)cp - (size_t)N + L * 2) % L]);
            sum_ += now - oldTerm; sp_ += std::norm(v) - std::norm(line_[(w_ + L - (size_t)cp) % L]);
            line_[w_] = v; w_ = (w_ + 1) % L;
            const float m = std::abs(sum_);
            int bin = (int)(n_ % sym_);
            fold_[(size_t)bin] += m;
            foldC_[(size_t)bin] += sum_;
            foldP_ += std::max(sp_, 0.f);
            foldPw_[(size_t)bin] += std::norm(v);
            n_++; foldN_++;
            if (foldN_ == (int64_t)(0.05 * nu_.workRate)) evaluate();
            // Frame structure from the envelope
            envAcc_ += std::norm(v); if (++envSub_ == kEnvSub) { const size_t eb = (size_t)((double)envCount_ * kEnvSub / nu_.workRate / nu_.framePeriodSec * kEnvBins) % kEnvBins; env_[eb] += envAcc_; envAcc_ = 0; envSub_ = 0; envCount_++; envN_++; }
        }
    }
    void fill(AvRoadTelemetry& t) const override {
        t.headline = detected_ ? fmt("%s: symbol %.1f us, CP %.1f us", nu_.name, sym_ / nu_.workRate * 1e6, nu_.ncp / nu_.workRate * 1e6) : fmt("searching for %s structure", nu_.name);
        t.facts.push_back({"symbol timing", fmt("sample %d of %d", peakBin_, sym_)});
        t.facts.push_back({"prefix correlation", fmt("%.2f", corr_)});
        t.facts.push_back({"carrier offset", detected_ ? fmt("%.0f Hz", cfo_) : std::string("-")});
        t.facts.push_back({"frame structure", frame_.empty() ? std::string("-") : frame_});
        t.facts.push_back({"content", k_ == 2 ? "structure only: the safety messages themselves (PSCCH / PSSCH turbo decode) are not read" : "structure only: not decoded (see docs/AVIA-ROAD-3.8.md)"});
    }
private:
    static constexpr int kEnvBins = 200, kEnvSub = 64;
    void evaluate() {
        float peak = 0, mean = 0; int at = 0;
        for (int i = 0; i < sym_; i++) { mean += fold_[(size_t)i]; if (fold_[(size_t)i] > peak) { peak = fold_[(size_t)i]; at = i; } }
        mean /= (float)sym_;
        float others = 0; int no = 0;
        for (int i = 0; i < sym_; i++) if (std::abs(i - at) > nu_.ncp && std::abs(i - at) < sym_ - nu_.ncp) { others += fold_[(size_t)i]; no++; }
        others /= (float)std::max(no, 1);
        const float ratio = peak / std::max(others, 1e-12f);
        const float wasDetected = detected_;
        corr_ = peak * (float)sym_ / std::max(foldP_, 1e-9f);      // 0 .. 1
        const bool alive = foldP_ / (float)std::max<int64_t>(foldN_, 1) > 1e-7f;        // silence: the running sums hold only rounding noise
        float pmin = 1e30f, psum = 0; for (float q : foldPw_) { pmin = std::min(pmin, q); psum += q; }
        const bool even = pmin > 0.3f * psum / (float)sym_;     // less than a few symbols of signal in this window (an edge of a burst) folds unevenly and proves nothing
        const bool hit = alive && even && ratio > kDetect && corr_ > kMinCorr;
        if (hit) lastHit_ = n_;
        detected_ = hit || (wasDetected && n_ - lastHit_ < (int64_t)(nu_.workRate * 2));
        if (hit) {
            peakBin_ = at; hits_++;
            cfo_ = std::arg(foldC_[(size_t)at]) / (2 * kPi * nu_.nfft / nu_.workRate);
            out->good(toDb(ratio));
            if (!wasDetected) out->row("Structure", nu_.name, fmt("OFDM symbol %.1f us, prefix %.1f us, %.0f samples/s", sym_ / nu_.workRate * 1e6, nu_.ncp / nu_.workRate * 1e6, nu_.workRate), toDb(ratio));
        } else if (alive && ratio > 1.3f) out->energy();
        // envelope folded over the frame period
        float emax = 0, esum = 0; for (float e : env_) { emax = std::max(emax, e); esum += e; }
        frame_.clear();
        if (envN_ > 2000 && esum > 0) {
            const float meanE = esum / kEnvBins;
            int on = 0; for (float e : env_) on += e > meanE * 0.5f;
            if (emax > 1.6f * meanE && on < kEnvBins * 8 / 10) frame_ = fmt("bursts: on for %.0f%% of each %.1f ms frame", 100.0 * on / kEnvBins, nu_.framePeriodSec * 1e3);
            else frame_ = fmt("continuous over each %.1f ms frame", nu_.framePeriodSec * 1e3);
        }
        std::fill(fold_.begin(), fold_.end(), 0.f); std::fill(foldPw_.begin(), foldPw_.end(), 0.f); std::fill(foldC_.begin(), foldC_.end(), cf32(0, 0)); foldN_ = 0; foldP_ = 0;
    }
    static constexpr float kDetect = 2.2f, kMinCorr = 0.05f;
    int k_;
    Numerology nu_;
    double fs_ = 0;
    Chain ch_;
    std::vector<cf32> y_, line_;
    size_t w_ = 0;
    cf32 sum_{0, 0};
    float sp_ = 0, foldP_ = 0;
    int sym_ = 1, peakBin_ = 0;
    int64_t n_ = 0, foldN_ = 0;
    std::vector<float> fold_, foldPw_, env_;
    int64_t lastHit_ = 0;
    std::vector<cf32> foldC_;
    int envSub_ = 0, envN_ = 0; int64_t envCount_ = 0; float envAcc_ = 0;
    bool detected_ = false;
    float corr_ = 0;
    double cfo_ = 0;
    uint64_t hits_ = 0;
    std::string frame_;
};

} // namespace

std::unique_ptr<Decoder> makeRadar(bool weather) { return std::make_unique<RadarDec>(weather); }
std::unique_ptr<Decoder> makeOfdmDetect(int kind) { return std::make_unique<OfdmDetect>(kind); }

} // namespace dect2::avroad
