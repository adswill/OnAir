// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Robust Packet (RPR) receiver (see rpr_rx.h).
#include "dect2/rpr/rpr_rx.h"
#include "dect2/rpr/rpr_dsp.h"
#include "dect2/rpr/rpr_tel.h"
#include "dect2/marine/marine_dsp.h"
#include "../audio/ham_listen.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>

namespace dect2 {

namespace {
constexpr double kFs = 8000;
constexpr size_t kBufSec = 30;
double nowUtc() { return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count(); }
}

struct RprReceiver::Impl {
    std::mutex mu;
    std::function<void(const std::string&)> log;
    double rate = 0, offsetHz = 0;
    RprTelemetry pub;
    std::atomic<bool> resetReq{true}, offsetReq{false}, clearReq{false};
    // feed thread
    double curRate = 0;
    bool running = false;
    marine::Front front;
    hfdig::UsbAudio usb;
    std::vector<cf32> base;
    std::vector<float> aud;
    std::vector<float> buf;                 // last kBufSec seconds of audio (ring)
    size_t wr = 0; int64_t na = 0;
    double secPow = 0; int secN = 0;
    std::vector<float> levelHist;
    int64_t nIn = 0;
    double nextReport = 0, nextAnalysis = 0, analysisEvery = 4.0;
    double t0 = 0;
    // worker
    std::thread worker;
    std::mutex wm;
    std::condition_variable wcv;
    bool job = false, stop = false, busy = false;
    std::vector<float> jobAudio;
    double jobTime = 0;
    // results (under mu)
    rpr::Analysis last;
    bool haveLast = false;
    std::vector<RprEvent> events;
    uint64_t analyses = 0, detections = 0, copiesTotal = 0;
    std::vector<RprBurst> bursts;
    uint64_t burstsTotal = 0;
    double lastBurstAbs = -1e9;
    bool sawComb = false;
    bool changed = true;
    double lastSigTime = -1e9;

    Impl() {
        buf.assign((size_t)(kBufSec * kFs), 0.f);
        worker = std::thread([this] { run(); });
    }
    ~Impl() {
        { std::lock_guard<std::mutex> lk(wm); stop = true; }
        wcv.notify_all();
        if (worker.joinable()) worker.join();
    }

    void say(const std::string& s) {
        std::function<void(const std::string&)> cb;
        { std::lock_guard<std::mutex> lk(mu); cb = log; }
        if (cb) cb(s);
    }

    void addEvent(const std::string& s, double sigTime) {
        RprEvent e;
        e.utc = (int64_t)(t0 + sigTime); e.timeSec = sigTime; e.text = s;
        events.insert(events.begin(), e);
        if (events.size() > kRprEventsMax) events.pop_back();
        changed = true;
    }

    void resetState() {
        double off;
        std::function<void(const std::string&)> cb;
        { std::lock_guard<std::mutex> lk(mu); curRate = rate; off = offsetHz; cb = log; }
        running = curRate >= rprTuning().minSampleRate - 1;
        if (running) { front.configure(curRate); front.setOffsetHz(off); }
        usb.reset();
        std::fill(buf.begin(), buf.end(), 0.f);
        wr = 0; na = 0; secPow = 0; secN = 0;
        nIn = 0; nextReport = 0; nextAnalysis = analysisEvery * kFs;
        t0 = nowUtc();
        {
            std::lock_guard<std::mutex> lk(mu);
            pub.inputRate = curRate; haveLast = false; sawComb = false; lastBurstAbs = -1e9;
        }
        changed = true;
        if (cb && curRate > 0) {
            cb(running ? "Robust Packet: 8 OFDM carriers 60 Hz apart around 1500 Hz, 50 baud, D-BPSK / D-QPSK, upper sideband (physical layer only: the coding is not public, needs a recording)" : "Robust Packet: the sample rate is too low (250 kHz at least)");
        }
    }

