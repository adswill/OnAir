// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/specx/specx_rx.h"
#include "dect2/specx/specx_tel.h"
#include "dect2/dsp/fftutil.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <vector>

namespace dect2 {

struct SpecxReceiver::Impl {
    std::mutex mu;
    std::function<void(const std::string&)> cb;
    double rate = 0;
    std::atomic<int> fftReq{4096}, avgReq{4};
    std::atomic<bool> resetReq{false};
    int N = 0, avg = 4;
    std::unique_ptr<Fft> fft;
    std::vector<float> win;
    double wsum2 = 1, enbw = 1;
    std::vector<cf32> buf;
    std::vector<double> acc;   // averaged power per bin (fft order)
    uint64_t frames = 0, inSamples = 0, skip = 0, nextPub = 0;
    double power = 0; uint64_t nPower = 0;
    SpecxTelemetry pub;

    void setup() {
        N = fftReq.load(); avg = avgReq.load();
        fft = std::make_unique<Fft>(N);
        win.resize((size_t)N);
        double s = 0, s2 = 0;
        for (int i = 0; i < N; i++) {   // 4-term Blackman-Harris
            const double t = 2 * M_PI * i / N;
            win[(size_t)i] = (float)(0.35875 - 0.48829 * std::cos(t) + 0.14128 * std::cos(2 * t) - 0.01168 * std::cos(3 * t));
            s += win[(size_t)i]; s2 += (double)win[(size_t)i] * win[(size_t)i];
        }
        wsum2 = s * s; enbw = N * s2 / (s * s);
        buf.clear(); acc.assign((size_t)N, 0.0); frames = 0; skip = 0; nextPub = 0; inSamples = 0; power = 0; nPower = 0;
    }

    void frame() {
        std::vector<cf32> x(buf.begin(), buf.begin() + N);
        for (int i = 0; i < N; i++) x[(size_t)i] *= win[(size_t)i];
        fft->forward(x.data());
        const double a = frames == 0 ? 1.0 : 1.0 / std::min<uint64_t>((uint64_t)avg, frames + 1);
        for (int i = 0; i < N; i++) acc[(size_t)i] += a * (std::norm(x[(size_t)i]) / wsum2 - acc[(size_t)i]);
        frames++;
    }

    void publish() {
        std::vector<float> s((size_t)N);
        int pk = 0;
        for (int i = 0; i < N; i++) {
            const double p = acc[(size_t)((i + N / 2) % N)];
            s[(size_t)i] = (float)(10 * std::log10(std::max(1e-20, p)));
            if (s[(size_t)i] > s[(size_t)pk]) pk = i;
        }
        std::vector<float> m(s);
        std::nth_element(m.begin(), m.begin() + N / 2, m.end());
        std::lock_guard<std::mutex> lk(mu);
        pub.seq++;
        pub.state = 1;
        pub.rate = rate; pub.fftSize = N; pub.enbw = enbw; pub.averaging = avg; pub.frames = frames;
        pub.floorDb = m[(size_t)N / 2]; pub.peakDb = s[(size_t)pk];
        pub.peakOffsetHz = (pk - N / 2) * rate / N;
        pub.powerDb = nPower ? 10 * std::log10(std::max(1e-20, power / nPower)) : -200;
        pub.spectrum = std::move(s);
        power = 0; nPower = 0;
    }
};

SpecxReceiver::SpecxReceiver() : p_(new Impl) {}
SpecxReceiver::~SpecxReceiver() = default;

void SpecxReceiver::configure(double r) {
    { std::lock_guard<std::mutex> lk(p_->mu); p_->rate = r; p_->pub = SpecxTelemetry(); }
    p_->resetReq = true;
    if (p_->cb && r > 0) { char b[96]; snprintf(b, sizeof b, "Spectrum explorer: %.3f Msps, no decoder", r / 1e6); p_->cb(b); }
}
bool SpecxReceiver::ready() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->rate > 0; }
void SpecxReceiver::reset() { p_->resetReq = true; }
void SpecxReceiver::setLogCallback(std::function<void(const std::string&)> cb) { p_->cb = std::move(cb); }
void SpecxReceiver::setFftSize(int n) {
    int k = 256;
    while (k < n && k < 65536) k <<= 1;
    if (k != p_->fftReq.load()) { p_->fftReq = k; p_->resetReq = true; }
}
void SpecxReceiver::setAveraging(int n) { p_->avgReq = std::max(1, std::min(100, n)); }

void SpecxReceiver::feed(const cf32* x, size_t n) {
    Impl& m = *p_;
    if (m.resetReq.exchange(false) || !m.fft) m.setup();
    m.avg = m.avgReq.load();
    if (m.rate <= 0) return;
    // at most about 4 million samples a second go through the FFT; the rest is skipped between frames
    const uint64_t gap = m.rate > 4e6 ? (uint64_t)((double)m.N * (m.rate / 4e6 - 1)) : 0;
    for (size_t i = 0; i < n; i++) {
        const cf32 v = x[i];
        if (!std::isfinite(v.real()) || !std::isfinite(v.imag())) continue;
        m.power += std::norm(v); m.nPower++;
        m.inSamples++;
        if (m.skip) { m.skip--; continue; }
        m.buf.push_back(v);
        if ((int)m.buf.size() >= m.N) { m.frame(); m.buf.clear(); m.skip = gap; }
    }
    if (m.frames && m.inSamples >= m.nextPub) { m.nextPub = m.inSamples + (uint64_t)(m.rate / 25); m.publish(); }
}

bool SpecxReceiver::telemetry(SpecxTelemetry& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (p_->pub.seq == lastSeq) return false;
    out = p_->pub;
    return true;
}

ModeTuning specxTuning() {
    ModeTuning t;
    t.stdMode = 43; t.id = "specx"; t.name = "Spectrum explorer";
    t.minMhz = 0.1; t.maxMhz = 7250; t.defMhz = 433.92;
    t.sampleRate = 10e6;          // HackRF Pro: 10 Msps tunes cleanly
    t.basebandHz = 0;
    t.bandwidthMhz = 10;
    t.minSampleRate = 250e3;
    return t;
}

std::string specxSummary(const SpecxTelemetry& t) {
    char b[160];
    if (t.state == 0) return "spectrum: waiting for samples";
    snprintf(b, sizeof b, "spectrum %.2f Msps, %d bins (RBW %.1f Hz), floor %.1f dBFS, peak %.1f dBFS at %+.1f kHz", t.rate / 1e6, t.fftSize,
             t.fftSize ? t.rate / t.fftSize : 0.0, t.floorDb, t.peakDb, t.peakOffsetHz / 1e3);
    return b;
}

} // namespace dect2
