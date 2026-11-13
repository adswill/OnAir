// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/astro/astro_rx.h"
#include "dect2/astro/astro_tel.h"
#include "dect2/astro/astro_dsp.h"
#include "dect2/dsp/fftutil.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <vector>

namespace dect2 {

struct AstroReceiver::Impl {
    std::mutex mu;
    std::function<void(const std::string&)> cb;
    std::function<bool(double)> retune;
    double rate = 0;
    std::atomic<int> fftReq{4096};
    std::atomic<double> intReq{10.0};
    std::atomic<bool> resetReq{false}, swReq{false};
    std::atomic<double> swHzReq{1e6}, swPeriodReq{1.0};
    int N = 0;
    std::unique_ptr<Fft> fft;
    std::vector<float> win;
    double wsum2 = 1;
    std::vector<cf32> buf, tmp;
    std::vector<double> acc[2], last[2];   // phase 0 = the radio's frequency, 1 = shifted (frequency switching)
    uint64_t frames[2] = {0, 0}, completed = 0, nextPub = 0, inSamples = 0, phaseLeft = 0, settle = 0;
    int phase = 0;
    bool sw = false;
    double swHz = 0;
    double power = 0; uint64_t nPower = 0;
    AstroTelemetry pub;

    double doneSec() const { return rate > 0 ? (double)(frames[0] + frames[1]) * N / rate : 0; }

    void setup() {
        N = fftReq.load();
        fft = std::make_unique<Fft>(N);
        win.resize((size_t)N);
        double s = 0;
        for (int i = 0; i < N; i++) { win[(size_t)i] = (float)(0.5 - 0.5 * std::cos(2 * M_PI * i / N)); s += win[(size_t)i]; }
        wsum2 = s * s;
        buf.clear(); tmp.resize((size_t)N);
        for (int k = 0; k < 2; k++) { acc[k].assign((size_t)N, 0.0); last[k].clear(); frames[k] = 0; }
        completed = 0; nextPub = 0; inSamples = 0; power = 0; nPower = 0;
        sw = swReq.load() && retune; swHz = swHzReq.load();
        if (phase != 0 && retune) retune(0.0);
        phase = 0;
        phaseLeft = sw ? (uint64_t)(swPeriodReq.load() * rate) : 0;
        settle = 0;
    }

    void frame() {
        for (int i = 0; i < N; i++) tmp[(size_t)i] = buf[(size_t)i] * win[(size_t)i];
        fft->forward(tmp.data());
        std::vector<double>& a = acc[phase];
        for (int i = 0; i < N; i++) a[(size_t)i] += std::norm(tmp[(size_t)((i + N / 2) % N)]) / wsum2;   // centre bin in the middle
        frames[phase]++;
        if (doneSec() >= intReq.load() && frames[0] > 0 && (!sw || frames[1] > 0)) {   // integration done: keep it, start the next one
            for (int k = 0; k < 2; k++) {
                last[k].assign((size_t)N, 0.0);
                if (frames[k]) for (int i = 0; i < N; i++) last[k][(size_t)i] = acc[k][(size_t)i] / (double)frames[k];
                if (k == 1 && !sw) last[k].clear();
                acc[k].assign((size_t)N, 0.0); frames[k] = 0;
            }
            completed++;
            if (cb) { char b[96]; snprintf(b, sizeof b, "Radio astronomy: integration %llu done (%.0f s)", (unsigned long long)completed, intReq.load()); cb(b); }
        }
    }