    // ---------------------------------------------------------------- worker
    void run() {
        for (;;) {
            std::vector<float> a;
            double tt = 0;
            {
                std::unique_lock<std::mutex> lk(wm);
                wcv.wait(lk, [&] { return job || stop; });
                if (stop) return;
                a.swap(jobAudio); tt = jobTime; job = false; busy = true;
            }
            rpr::Analysis r = rpr::analyse(a.data(), a.size());
            {
                std::lock_guard<std::mutex> lk(mu);
                last = std::move(r); haveLast = true; analyses++;
                const auto& L = last;
                if (L.found) {
                    detections++;
                    const double t00 = tt - (double)a.size() / kFs;
                    for (const auto& bu : L.bursts) {
                        const double abs = t00 + bu.startSec;
                        if (abs <= lastBurstAbs + 0.3) continue;
                        lastBurstAbs = abs;
                        RprBurst rb;
                        rb.utc = (int64_t)(t0 + abs); rb.timeSec = abs; rb.lenSec = bu.lenSec; rb.snrDb = bu.snrDb;
                        rb.level = bu.level; rb.order = bu.order; rb.nSyms = bu.nSyms;
                        // the first demodulated bits (differential steps, symbol by symbol over the carriers): coded and scrambled, shown as hex
                        std::string bits; unsigned acc = 0; int nb = 0;
                        const int bps = rpr::bitsOf(bu.order);
                        for (size_t m = 0; m < bu.sym[0].size() && m < 24; m++)
                            for (int k = 0; k < rpr::kCarriers; k++)
                                for (int q = bps - 1; q >= 0; q--) { acc = (acc << 1) | ((bu.sym[(size_t)k][m] >> q) & 1u); if (++nb == 4) { bits.push_back("0123456789abcdef"[acc & 15]); acc = 0; nb = 0; } }
                        rb.bits = bits;
                        bursts.insert(bursts.begin(), rb);
                        if (bursts.size() > kRprBurstsMax) bursts.pop_back();
                        burstsTotal++;
                        char b[200];
                        snprintf(b, sizeof b, "burst %.2f s, speed level %d (%s), %d symbols per carrier, %.0f dB", bu.lenSec, bu.level, bu.level == 1 ? "D-BPSK" : "D-QPSK", bu.nSyms, bu.snrDb);
                        addEvent(b, abs);
                    }
                    if (!sawComb) { char b[160]; snprintf(b, sizeof b, "Robust Packet comb found: centre %.0f Hz (%+.0f Hz from 1500)", L.centreHz, L.centreHz - 1500.0); addEvent(b, tt); sawComb = true; }
                    lastSigTime = tt;
                } else if (sawComb && tt - lastSigTime > 20) {
                    addEvent("signal gone", tt);
                    sawComb = false;
                }
                changed = true;
            }
            { std::lock_guard<std::mutex> lk(wm); busy = false; }
            wcv.notify_all();
        }
    }

    void submit() {
        std::lock_guard<std::mutex> lk(wm);
        if (job || busy) return;
        const size_t have = (size_t)std::min<int64_t>(na, (int64_t)buf.size());
        if (have < (size_t)(3 * kFs)) return;
        jobAudio.resize(have);
        for (size_t i = 0; i < have; i++) jobAudio[i] = buf[(wr + buf.size() - have + i) % buf.size()];
        jobTime = (double)na / kFs;
        job = true;
        wcv.notify_all();
    }

    // ---------------------------------------------------------------- audio
    void process(const float* x, size_t n) {
        for (size_t i = 0; i < n; i++) {
            buf[wr] = x[i];
            wr = (wr + 1) % buf.size();
            na++;
            secPow += (double)x[i] * x[i]; secN++;
            if (secN >= 8000) {
                levelHist.push_back((float)(10 * std::log10(secPow / secN + 1e-12)));
                if (levelHist.size() > kRprHist) levelHist.erase(levelHist.begin());
                secPow = 0; secN = 0; changed = true;
            }
        }
        if ((double)na >= nextAnalysis) {
            nextAnalysis = (double)na + analysisEvery * kFs;
            submit();
        }
    }

