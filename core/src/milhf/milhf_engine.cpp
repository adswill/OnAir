// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// HF waveform identifier in the engine: dial to 0 Hz, two-stage decimation to about 12 kHz, real audio (upper sideband) into the analyser.
#include "dect2/milhf/milhf_engine.h"
#include "dect2/wb/wb_export.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <mutex>

namespace dect2 {

namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr double kMid = 48000.0, kAudio = 12000.0;
constexpr size_t kKeep = 131072, kClipMax = 20 * 12500;
}

struct MilhfReceiver::Impl {
    mutable std::mutex mu;
    double rate = 0, offsetHz = 0, centerHz = 0, phase = 0;
    int dec1 = 1, dec2 = 1;
    double fsMid = 0, fsA = 0;
    std::complex<float> acc{0, 0};
    int accN = 0;
    std::vector<float> fir;                       // stage-2 low pass
    std::vector<std::complex<float>> hist;        // last taps samples of stage 1
    int dcount = 0;
    std::vector<float> aud, clip;
    size_t sinceAnalysis = 0, sinceRow = 0;
    uint64_t seq = 0;
    MilhfTelemetry snap;
    std::function<void(const std::string&)> log;

    void clearState() {
        acc = {0, 0}; accN = 0; phase = 0; dcount = 0;
        hist.assign(fir.size(), std::complex<float>(0, 0));
        aud.clear(); clip.clear(); sinceAnalysis = sinceRow = 0;
        snap = MilhfTelemetry();
        snap.seq = seq;
        snap.inputRate = rate;
        snap.centerHz = centerHz;
        snap.audioRate = fsA;
    }

    void row() {
        const size_t need = 2048;
        if (aud.size() < need) return;
        std::vector<float> db;
        milhf::psdDb(aud.data() + (aud.size() - need), need, fsA, 512, db);
        if (db.empty()) return;
        if ((int)snap.rows >= kMilhfRows) {
            snap.wf.erase(snap.wf.begin(), snap.wf.begin() + kMilhfCols);
            snap.rows--;
        }
        const double binHz = fsA / 512.0;
        for (int c = 0; c < kMilhfCols; c++) {
            const size_t k = std::min(db.size() - 1, (size_t)std::lround(c * 3600.0 / (kMilhfCols - 1) / binHz));
            snap.wf.push_back((uint8_t)std::clamp((db[k] + 120.0) * 1.5, 0.0, 255.0));
        }
        snap.rows++;
    }

    void analyse() {
        const size_t n = std::min<size_t>(aud.size(), 65536);
        snap.res = milhf::analyze(aud.data() + (aud.size() - n), n, fsA);
        snap.analysed = true;
        snap.dataValid = snap.res.f.signal;
        snap.snrDb = (float)snap.res.f.snrDb;
        if (snap.res.f.signal) snap.blocksOk++;   // an analysis of noise is not a block
        snap.state = 2;
        double e = 0;
        const size_t m = std::min<size_t>(aud.size(), 12000);
        for (size_t i = aud.size() - m; i < aud.size(); i++) e += (double)aud[i] * aud[i];
        snap.levelDb = (float)(10 * std::log10(e / std::max<size_t>(1, m) + 1e-20));
        snap.audioSecs = (double)aud.size() / fsA;
        if (log && !snap.res.cands.empty()) {
            char b[160];
            std::snprintf(b, sizeof b, "HF ID: best estimate %s (%.0f %%), identification only", snap.res.cands[0].name.c_str(), 100.0 * snap.res.cands[0].conf);
            log(b);
        }
    }

    void audioSample(float a) {
        aud.push_back(a);
        clip.push_back(a);
        if (aud.size() > kKeep) aud.erase(aud.begin(), aud.begin() + (aud.size() - 65536));
        const size_t cmax = (size_t)(20.0 * fsA);
        if (clip.size() > cmax * 2) clip.erase(clip.begin(), clip.begin() + (clip.size() - cmax));
        snap.state = std::max(snap.state, 1);
        if (++sinceRow >= (size_t)(0.2 * fsA)) { sinceRow = 0; row(); snap.seq = ++seq; }
        if (++sinceAnalysis >= (size_t)(2.0 * fsA) && aud.size() >= 16384) { sinceAnalysis = 0; analyse(); snap.seq = ++seq; }
    }
};

MilhfReceiver::MilhfReceiver() : p_(new Impl) {}
MilhfReceiver::~MilhfReceiver() = default;