    void publish() {
        AstroTelemetry t;
        t.state = 1;
        t.rate = rate; t.fftSize = N; t.integrationSec = intReq.load(); t.doneSec = doneSec();
        t.progress = (float)std::min(1.0, t.doneSec / std::max(1e-9, t.integrationSec));
        t.frames = frames[0] + frames[1]; t.completed = completed;
        auto mean = [&](int k) { std::vector<double> v((size_t)N, 0.0); if (frames[k]) for (int i = 0; i < N; i++) v[(size_t)i] = acc[k][(size_t)i] / (double)frames[k]; return v; };
        t.avg = frames[0] ? mean(0) : last[0];
        t.last = last[0];
        t.switching = sw; t.switchHz = sw ? swHz : 0;
        if (sw) { t.avgOff = frames[1] ? mean(1) : last[1]; t.lastOff = last[1]; }
        t.powerDb = nPower ? 10 * std::log10(std::max(1e-20, power / nPower)) : -200;
        power = 0; nPower = 0;
        std::lock_guard<std::mutex> lk(mu);
        t.seq = pub.seq + 1;
        pub = std::move(t);
    }
};

AstroReceiver::AstroReceiver() : p_(new Impl) {}
AstroReceiver::~AstroReceiver() = default;

void AstroReceiver::configure(double r) {
    { std::lock_guard<std::mutex> lk(p_->mu); p_->rate = r; const uint64_t s = p_->pub.seq; p_->pub = AstroTelemetry(); p_->pub.seq = s; }
    p_->resetReq = true;
    if (p_->cb && r > 0) { char b[96]; snprintf(b, sizeof b, "Radio astronomy: %.3f Msps, integrating the spectrum", r / 1e6); p_->cb(b); }
}
bool AstroReceiver::ready() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->rate > 0; }
void AstroReceiver::reset() { p_->resetReq = true; }
void AstroReceiver::setLogCallback(std::function<void(const std::string&)> cb) { p_->cb = std::move(cb); }
void AstroReceiver::setRetuneCallback(std::function<bool(double)> cb) { p_->retune = std::move(cb); p_->resetReq = true; }
void AstroReceiver::setFftSize(int n) {
    int k = 1024;
    while (k < n && k < 16384) k <<= 1;
    if (k != p_->fftReq.load()) { p_->fftReq = k; p_->resetReq = true; }
}
void AstroReceiver::setIntegration(double sec) { p_->intReq = std::clamp(sec, 0.01, 3600.0); }
void AstroReceiver::setFreqSwitch(bool on, double hz, double period) {
    if (on != p_->swReq.load() || hz != p_->swHzReq.load() || period != p_->swPeriodReq.load()) {
        p_->swReq = on; p_->swHzReq = hz; p_->swPeriodReq = std::max(0.05, period); p_->resetReq = true;
    }
}

void AstroReceiver::feed(const cf32* x, size_t n) {
    Impl& m = *p_;
    if (m.rate <= 0) return;
    if (m.resetReq.exchange(false) || !m.fft) m.setup();
    for (size_t i = 0; i < n; i++) {
        const cf32 v = x[i];
        m.inSamples++;
        if (m.sw) {
            if (m.phaseLeft == 0) {   // switch phase: retune, drop the partial frame and the settling samples
                m.phase ^= 1;
                m.retune(m.phase ? m.swHz : 0.0);
                m.buf.clear();
                m.settle = (uint64_t)(0.02 * m.rate);
                m.phaseLeft = (uint64_t)(m.swPeriodReq.load() * m.rate);
            }
            m.phaseLeft--;
            if (m.settle) { m.settle--; continue; }
        }
        if (!std::isfinite(v.real()) || !std::isfinite(v.imag())) continue;
        m.power += std::norm(v); m.nPower++;
        m.buf.push_back(v);
        if ((int)m.buf.size() >= m.N) { m.frame(); m.buf.clear(); }
    }
    if (m.inSamples >= m.nextPub && (m.frames[0] + m.frames[1] || m.completed)) { m.nextPub = m.inSamples + (uint64_t)(m.rate / 8); m.publish(); }
}

bool AstroReceiver::telemetry(AstroTelemetry& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (p_->pub.seq == lastSeq) return false;
    out = p_->pub;
    return true;
}

ModeTuning astroTuning() {
    ModeTuning t;
    t.stdMode = 50; t.id = "astro"; t.name = "Radio astronomy";
    t.minMhz = 10; t.maxMhz = 6000; t.defMhz = astro::kHiHz / 1e6;
    t.sampleRate = 2.5e6;         // +-1.25 MHz around H I = +-260 km/s
    t.basebandHz = 0;
    t.bandwidthMhz = 2.5;
    t.minSampleRate = 250e3;
    t.carrierAtCentre = true;     // the line can sit on the centre: the tool bridges the DC spike itself
    return t;
}

std::string astroSummary(const AstroTelemetry& t) {
    char b[200];
    if (t.state == 0) return "radio astronomy: waiting for samples";
    snprintf(b, sizeof b, "integrating %.2f Msps, %d bins (%.0f Hz), %.0f of %.0f s (%.0f%%), %llu done, %.1f dBFS%s", t.rate / 1e6, t.fftSize,
             t.fftSize ? t.rate / t.fftSize : 0.0, t.doneSec, t.integrationSec, 100.0 * t.progress, (unsigned long long)t.completed, t.powerDb,
             t.switching ? ", frequency switching" : "");
    return b;
}

} // namespace dect2