    void report() {
        std::lock_guard<std::mutex> lk(mu);
        if (!changed && pub.seq > 0) return;
        RprTelemetry& p = pub;
        p.seq++;
        p.timeSec = (double)na / kFs;
        p.inputRate = curRate;
        p.levelDb = levelHist.empty() ? -200.f : levelHist.back();
        p.levelHist = levelHist;
        p.analyses = analyses; p.detections = detections;
        p.events = events; p.bursts = bursts;
        p.blocksOk = burstsTotal;
        p.rows.clear();
        if (haveLast && !last.carrierHz.empty() && last.combDb > 0) {
            const auto& L = last;
            p.state = burstsTotal ? 2 : 1;
            p.centreHz = L.centreHz; p.combDb = L.combDb; p.carrierHz = L.carrierHz; p.cfoHz = L.centreHz - 1500.0;
            p.carriersSeen = L.carriersSeen; p.snrDb = (float)L.snrDb;
            p.level = L.bursts.empty() ? 0 : L.bursts.back().level;
            p.dataValid = !L.bursts.empty() || burstsTotal > 0;
            if (!L.bursts.empty()) {
                static const char* kDig = "0123456789abcdef";
                const auto& bu = L.bursts.back();
                for (size_t c = 0; c < bu.sym.size() && c < 3; c++) {
                    std::string row;
                    const size_t from = bu.sym[c].size() > kRprRowSyms ? bu.sym[c].size() - kRprRowSyms : 0;
                    for (size_t i = from; i < bu.sym[c].size(); i++) row.push_back(kDig[bu.sym[c][i] & 15]);
                    p.rows.push_back(row);
                }
            }
        } else {
            p.state = 0; p.dataValid = false; p.carrierHz.clear(); p.snrDb = 0; p.cfoHz = 0; p.level = 0; p.carriersSeen = 0; p.combDb = 0;
        }
        changed = false;
    }
};

RprReceiver::RprReceiver() : p_(std::make_unique<Impl>()) {}
RprReceiver::~RprReceiver() = default;

void RprReceiver::configure(double r) { std::lock_guard<std::mutex> lk(p_->mu); p_->rate = r; p_->resetReq = true; }
void RprReceiver::setSignalOffset(double hz) { std::lock_guard<std::mutex> lk(p_->mu); p_->offsetHz = hz; p_->offsetReq = true; }
bool RprReceiver::ready() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->rate >= rprTuning().minSampleRate - 1; }
void RprReceiver::reset() { p_->resetReq = true; }
void RprReceiver::setAnalysisInterval(double sec) { p_->analysisEvery = std::max(0.5, sec); }

void RprReceiver::feed(const cf32* x, size_t n) {
    Impl& m = *p_;
    if (m.resetReq.exchange(false)) { m.offsetReq = false; m.resetState(); }
    if (m.offsetReq.exchange(false) && m.running) { std::lock_guard<std::mutex> lk(m.mu); m.front.setOffsetHz(m.offsetHz); }
    if (m.clearReq.exchange(false)) { m.events.clear(); m.bursts.clear(); m.burstsTotal = 0; m.haveLast = false; m.sawComb = false; m.changed = true; }
    if (m.curRate <= 0) return;
    m.nIn += (int64_t)n;
    if (m.running) {
        m.base.clear();
        m.front.process(x, n, m.base);
        for (cf32& v : m.base) if (!std::isfinite(v.real()) || !std::isfinite(v.imag())) v = cf32(0, 0);
        m.aud.clear();
        double pw = 0;
        m.usb.process(m.base.data(), m.base.size(), m.aud, pw);
        m.process(m.aud.data(), m.aud.size());
    }
    while ((double)m.nIn >= m.nextReport) {
        m.nextReport += 0.25 * m.curRate;
        m.report();
    }
}

bool RprReceiver::telemetry(RprTelemetry& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (p_->pub.seq <= lastSeq) return false;
    out = p_->pub;
    return true;
}
void RprReceiver::setLogCallback(std::function<void(const std::string&)> cb) { std::lock_guard<std::mutex> lk(p_->mu); p_->log = std::move(cb); }
void RprReceiver::clearResults() {
    p_->clearReq = true;
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->pub.events.clear(); p_->pub.bursts.clear(); p_->pub.blocksOk = 0; p_->pub.seq++;
}
void RprReceiver::flush() {
    Impl& m = *p_;
    if (!m.running) return;
    // wait for a running analysis, then analyse what is buffered now
    for (int i = 0; i < 2; i++) {
        {
            std::unique_lock<std::mutex> lk(m.wm);
            m.wcv.wait(lk, [&] { return !m.job && !m.busy; });
        }
        m.submit();
        std::unique_lock<std::mutex> lk(m.wm);
        m.wcv.wait(lk, [&] { return !m.job && !m.busy; });
    }
    m.changed = true;
    m.report();
}

ModeTuning rprTuning() {
    ModeTuning t;
    t.stdMode = 901;
    t.id = "rpr";
    t.name = "Robust Packet (RPR)";
    t.minMhz = 1.8; t.maxMhz = 30; t.defMhz = 10.1473;   // 10147.3 kHz, the world-wide RPR frequency (30 m) of the sigidwiki page; upper sideband dial
    t.sampleRate = 2000000;
    t.basebandHz = 1750000;
    t.bandwidthMhz = 0.004;
    t.minSampleRate = 250000;
    t.tuneOffsetHz = 20000;
    return t;
}

} // namespace dect2