void MilhfReceiver::configure(double inputRateHz) {
    std::lock_guard<std::mutex> l(p_->mu);
    Impl& d = *p_;
    d.rate = inputRateHz;
    d.dec1 = inputRateHz > kMid ? (int)std::lround(inputRateHz / kMid) : 1;
    d.fsMid = inputRateHz / d.dec1;
    d.dec2 = std::max(1, (int)std::lround(d.fsMid / kAudio));
    d.fsA = d.fsMid / d.dec2;
    // 63-tap windowed-sinc low pass at 5.2 kHz (stage 2)
    const int taps = 63;
    d.fir.assign(taps, 0.f);
    const double fc = std::min(5200.0, d.fsA * 0.43) / d.fsMid;
    double sum = 0;
    for (int i = 0; i < taps; i++) {
        const double x = i - (taps - 1) / 2.0;
        const double s = x == 0 ? 2 * fc : std::sin(2 * kPi * fc * x) / (kPi * x);
        const double w = 0.54 - 0.46 * std::cos(2 * kPi * i / (taps - 1));
        d.fir[i] = (float)(s * w);
        sum += d.fir[i];
    }
    for (auto& v : d.fir) v = (float)(v / sum);
    d.clearState();
}

void MilhfReceiver::setSignalOffset(double hz) { std::lock_guard<std::mutex> l(p_->mu); p_->offsetHz = hz; }
void MilhfReceiver::setCenterHz(double hz) { std::lock_guard<std::mutex> l(p_->mu); p_->centerHz = hz; p_->snap.centerHz = hz; }

bool MilhfReceiver::ready() const {
    std::lock_guard<std::mutex> l(p_->mu);
    return p_->rate >= milhfTuning().minSampleRate && p_->fsA >= 8000.0;
}

void MilhfReceiver::reset() { std::lock_guard<std::mutex> l(p_->mu); p_->clearState(); }

void MilhfReceiver::feed(const std::complex<float>* x, size_t n) {
    std::lock_guard<std::mutex> l(p_->mu);
    Impl& d = *p_;
    if (d.fsA <= 0) return;
    const double step = -2.0 * kPi * d.offsetHz / d.rate;
    for (size_t i = 0; i < n; i++) {
        d.acc += x[i] * std::complex<float>((float)std::cos(d.phase), (float)std::sin(d.phase));
        d.phase += step;
        if (d.phase > kPi) d.phase -= 2 * kPi;
        if (d.phase < -kPi) d.phase += 2 * kPi;
        if (++d.accN < d.dec1) continue;
        const std::complex<float> m = d.acc / (float)d.dec1;
        d.acc = {0, 0};
        d.accN = 0;
        d.hist.erase(d.hist.begin());
        d.hist.push_back(m);
        if (++d.dcount < d.dec2) continue;
        d.dcount = 0;
        std::complex<float> o(0, 0);
        for (size_t k = 0; k < d.fir.size(); k++) o += d.hist[k] * d.fir[d.fir.size() - 1 - k];
        d.audioSample(2.0f * o.real());
    }
}

bool MilhfReceiver::telemetry(MilhfTelemetry& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> l(p_->mu);
    if (p_->snap.seq == 0 || p_->snap.seq == lastSeq) return false;
    out = p_->snap;
    out.inputRate = p_->rate;
    out.centerHz = p_->centerHz;
    return true;
}

void MilhfReceiver::setLogCallback(std::function<void(const std::string&)> cb) { std::lock_guard<std::mutex> l(p_->mu); p_->log = std::move(cb); }

bool MilhfReceiver::saveClip(const std::string& path, std::string& err) {
    std::vector<float> a;
    int rate = 0;
    {
        std::lock_guard<std::mutex> l(p_->mu);
        const size_t cmax = (size_t)(20.0 * p_->fsA);
        const size_t n = std::min(p_->clip.size(), cmax);
        a.assign(p_->clip.end() - (long)n, p_->clip.end());
        rate = (int)std::lround(p_->fsA);
    }
    if (a.size() < (size_t)rate) { err = "less than a second of audio so far"; return false; }
    float pk = 1e-9f;
    for (float v : a) pk = std::max(pk, std::fabs(v));
    for (auto& v : a) v *= 0.9f / pk;
    return wb::writeWavMono16(path, a, rate, err);
}

ModeTuning milhfTuning() {
    ModeTuning t;
    t.stdMode = 147;
    t.id = "milhf";
    t.name = "HF waveform ID";
    t.minMhz = 1.8;
    t.maxMhz = 30;
    t.defMhz = 4.625;      // the Buzzer's channel, a fixed public example
    t.sampleRate = 2000000;
    t.basebandHz = 1750000;
    t.bandwidthMhz = 0.006;
    t.minSampleRate = 250000;
    t.tuneOffsetHz = 20000;     // as PSK and MT63: the radio sits 20 kHz above the dial, away from its DC spike
    return t;
}

std::string milhfSummary(const MilhfTelemetry& t) {
    char b[200];
    if (t.state == 0) return "HF ID: waiting for samples";
    if (!t.analysed) return "HF ID: collecting audio";
    if (!t.res.f.signal) {
        snprintf(b, sizeof b, "HF ID: no signal (%.0f dBFS)", t.levelDb);
        return b;
    }
    if (t.res.cands.empty()) return "HF ID: signal, no estimate";
    snprintf(b, sizeof b, "HF ID (estimate): %s, %.0f %%", t.res.cands[0].name.c_str(), 100.0 * t.res.cands[0].conf);
    return b;
}

}  // namespace dect2
