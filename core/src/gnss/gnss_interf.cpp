// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// GNSS interference and spoofing monitor (see gnss_interf.h)
#include "dect2/gnss/gnss_interf.h"
#include "dect2/dsp/dsp_compat.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include "dect2/common/mathconst.h"

namespace dect2 {

namespace {
using dect2::math::kPi;
constexpr double kC = dect2::math::kSpeedOfLight;
constexpr size_t kMaxHistory = 900, kMaxEvents = 200, kMaxSpikes = 16;
constexpr size_t kAnalyse = 4096;            // samples of each block that go through the spectra (the pulse detector sees all of them)

int log2of(int n) { int l = 0; while ((1 << l) < n) l++; return l; }
double medianOf(std::vector<double> v) {
    if (v.empty()) return 0;
    const size_t m = v.size() / 2;
    std::nth_element(v.begin(), v.begin() + (long)m, v.end());
    return v[m];
}
std::string fmt(const char* f, double a, double b = 0, double c = 0) {
    char buf[160];
    snprintf(buf, sizeof buf, f, a, b, c);
    return buf;
}
} // namespace

GnssInterfMonitor::GnssInterfMonitor() { reset(); }

double GnssInterfMonitor::now() const {
    if (clock_) return clock_();
    return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
}

void GnssInterfMonitor::configure(double rateHz) {
    rate_ = rateHz;
    reset();
}

void GnssInterfMonitor::reset() {
    r_ = GnssInterference();
    winL_.assign(kLongN, 0.f); winS_.assign(kShortN, 0.f);
    for (int i = 0; i < kLongN; i++) winL_[(size_t)i] = (float)(0.5 - 0.5 * std::cos(2 * kPi * i / kLongN));
    for (int i = 0; i < kShortN; i++) winS_[(size_t)i] = (float)(0.5 - 0.5 * std::cos(2 * kPi * i / kShortN));
    re_.assign(kLongN, 0.f); im_.assign(kLongN, 0.f);
    acc_.assign(kLongN, 0.0);
    accFrames_ = 0; sumPow_ = 0; sumN_ = 0; secSamples_ = 0; perSec_ = (int64_t)std::llround(rate_); sigT_ = 0;
    lastPeak_ = -1; lastDelta_ = 0; lastStrong_ = false; strongPairs_ = 0; sweptPairs_ = 0; deltas_.clear();
    inPulse_ = false; pulseWins_ = 0; pulses_ = 0; hotWins_ = 0; totalWins_ = 0; pulseLevel_ = 0;
    pulseSweptUp_ = 0; pulseSweptDown_ = 0; pulseMulti_ = 0;
    learnFloors_.clear(); knownSpikes_.clear(); baseline_ = 0; clearSecs_ = 0; prevStatus_ = 0; prevReason_.clear(); prevKinds_ = 0;
    cn0Sum_ = 0; cn0Count_ = 0; cn0Tracked_ = 0; cn0Recent_.clear(); cn0RefSet_ = false;
    lastFixCount_ = 0; havePrevFix_ = false; nAvg_ = 0;
    for (int i = 0; i < 3; i++) { prevEcef_[i] = 0; sumEcef_[i] = 0; }
    prevBias_ = 0; prevFixT_ = 0; prevDrift_ = 0;
    posJumpUntil_ = -1; clockJumpUntil_ = -1; prevSpoof_ = false; prevSpoofReason_.clear(); spoofSec_ = 0; farSince_ = -1;
    pend_ = GnssInterfPoint(); pendN_ = 0;
}

void GnssInterfMonitor::event(int level, const std::string& text) {
    GnssInterfEvent e;
    e.t = sigT_; e.unixS = now(); e.level = level; e.text = text;
    r_.events.push_back(e);
    if (r_.events.size() > kMaxEvents) r_.events.erase(r_.events.begin(), r_.events.begin() + (long)(r_.events.size() - kMaxEvents));
    if (log_) log_("Interference monitor: " + text);
}

void GnssInterfMonitor::process(const cf32* x, size_t n, double dcI, double dcQ) {
    if (rate_ <= 0 || n == 0) return;
    const cf32 dc((float)dcI, (float)dcQ);
    const size_t a = std::min(n, kAnalyse);
    // the averaged spectrum (Hann windowed frames, centre in the middle)
    for (size_t f = 0; f + kLongN <= a; f += kLongN) {
        for (int i = 0; i < kLongN; i++) {
            const cf32 v = (x[f + (size_t)i] - dc) * winL_[(size_t)i];
            re_[(size_t)i] = v.real(); im_[(size_t)i] = v.imag();
        }
        fftSplit(re_.data(), im_.data(), log2of(kLongN), false);
        for (int k = 0; k < kLongN; k++) {
            const size_t j = (size_t)((k + kLongN / 2) % kLongN);
            acc_[j] += (double)re_[(size_t)k] * re_[(size_t)k] + (double)im_[(size_t)k] * im_[(size_t)k];
        }
        accFrames_++;
    }
    // short spectra: does the strongest bin move steadily from one to the next (a swept tone)
    if (a < n) lastStrong_ = false;      // the frames of this block do not follow on from the last block's
    float sre[kShortN], sim[kShortN];
    for (size_t f = 0; f + kShortN <= a; f += kShortN) {
        for (int i = 0; i < kShortN; i++) {
            const cf32 v = (x[f + (size_t)i] - dc) * winS_[(size_t)i];
            sre[i] = v.real(); sim[i] = v.imag();
        }
        fftSplit(sre, sim, log2of(kShortN), false);
        double sum = 0, pk = 0;
        int pi = 0;
        for (int k = 0; k < kShortN; k++) {
            const double p = (double)sre[k] * sre[k] + (double)sim[k] * sim[k];
            sum += p;
            if (p > pk) { pk = p; pi = k; }
        }
        const double others = (sum - pk) / (kShortN - 1);
        const bool strong = others > 0 && pk > 10.0 * others;
        if (strong && lastStrong_) {
            int d = pi - lastPeak_;
            if (d >= kShortN / 2) d -= kShortN;
            if (d < -kShortN / 2) d += kShortN;
            strongPairs_++;
            if (d != 0 && lastDelta_ != 0 && (d > 0) == (lastDelta_ > 0) && std::abs(d - lastDelta_) <= 2) { sweptPairs_++; if (deltas_.size() < 4096) deltas_.push_back(d); }
            lastDelta_ = d;
        } else {
            lastDelta_ = 0;
        }
        lastStrong_ = strong;
        lastPeak_ = pi;
    }
    // pulses: windows of kWin samples far above the median window power of the block
    const size_t nw = n / kWin;
    wpow_.resize(nw);
    double total = 0;
    for (size_t w = 0; w < nw; w++) {
        float s = 0;
        for (int i = 0; i < kWin; i++) { const cf32 v = x[w * kWin + (size_t)i] - dc; s += v.real() * v.real() + v.imag() * v.imag(); }
        wpow_[w] = s / kWin;
        total += s;
    }
    for (size_t i = nw * kWin; i < n; i++) { const cf32 v = x[i] - dc; total += (double)v.real() * v.real() + (double)v.imag() * v.imag(); }
    if (nw > 8) {
        std::vector<float> tmp(wpow_);
        std::nth_element(tmp.begin(), tmp.begin() + (long)(nw / 2), tmp.end());
        const float med = std::max(tmp[nw / 2], 1e-12f), thr = 8.f * med;
        for (size_t w = 0; w < nw; w++) {
            const bool hot = wpow_[w] > thr;
            cf32 lag(0, 0);
            if (hot) for (int i = 1; i < kWin; i++) lag += (x[w * kWin + (size_t)i] - dc) * std::conj(x[w * kWin + (size_t)i - 1] - dc);
            if (hot) {
                hotWins_++;
                pulseLevel_ += wpow_[w] / med;
                if (!inPulse_) { inPulse_ = true; pulses_++; pulseWins_ = 1; pulseFirst_ = lag; }
                else pulseWins_++;
                pulseLast_ = lag;
            } else if (inPulse_) {
                inPulse_ = false;
                if (pulseWins_ >= 2) {
                    pulseMulti_++;
                    const double df = std::arg(pulseLast_ * std::conj(pulseFirst_)) / (2 * kPi);   // cycles per sample
                    if (df > 0.01) pulseSweptUp_++;
                    else if (df < -0.01) pulseSweptDown_++;
                }
            }
        }
        totalWins_ += (int)nw;
    }
    sumPow_ += total; sumN_ += (int64_t)n;
    secSamples_ += (int64_t)n;
    while (perSec_ > 0 && secSamples_ >= perSec_) {
        secSamples_ -= perSec_;
        sigT_ += 1.0;
        endSecond();
    }
}

void GnssInterfMonitor::pushHistory(const GnssInterfPoint& p) {
    if (pendN_ == 0) pend_ = GnssInterfPoint();
    pend_.t = p.t; pend_.unixS = p.unixS;
    pend_.noiseDb += p.noiseDb; pend_.powerDb += p.powerDb; pend_.cn0 += p.cn0; pend_.flatness += p.flatness;
    pend_.tracked = std::max(pend_.tracked, p.tracked);
    pend_.status = std::max(pend_.status, p.status);
    pend_.spoof = std::max(pend_.spoof, p.spoof);
    pendN_++;
    if (pendN_ < (int)std::lround(r_.historyStepS)) return;
    GnssInterfPoint q = pend_;
    q.noiseDb /= (float)pendN_; q.powerDb /= (float)pendN_; q.cn0 /= (float)pendN_; q.flatness /= (float)pendN_;
    pendN_ = 0;
    auto& h = r_.history;
    h.push_back(q);
    if (h.size() >= kMaxHistory) {
        // merge pairs: half as many points, twice as long each
        std::vector<GnssInterfPoint> m;
        for (size_t i = 0; i + 1 < h.size(); i += 2) {
            GnssInterfPoint c = h[i + 1];
            c.noiseDb = 0.5f * (h[i].noiseDb + h[i + 1].noiseDb); c.powerDb = 0.5f * (h[i].powerDb + h[i + 1].powerDb);
            c.cn0 = 0.5f * (h[i].cn0 + h[i + 1].cn0); c.flatness = 0.5f * (h[i].flatness + h[i + 1].flatness);
            c.tracked = std::max(h[i].tracked, h[i + 1].tracked); c.status = std::max(h[i].status, h[i + 1].status); c.spoof = std::max(h[i].spoof, h[i + 1].spoof);
            m.push_back(c);
        }
        if (h.size() % 2) m.push_back(h.back());
        h.swap(m);
        r_.historyStepS *= 2;
    }
}

void GnssInterfMonitor::endSecond() {
    if (accFrames_ == 0 || sumN_ == 0) return;
    const int N = kLongN;
    const double binHz = rate_ / N;
    double w2 = 0;
    for (float w : winL_) w2 += (double)w * w;
    const double norm = 1.0 / ((double)accFrames_ * N * w2);   // the bins of white noise of power s2 then sum to s2
    std::vector<double> P((size_t)N);
    for (int k = 0; k < N; k++) P[(size_t)k] = std::max(acc_[(size_t)k] * norm, 1e-20);
    auto usable = [&](int k) { const int d = std::abs(k - N / 2); return d > 2 && d < (int)(0.42 * N); };
    std::vector<double> u;
    double mean = 0, lmean = 0;
    for (int k = 0; k < N; k++) if (usable(k)) { u.push_back(P[(size_t)k]); mean += P[(size_t)k]; lmean += std::log(P[(size_t)k]); }
    mean /= (double)u.size(); lmean /= (double)u.size();
    const double floor = medianOf(u);
    r_.valid = true;
    r_.spanHz = rate_;
    r_.spectrum.resize((size_t)N);
    for (int k = 0; k < N; k++) r_.spectrum[(size_t)k] = (float)(10 * std::log10(P[(size_t)k] / binHz));
    r_.noiseDb = (float)(10 * std::log10(floor / binHz));
    r_.powerDb = (float)(10 * std::log10(std::max(sumPow_ / (double)sumN_, 1e-20)));
    r_.flatness = (float)(std::exp(lmean) / mean);
    const bool learning = sigT_ <= kLearnSecs;
    r_.learning = learning;
    // narrowband lines: clusters of bins 8 dB above the floor, at most 8 bins wide
    r_.spikes.clear();
    const double thr = floor * std::pow(10.0, 0.8);
    int wideBins = 0;
    for (int k = 0; k < N; ) {
        if (!usable(k) || P[(size_t)k] <= thr) { k++; continue; }
        int e = k, pkI = k;
        while (e + 1 < N && usable(e + 1) && (P[(size_t)e + 1] > thr || (e + 2 < N && P[(size_t)e + 2] > thr && usable(e + 2)))) { e++; if (P[(size_t)e] > P[(size_t)pkI]) pkI = e; }
        const int w = e - k + 1;
        if (w <= 8) {
            GnssInterfSpike s;
            s.offsetHz = (pkI - N / 2) * binHz;
            s.aboveDb = (float)(10 * std::log10(P[(size_t)pkI] / floor));
            s.widthHz = (float)(w * binHz);
            r_.spikes.push_back(s);
        } else {
            wideBins += w;
        }
        k = e + 1;
    }
    for (auto& s : r_.spikes) {
        if (learning && s.aboveDb < 20) { knownSpikes_.push_back(s.offsetHz); s.known = true; continue; }
        for (double f : knownSpikes_) if (std::fabs(f - s.offsetHz) <= 2.5 * binHz) s.known = true;
    }
    std::sort(r_.spikes.begin(), r_.spikes.end(), [](const GnssInterfSpike& a, const GnssInterfSpike& b) { return a.aboveDb > b.aboveDb; });
    if (r_.spikes.size() > kMaxSpikes) r_.spikes.resize(kMaxSpikes);
    // swept tone
    r_.chirpScore = strongPairs_ >= 30 ? (float)sweptPairs_ / (float)strongPairs_ : 0.f;
    r_.chirpRateHzPerUs = 0;
    if (!deltas_.empty()) {
        std::vector<double> d(deltas_.begin(), deltas_.end());
        r_.chirpRateHzPerUs = (float)(medianOf(d) * (rate_ / kShortN) / (kShortN / rate_) * 1e-6);
    }
    const bool sweptPulses = pulseMulti_ >= 10 && std::max(pulseSweptUp_, pulseSweptDown_) >= 0.8 * pulseMulti_;
    const bool chirp = r_.chirpScore >= 0.4f || sweptPulses;
    // pulses
    const double secs = (double)totalWins_ * kWin / rate_;
    r_.pulseRateHz = secs > 0 ? (float)(pulses_ / secs) : 0.f;
    r_.pulseDuty = totalWins_ > 0 ? (float)hotWins_ / (float)totalWins_ : 0.f;
    r_.pulseAboveDb = hotWins_ > 0 ? (float)(10 * std::log10(pulseLevel_ / hotWins_)) : 0.f;
    const bool pulsed = !sweptPulses && pulses_ >= 10 && r_.pulseRateHz >= 20 && r_.pulseDuty < 0.5f;
    // C/N0 of the second
    const float cn0 = cn0Count_ > 0 ? (float)(cn0Sum_ / cn0Count_) : 0.f;
    if (cn0 > 0) { cn0Recent_.push_back(cn0); if (cn0Recent_.size() > 3) cn0Recent_.erase(cn0Recent_.begin()); }
    else cn0Recent_.clear();
    float cur = 0;
    for (float c : cn0Recent_) cur += c;
    if (!cn0Recent_.empty()) cur /= (float)cn0Recent_.size();
    r_.cn0Avg = cn0;
    // the baseline and the status
    unsigned kinds = 0;
    int st = GnssInterfClear;
    std::string why;
    auto add = [&](int s, const std::string& t) { st = std::max(st, s); why += (why.empty() ? "" : "; ") + t; };
    if (learning) {
        learnFloors_.push_back((float)r_.noiseDb);
        std::vector<double> lf(learnFloors_.begin(), learnFloors_.end());
        baseline_ = medianOf(lf);
        r_.baselineDb = (float)baseline_;
        r_.riseDb = 0;
    } else {
        const double rise = r_.noiseDb - baseline_;
        r_.riseDb = (float)rise;
        if (chirp) {
            kinds |= GnssKindChirp;
            if (sweptPulses && r_.chirpScore < 0.4f) add(GnssInterfAlarm, fmt("swept tone through the band (%.0f bursts a second)", r_.pulseRateHz));
            else add(GnssInterfAlarm, fmt("swept tone, %.3f MHz/us", std::fabs(r_.chirpRateHzPerUs) / 1e6));
        }
        if (pulsed) { kinds |= GnssKindPulsed; add(GnssInterfAlarm, fmt("pulses, %.0f a second, %.0f dB above the band", r_.pulseRateHz, r_.pulseAboveDb)); }
        int lines = 0;
        const GnssInterfSpike* top = nullptr;
        for (auto& s : r_.spikes) if (!s.known && s.aboveDb >= 10) { lines++; if (!top) top = &s; }
        if (top && !chirp) {
            kinds |= GnssKindCw;
            add(GnssInterfAlarm, fmt(lines > 1 ? "narrowband line at %+.1f kHz, %.0f dB above the floor (%.0f lines)" : "narrowband line at %+.1f kHz, %.0f dB above the floor", top->offsetHz / 1e3, top->aboveDb, lines));
        }
        if (rise >= 2 && !chirp && !pulsed) {
            kinds |= GnssKindWideband;
            add(rise >= 6 ? GnssInterfAlarm : GnssInterfRaised, fmt(r_.flatness > 0.7f ? "noise floor up %.1f dB, flat across the band (wideband)" : "noise floor up %.1f dB, part of the band", rise));
        }
        if (cn0RefSet_ && cur > 0 && r_.cn0Ref - cur >= 4 && rise >= 1) add(GnssInterfAlarm, fmt("C/N0 down %.1f dB", r_.cn0Ref - cur));
        // the baseline follows a falling floor, and a rising one only very slowly while the band is clear
        if (rise < 0) baseline_ += rise * 0.2;
        else if (st == GnssInterfClear) baseline_ += std::min(rise, 0.01);
        r_.baselineDb = (float)baseline_;
    }
    // C/N0 reference: from clear seconds only
    if (st == GnssInterfClear && prevStatus_ == GnssInterfClear && cur > 0 && cn0Tracked_ >= 3) {
        if (!cn0RefSet_) { r_.cn0Ref = cur; cn0RefSet_ = true; }
        else r_.cn0Ref += (cur - r_.cn0Ref) / 30.f;
    }
    r_.cn0DropDb = cn0RefSet_ && cur > 0 ? r_.cn0Ref - cur : 0.f;
    // hold a status for 3 seconds before it falls back
    if (st < prevStatus_ && clearSecs_ < 3) { clearSecs_++; st = prevStatus_; why = prevReason_; kinds |= prevKinds_; }
    else if (st >= prevStatus_) clearSecs_ = 0;
    r_.kinds = kinds;
    r_.status = st;
    r_.reason = why;
    if (st != prevStatus_) event(st, st == GnssInterfClear ? "clear" : std::string(gnssInterfStatusName(st)) + ": " + why);
    else if (st != GnssInterfClear && (kinds & ~prevKinds_)) event(st, std::string(gnssInterfStatusName(st)) + ": " + why);
    prevStatus_ = st; prevReason_ = why; prevKinds_ = kinds;
    if (!learning && sigT_ - kLearnSecs < 1.5) event(0, fmt("baseline learned: noise floor %.1f dBFS/Hz", baseline_));
    // history
    GnssInterfPoint p;
    p.t = sigT_; p.unixS = now(); p.noiseDb = r_.noiseDb; p.powerDb = r_.powerDb; p.cn0 = cn0; p.flatness = r_.flatness;
    p.tracked = (uint8_t)std::min(cn0Tracked_, 255); p.status = (uint8_t)st; p.spoof = spoofSec_ || r_.spoofing;
    pushHistory(p);
    spoofSec_ = 0;
    // start the next second
    std::fill(acc_.begin(), acc_.end(), 0.0);
    accFrames_ = 0; sumPow_ = 0; sumN_ = 0;
    strongPairs_ = 0; sweptPairs_ = 0; deltas_.clear();
    pulses_ = 0; hotWins_ = 0; totalWins_ = 0; pulseLevel_ = 0; pulseSweptUp_ = 0; pulseSweptDown_ = 0; pulseMulti_ = 0;
    cn0Sum_ = 0; cn0Count_ = 0;
}

void GnssInterfMonitor::update(const GnssTelemetry& t, double almMismatchKm, int almPrn) {
    // C/N0 of the tracked satellites
    double s = 0, s2 = 0;
    int n = 0;
    for (const auto& c : t.channels)
        if (c.state >= GnssChLocked && c.cn0 > 0) { s += c.cn0; s2 += (double)c.cn0 * c.cn0; n++; }
    cn0Tracked_ = n;
    r_.cn0N = n;
    if (n > 0) {
        const double m = s / n;
        cn0Sum_ += m; cn0Count_++;
        r_.cn0Spread = (float)std::sqrt(std::max(0.0, s2 / n - m * m));
    } else {
        r_.cn0Spread = 0;
    }
    r_.cn0Uniform = n >= 5 && r_.cn0Spread < 1.0f;
    const double ts = t.signalSecs;
    // the fix
    const GnssFix& f = t.fix;
    if (f.valid && f.fixCount != lastFixCount_) {
        lastFixCount_ = f.fixCount;
        double bias = 0;
        for (int k = 0; k < GnssSystems; k++) if (f.systemInFix[k]) { bias = f.clockBiasM[k]; break; }
        if (havePrevFix_ && ts - prevFixT_ < 10 && ts > prevFixT_) {
            const double dt = ts - prevFixT_;
            const double d = std::sqrt((f.ecef[0] - prevEcef_[0]) * (f.ecef[0] - prevEcef_[0]) + (f.ecef[1] - prevEcef_[1]) * (f.ecef[1] - prevEcef_[1]) + (f.ecef[2] - prevEcef_[2]) * (f.ecef[2] - prevEcef_[2]));
            const double allow = 100 + 3 * f.speedMps * dt + 5 * (double)f.hErrM;
            if (d > allow) {
                posJumpUntil_ = ts + 60;
                r_.lastJumpM = (float)d;
                event(2, fmt("position jumped %.0f m in %.1f s", d, dt));
            }
            const double db = bias - prevBias_ - 0.5 * (f.clockDriftMps + prevDrift_) * dt;
            const double ms = kC * 1e-3;
            if (std::fabs(db) > 3000 && std::fabs(db - std::round(db / ms) * ms) > 1000) {
                clockJumpUntil_ = ts + 60;
                r_.lastClockJumpM = (float)db;
                event(2, fmt("receiver clock jumped %.1f us", db / kC * 1e6));
            }
        }
        // the long-term average (for a fixed antenna)
        if (nAvg_ >= 30) {
            double d2 = 0;
            for (int k = 0; k < 3; k++) { const double e = f.ecef[k] - sumEcef_[k] / nAvg_; d2 += e * e; }
            r_.distFromAvgM = (float)std::sqrt(d2);
            const bool farBy = r_.distFromAvgM > 300 + 5 * f.hErrM;
            if (farBy && !r_.farFromAverage) { farSince_ = ts; event(2, fmt("position %.0f m from the average of %.0f fixes", r_.distFromAvgM, nAvg_)); }
            r_.farFromAverage = farBy;
            if (farBy && ts - farSince_ > 600) {      // ten minutes away: the antenna has moved, start a new average
                nAvg_ = 0; for (double& v : sumEcef_) v = 0;
                r_.farFromAverage = false;
                event(0, "the position has stayed away for ten minutes: a new average starts");
            }
        }
        if (!r_.farFromAverage && ts >= posJumpUntil_) { for (int k = 0; k < 3; k++) sumEcef_[k] += f.ecef[k]; nAvg_++; }
        r_.avgFixes = nAvg_;
        for (int k = 0; k < 3; k++) prevEcef_[k] = f.ecef[k];
        prevBias_ = bias; prevFixT_ = ts; prevDrift_ = f.clockDriftMps;
        havePrevFix_ = true;
    }
    r_.posJump = ts < posJumpUntil_;
    r_.clockJump = ts < clockJumpUntil_;
    r_.almMismatchKm = (float)std::max(0.0, almMismatchKm);
    r_.almanacMismatch = almMismatchKm > 100;
    std::string why;
    auto add = [&](const std::string& x) { why += (why.empty() ? "" : "; ") + x; };
    if (r_.cn0Uniform) add(fmt("%.0f satellites within %.1f dB of each other", n, r_.cn0Spread));
    if (r_.posJump) add(fmt("position jump of %.0f m", r_.lastJumpM));
    if (r_.clockJump) add(fmt("clock jump of %.1f us", r_.lastClockJumpM / kC * 1e6));
    if (r_.farFromAverage) add(fmt("%.0f m from the long-term position", r_.distFromAvgM));
    if (r_.almanacMismatch) add(gnssSatName(GnssGps, almPrn) + fmt(" ephemeris %.0f km from its almanac", almMismatchKm));
    r_.spoofing = !why.empty();
    r_.spoofReason = why;
    if (r_.spoofing) spoofSec_ = 1;
    if (r_.spoofing != prevSpoof_) event(r_.spoofing ? 2 : 0, r_.spoofing ? "spoofing indicator: " + why : "spoofing indicators clear");
    prevSpoof_ = r_.spoofing;
}

std::string gnssInterfCsv(const GnssInterference& r) {
    std::string o = "signal_s,utc,noise_dbfs_hz,power_dbfs,cn0_avg_dbhz,tracked,flatness,status,spoofing\n";
    char b[256];
    auto utc = [](double unixS, char* out, size_t n) {
        const time_t tt = (time_t)unixS;
        struct tm g;
#ifdef _WIN32
        gmtime_s(&g, &tt);
#else
        gmtime_r(&tt, &g);
#endif
        strftime(out, n, "%Y-%m-%dT%H:%M:%SZ", &g);
    };
    char ts[32];
    for (const auto& p : r.history) {
        utc(p.unixS, ts, sizeof ts);
        snprintf(b, sizeof b, "%.0f,%s,%.2f,%.2f,%.1f,%d,%.3f,%s,%d\n", p.t, ts, p.noiseDb, p.powerDb, p.cn0, p.tracked, p.flatness, gnssInterfStatusName(p.status), p.spoof);
        o += b;
    }
    o += "\nsignal_s,utc,level,event\n";
    for (const auto& e : r.events) {
        utc(e.unixS, ts, sizeof ts);
        std::string text = e.text;
        for (auto& c : text) if (c == '"') c = '\'';
        snprintf(b, sizeof b, "%.0f,%s,%s,\"", e.t, ts, e.level == 2 ? "alarm" : e.level == 1 ? "warning" : "info");
        o += b; o += text; o += "\"\n";
    }
    return o;
}

} // namespace dect2
