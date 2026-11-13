// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// KAIT for the engine (see kait_live.h).
#include "dect2/kait/kait_live.h"
#include "dect2/kait/kait_rx.h"
#include "dect2/kait/kait_tel.h"
#include "dect2/kapp/kapp_frame.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>

namespace dect2 {

namespace {
constexpr uint64_t kRate = (uint64_t)kait::kInternalRate;       // 632000
constexpr uint64_t kKeep = 16 * kRate;                           // samples kept
constexpr uint64_t kCaptureEnd = (uint64_t)(10.45 * kait::kInternalRate);   // from the start sweep to the end of the capture
constexpr uint64_t kLead = (uint64_t)(0.3 * kait::kInternalRate);

struct Shared {
    std::mutex mu;
    KaitTelemetry t;
    uint64_t seq = 0;
    std::atomic<uint64_t> gen{0};
    std::atomic<bool> busy{false};
    std::function<void(const std::string&)> log;
    void publish(uint64_t g, const std::function<void(KaitTelemetry&)>& edit) {
        std::lock_guard<std::mutex> l(mu);
        if (g != gen.load()) return;
        edit(t);
        t.seq = ++seq;
    }
    void say(const std::string& s) {
        std::function<void(const std::string&)> f;
        { std::lock_guard<std::mutex> l(mu); f = log; }
        if (f) f(s);
    }
};
}  // namespace

struct KaitLiveReceiver::Impl {
    mutable std::mutex feedMu;
    std::shared_ptr<Shared> sh = std::make_shared<Shared>();
    double rate = 0;
    bool ready = false;
    std::unique_ptr<kapp::Resampler> rs;
    std::vector<kait::cf> buf;
    size_t head = 0;
    uint64_t absHead = 0, total = 0, scanned = 0, blockedUntil = 0, startAbs = 0, nextReport = 0;
    int mode = 0;   // 0 scanning, 1 capturing, 2 decoding
    uint64_t gen() const { return sh->gen.load(); }

    void restart() {
        sh->gen++;                 // a running decode drops its results
        const uint64_t g = gen();
        buf.clear();
        head = 0;
        absHead = total = scanned = blockedUntil = startAbs = nextReport = 0;
        mode = 0;
        rs.reset();
        if (ready) {
            rs = std::make_unique<kapp::Resampler>(rate, kait::kInternalRate);
            // the stream starts with the lead's worth of silence: a burst that begins on the first sample (a recording started by the burst)
            // has a start sweep whose correlation peak, moved earlier by a positive carrier offset, would otherwise fall before the buffer
            // and the search would take a later peak inside the burst
            buf.assign((size_t)kLead, kait::cf{0.f, 0.f});
            total = kLead;
        }
        sh->publish(g, [&](KaitTelemetry& t) {
            t = KaitTelemetry();
            t.inputRate = rate;
            t.state = ready ? 1 : 0;
        });
    }

    void startDecode(std::vector<kait::cf> seg) {
        mode = 2;
        sh->busy = true;
        const uint64_t g = gen();
        std::shared_ptr<Shared> s = sh;
        sh->publish(g, [](KaitTelemetry& t) { t.state = 3; t.headerOk = false; t.layerNow = -1; });
        std::thread([s, g, seg = std::move(seg)]() mutable {
            kait::RxConfig cfg;
            cfg.makeImage = true;
            cfg.progressiveImages = true;
            auto fill = [](KaitTelemetry& t, const kait::RxResult& r, int layer) {
                t.headerOk = r.headerOk;
                t.callsign = r.hdr.callsign;
                t.width = r.hdr.width;
                t.height = r.hdr.height;
                t.profile = r.hdr.profileId;
                t.nLayers = r.hdr.nLayers;
                t.layersComplete = r.layersComplete;
                t.layerNow = layer;
                t.layerBlocks.clear();
                t.layerBlocksOk.clear();
                for (const kait::LayerResult& l : r.layers) { t.layerBlocks.push_back(l.blocks); t.layerBlocksOk.push_back(l.blocksOk); }
                t.bytesGood = r.bytesGood;
                t.dataValid = r.bytesGood > 0;
                t.blocksOk = t.blocksBad = 0;
                for (const kait::LayerResult& l : r.layers) { t.blocksOk += (uint64_t)l.blocksOk; t.blocksBad += (uint64_t)(l.attempted - l.blocksOk); }
                t.bytesTotal = r.hdr.streamBytes();
                t.snrDb = (float)r.snrDb;
                t.cfoHz = r.cfoHz;
                t.clockPpm = r.clockPpm;
                if (!r.image.rgb.empty()) {
                    t.image = std::make_shared<const kait::Image>(r.image);
                    t.imageSeq++;
                }
            };
            const kait::RxResult res = kait::decodeBurst(seg, cfg, [&](int layer, const kait::RxResult& part) {
                s->publish(g, [&](KaitTelemetry& t) { fill(t, part, layer); t.state = 3; });
            });
            s->publish(g, [&](KaitTelemetry& t) {
                if (res.headerOk) { fill(t, res, (int)res.layers.size() - 1); t.burstsDecoded++; }
                t.headerOk = res.headerOk;
                t.state = 4;
            });
            if (res.headerOk)
                s->say("KAIT: " + res.hdr.callsign + ", " + std::to_string(res.hdr.width) + "x" + std::to_string(res.hdr.height) + ", " + std::to_string(res.layersComplete) + " of " +
                       std::to_string(res.hdr.nLayers) + " layers, " + std::to_string(res.bytesGood) + " bytes");
            else s->say("KAIT: a sweep was found but the header did not decode");
            s->busy = false;
        }).detach();
    }

    void trim() {
        const size_t have = buf.size() - head;
        if (have > kKeep) {
            const size_t drop = have - (size_t)kKeep;
            head += drop;
            absHead += drop;
        }
        if (head > (size_t)(4 * kRate)) {
            buf.erase(buf.begin(), buf.begin() + (long)head);
            head = 0;
        }
    }

    void step() {
        if (mode == 2) {
            if (!sh->busy.load()) { mode = 0; scanned = std::max(scanned, blockedUntil); }
            return;
        }
        if (mode == 0) {
            const uint64_t from0 = std::max({scanned, blockedUntil, absHead});
            if (total < from0 + kRate) return;   // scan once a second
            const uint64_t from = from0 >= 2 * (uint64_t)kait::kSweepSamples && from0 - 2 * (uint64_t)kait::kSweepSamples >= std::max(blockedUntil, absHead) ? from0 - 2 * kait::kSweepSamples : from0;
            double pos = 0, ratio = 0;
            const kait::cf* p = buf.data() + head + (from - absHead);
            const bool hit = kait::findStartSweep(p, (size_t)(total - from), 60.f, pos, ratio);
            scanned = total;
            if (!hit) return;
            startAbs = from + (uint64_t)std::max(0.0, pos);
            mode = 1;
            nextReport = 0;
            const uint64_t g = gen();
            sh->publish(g, [](KaitTelemetry& t) { t.bursts++; t.state = 2; t.captureSec = 0; });
            sh->say("KAIT: sweep found, receiving the burst");
        }
        if (mode == 1) {
            if (total >= startAbs + kCaptureEnd) {
                const uint64_t from = startAbs > kLead + absHead ? startAbs - kLead : absHead;
                std::vector<kait::cf> seg(buf.begin() + (long)(head + (from - absHead)), buf.begin() + (long)(head + (total - absHead)));
                blockedUntil = startAbs + kCaptureEnd;
                startDecode(std::move(seg));
            } else if (total >= nextReport) {
                nextReport = total + kRate / 4;
                const double sec = (double)(total - startAbs) / kait::kInternalRate;
                sh->publish(gen(), [&](KaitTelemetry& t) { t.state = 2; t.captureSec = std::min(10.0, sec); });
            }
        }
    }
};

KaitLiveReceiver::KaitLiveReceiver() : p_(std::make_unique<Impl>()) {}
KaitLiveReceiver::~KaitLiveReceiver() { p_->sh->gen++; }

void KaitLiveReceiver::configure(double r) {
    Impl& m = *p_;
    std::lock_guard<std::mutex> lk(m.feedMu);
    m.rate = r;
    m.ready = r >= kaitTuning().minSampleRate - 1;
    m.restart();
}

bool KaitLiveReceiver::ready() const {
    std::lock_guard<std::mutex> lk(p_->feedMu);
    return p_->ready;
}

void KaitLiveReceiver::reset() {
    Impl& m = *p_;
    std::lock_guard<std::mutex> lk(m.feedMu);
    m.restart();
}

void KaitLiveReceiver::feed(const cf32* x, size_t n) {
    Impl& m = *p_;
    if (n == 0) return;
    std::lock_guard<std::mutex> lk(m.feedMu);
    if (!m.ready || !m.rs) return;
    const std::vector<kait::cf> y = m.rs->process(x, n);
    m.buf.insert(m.buf.end(), y.begin(), y.end());
    m.total += y.size();
    m.trim();
    m.step();
}

bool KaitLiveReceiver::telemetry(KaitTelemetry& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> lk(p_->sh->mu);
    if (p_->sh->t.seq == 0 || p_->sh->t.seq == lastSeq) return false;
    out = p_->sh->t;
    return true;
}

void KaitLiveReceiver::setLogCallback(std::function<void(const std::string&)> cb) {
    std::lock_guard<std::mutex> l(p_->sh->mu);
    p_->sh->log = std::move(cb);
}

void KaitLiveReceiver::waitIdle() {
    for (int i = 0; i < 6000 && p_->sh->busy.load(); i++) std::this_thread::sleep_for(std::chrono::milliseconds(10));
}

ModeTuning kaitTuning() {
    ModeTuning t;
    t.stdMode = 133; t.id = "kait"; t.name = "KAIT";
    t.minMhz = 0.1; t.maxMhz = 6000; t.defMhz = 433.0;   // modeTuning() opens the range to any frequency; the signal is 190 kHz wide
    t.sampleRate = 1000000;       // the radio rate asked for; the receiver resamples to 632 ksps itself
    t.basebandHz = 0;
    t.bandwidthMhz = 0.2;
    t.minSampleRate = 400000;
    t.tuneOffsetHz = 0;
    return t;
}

} // namespace dect2
