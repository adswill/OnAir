// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// KAAL for the engine: see kaal_live.h.
#include "dect2/kaal/kaal_live.h"
#include "dect2/kaal/kaal_rx.h"
#include "dect2/kaal/kaal_beacon.h"
#include "dect2/kaal/kaal_tel.h"
#include "dect2/kaal/kaal_v4.h"
#include "dect2/common/diag.h"
#include "dect2/audio/audioout.h"
#include "dect2/dsp/fftutil.h"
#include "dect2/dsp/resampler.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <vector>

namespace dect2 {

struct KaalLiveReceiver::Impl {
    std::mutex feedMu;                       // the processing state below
    std::mutex pubMu;                        // the published telemetry and the callbacks
    kaal::VoiceRx vrx;
    kaal::BeaconSliceRx brx;
    kaal::SweepIdScanner scan;               // station ID from the sweep alone, while the voice receiver is not locked
    kaal::SweepIdResult scanId;              // the last ID the scanner found (kept while the voice receiver has the signal)
    bool scanWasFed = false;
    double rate = 0;
    // The input goes to 48 kHz here, then a coarse carrier search (the two radios' oscillators differ by up to several kHz at UHF, more than the
    // receiver's own capture range): the centre of the 8 kHz wide signal in the averaged spectrum is mixed to 0 Hz while the receiver is not locked
    // The working rate is 96 kHz: a signal of 8 ksym/s (12 / 16 kbit/s voice) is decimated to 48 kHz for the receivers; one of 16 ksym/s (24 / 32 kbit/s,
    // about 20 kHz wide) goes to them as it is, read as a 48 kHz stream: the whole format is the same one at twice the speed, so the receivers
    // need no other change. Which of the two it is comes from the width of the spectrum.
    static constexpr int kN = 2048;          // spectrum bins of the 96 kHz stream
    static constexpr double kWR = 96000.0;
    RationalResampler rs, dec;
    bool resampling = false;
    bool wide = false;                       // 16 ksym/s
    bool isWideDec = false;
    std::vector<cf32> y96, m48;
    Fft fft{kN};
    std::vector<float> win;
    std::vector<cf32> seg;                   // the segment being filled (kN samples), hop kN / 2
    std::vector<double> psd;
    int psdCount = 0;
    double carrier = 0;                      // Hz: the carrier offset taken out
    double mixPhase = 0;
    cf32 dcm{0.f, 0.f};                      // slow DC estimate: the RTL-SDR's spike at the tune centre
    bool locked = false;
    uint64_t sinceDecision = 0;
    double contrastDb = 0;
    bool decided = false, refined = false;
    int badRestarts = 0;
    double fine = 0, finePhase = 0;          // the voice receiver's residual carrier (Hz), once it is locked
    bool ready = false;
    uint64_t samples = 0, nextReport = 0, seq = 0;
    float level = 0;                         // decaying peak of the speech, 0 .. 1
    float lastSample = 0;                    // for the 16 kHz to 48 kHz interpolation
    std::vector<int16_t> pcm;
    std::vector<float> stereo;
    // frame version 4: its own receiver on the 96 kHz stream brought to 48 kHz with the full 20 kHz band (not the mixed one: it has +-4 kHz of
    // capture itself and must not see the jumps of the v3 coarse carrier search)
    kaal::v4::FrameRx v4rx;
    RationalResampler r48;
    std::vector<cf32> v48;
    bool v4locked = false;
    std::unique_ptr<kaal::v4::SlotVoiceDecoder> v4dec;
    int v4play = -1;
    std::string v4call[4];
    uint64_t v4texts = 0;
    std::vector<int16_t> v4pcm;
    // the last beacon
    bool beaconValid = false;
    uint64_t beacons = 0;
    double beaconAtSec = 0;
    kaal::BeaconFields bf;
    std::string btext;
    // sound
    std::atomic<float> volume{1.f};
    std::atomic<bool> muted{false}, silent{false};
    std::unique_ptr<AudioOut> audioOut;
    std::function<void(const std::string&)> log;
    std::function<void(const int16_t*, size_t)> tap;
    KaalTelemetry pub;
    bool wasLocked = false;

    void play(const int16_t* x, size_t n) {
        if (n == 0) return;
        stereo.resize(n * 3 * 2);
        float peak = 0;
        size_t o = 0;
        for (size_t i = 0; i < n; i++) {
            const float cur = (float)x[i] / 32768.f;
            peak = std::max(peak, std::fabs(cur));
            for (int k = 1; k <= 3; k++) {
                const float v = lastSample + (cur - lastSample) * (float)k / 3.f;
                stereo[o++] = v; stereo[o++] = v;
            }
            lastSample = cur;
        }
        level = std::max(peak, level * 0.9f);
        if (silent.load()) return;
        if (!audioOut) { audioOut = std::make_unique<AudioOut>(); audioOut->start(48000); audioOut->setStartThreshold(48000 / 10); }   // 0.1 s of prefill (the speech arrives in 80 ms blocks)
        audioOut->setVolume(volume.load());
        audioOut->setMuted(muted.load());
        if (audioOut->bufferedFrames() > 48000 * 3 / 2) audioOut->flush();
        audioOut->write(stereo.data(), (int)(stereo.size() / 2));
    }

    // best window of Wbins bins of the averaged spectrum: its sum, its centre bin, and how far it stands above the median window (the noise)
    struct Win { double best = 0, med = 0; int c = 0; };
    Win bestWindow(int Wbins) const {
        Win w;
        std::vector<double> sc((size_t)kN);
        double best = -1;
        for (int c = 0; c < kN; c++) {
            double acc = 0;
            for (int k = -Wbins / 2; k < Wbins - Wbins / 2; k++) acc += psd[(size_t)((c + k + kN) & (kN - 1))];
            sc[(size_t)c] = acc;
            if (acc > best) { best = acc; w.c = c; }
        }
        std::vector<double> sorted(sc);
        std::nth_element(sorted.begin(), sorted.begin() + kN / 2, sorted.end());
        w.best = best; w.med = sorted[(size_t)kN / 2];
        return w;
    }

    void coarseDecide() {
        const double binHz = kWR / kN;
        double tot = 0;
        for (int b = 0; b < kN; b++) tot += psd[(size_t)b];
        if (tot <= 0) return;
        // two hypotheses: the signal is 8 kHz wide (-6 dB width of the 8 ksym/s RRC signal) or 16 kHz wide (16 ksym/s). A narrow signal fills the 8 kHz
        // window and only half of the 16 kHz one, so its contrast is 3 dB higher in the first; a wide one gives about the same in both
        const int W8 = (int)std::lround(8000.0 / binHz), W16 = (int)std::lround(16000.0 / binHz);
        const Win a = bestWindow(W8), b = bestWindow(W16);
        const double c8 = a.med > 0 ? a.best / a.med : 0, c16 = b.med > 0 ? b.best / b.med : 0;
        contrastDb = 10 * std::log10(std::max(c8, c16) + 1e-30);
        if (contrastDb < 5.0) return;   // no signal
        // the width: the smallest window that holds 90 % of the power above the noise floor (the median bin). A narrow signal is about 8 kHz by that
        // measure, a wide one 16 kHz; the line between them is 11.5 kHz. It works with a noiseless signal (a recording) and with a noisy one
        std::vector<double> ex((size_t)kN);
        {
            std::vector<double> srt(psd);
            std::nth_element(srt.begin(), srt.begin() + kN / 2, srt.end());
            const double floorP = srt[(size_t)kN / 2];
            double tx = 0;
            for (int k = 0; k < kN; k++) { ex[(size_t)k] = std::max(0.0, psd[(size_t)k] - floorP); tx += ex[(size_t)k]; }
            if (tx <= 0) return;
            bool wideSig = false;
            for (int Wb = (int)std::lround(5000.0 / binHz); Wb <= (int)std::lround(30000.0 / binHz); Wb += 6) {
                double acc = 0, bestW = 0;
                for (int k = 0; k < Wb; k++) acc += ex[(size_t)k];
                bestW = acc;
                for (int c = 1; c < kN; c++) { acc += ex[(size_t)((c + Wb - 1) & (kN - 1))] - ex[(size_t)(c - 1)]; bestW = std::max(bestW, acc); }
                if (bestW >= 0.9 * tx) { wideSig = (double)Wb * binHz > 11500.0; break; }
            }
            isWideDec = wideSig;
        }
        const bool isWide = isWideDec;
        const Win& w = isWide ? b : a;
        const int Wb = isWide ? W16 : W8;
        const double edgeHz = isWide ? 8667.0 : 4333.0;   // the centre is this far above the -6 dB point of the left edge (the PSD is the raised cosine, 25 % there)
        // the left edge of the signal: where the spectrum rises to -6 dB of its flat top. The right edge is not used: the beacon slice sits on it.
        const int ext = (int)std::lround((isWide ? 5000.0 : 2500.0) / binHz);
        const double meanIn = w.best / (double)Wb;
        const double lvl = 0.25 * meanIn;
        auto sp = [&](int k) { double acc = 0; for (int j = -2; j <= 2; j++) acc += psd[(size_t)((k + j + 2 * kN) & (kN - 1))]; return acc / 5.0; };
        int kk = w.c - Wb / 2 - ext;
        while (kk < w.c && sp(kk) < lvl) kk++;
        const double p1 = sp(kk), p0 = sp(kk - 1);
        const double frac = p1 > p0 ? std::max(0.0, std::min(1.0, (lvl - p0) / (p1 - p0))) : 0.0;
        const double cb = (double)(kk - 1) + frac + edgeHz / binHz;
        double hz = cb * binHz;
        if (hz >= kWR / 2) hz -= kWR;
        if (!decided || isWide != wide || std::fabs(hz - carrier) > 600.0) {   // a new carrier or width: what was gathered under the old one is useless
            carrier = hz;
            decided = true;
            wide = isWide;
            configureReceivers();
        }
    }

    void configureReceivers() {
        vrx.configure(48000.0);
        vrx.setFastAcquire(true);
        brx.configure(48000.0);
        scan.configure(48000.0);
        scanWasFed = false;
        dec.reset();
    }

    // one block of 96 kHz samples: DC block, spectrum, mix, then (8 ksym/s) decimation to 48 kHz and the receivers
    void process96(const cf32* x, size_t n) {
        const double dph = 2 * 3.141592653589793 * carrier / kWR;
        std::vector<cf32> mixed(n);
        for (size_t i = 0; i < n; i++) {
            const cf32 v = x[i];   // no DC blocker: a signal on the tune centre has real content at 0 Hz (silent speech frames especially); the receiver removes the mean itself
            seg.push_back(v);
            if (seg.size() == (size_t)kN) {
                std::vector<cf32> f((size_t)kN);
                for (int k = 0; k < kN; k++) f[(size_t)k] = seg[(size_t)k] * win[(size_t)k];
                fft.forward(f.data());
                const double al = psdCount ? 0.04 : 1.0;
                for (int k = 0; k < kN; k++) psd[(size_t)k] += al * ((double)std::norm(f[(size_t)k]) - psd[(size_t)k]);
                psdCount++;
                seg.erase(seg.begin(), seg.begin() + kN / 2);
            }
            mixPhase -= dph;
            if (mixPhase < -6.283185307179586) mixPhase += 6.283185307179586;
            if (mixPhase > 6.283185307179586) mixPhase -= 6.283185307179586;
            mixed[i] = v * cf32((float)std::cos(mixPhase), (float)std::sin(mixPhase));
        }
        sinceDecision += n;
        v48.clear();
        r48.process(x, n, v48);
        if (!v48.empty()) { v4rx.feed(v48.data(), v48.size()); v4drain(); }
        if (v4locked) return;   // a v4 signal: the v3 receivers would only search
        if (!locked && sinceDecision >= 48000 && psdCount > 20) { sinceDecision = 0; coarseDecide(); }
        const cf32* p = mixed.data();
        size_t m = n;
        if (!wide) {
            m48.clear();
            dec.process(mixed.data(), n, m48);
            p = m48.data(); m = m48.size();
        }
        if (m == 0) return;
        std::vector<cf32> st(p, p + m);
        vrx.feed(st.data(), m);
        if (fine != 0.0) {   // the beacon receiver has little capture range: it gets the signal with the voice receiver's own carrier estimate taken out too
            const double dpf = 2 * 3.141592653589793 * fine / 48000.0;
            for (size_t i = 0; i < m; i++) {
                finePhase -= dpf;
                if (finePhase < -6.283185307179586) finePhase += 6.283185307179586;
                if (finePhase > 6.283185307179586) finePhase -= 6.283185307179586;
                st[i] *= cf32((float)std::cos(finePhase), (float)std::sin(finePhase));
            }
        }
        brx.feed(st.data(), m);
        {   // the sweep-only scanner reads the signals the voice receiver cannot lock to
            const bool vlocked = vrx.stats().locked;
            if (!vlocked) { scan.feed(st.data(), m); scanWasFed = true; }
            else if (scanWasFed) { scan.reset(); scanWasFed = false; }
            kaal::SweepIdResult ir;
            while (scan.poll(ir)) {
                scanId = ir;
                std::function<void(const std::string&)> lg;
                { std::lock_guard<std::mutex> pk(pubMu); lg = log; }
                if (lg) lg("KAAL sweep ID " + ir.callsign);
            }
        }
    }

    void v4drain() {
        v4locked = v4rx.stats().locked;
        kaal::v4::RxSuperframe r;
        while (v4rx.popSuperframe(r)) {
            if (v4play < 0 || !kaal::v4::profileVoice(r.slot[v4play].profile))
                for (int s = 0; s < kaal::v4::kSlots; s++) if (r.slot[s].ok && kaal::v4::profileVoice(r.slot[s].profile)) { v4play = s; break; }
            if (v4play < 0) continue;
            const kaal::v4::SlotPayload& p = r.slot[v4play];
            if (!v4dec) { const kaal::v4::SlotCodec* c = kaal::v4::slotCodec(); if (c) v4dec.reset(c->makeDecoder()); }
            if (!v4dec) continue;
            v4pcm.assign(kaal::v4::kPcmPerPayload, 0);
            if (p.ok && !kaal::v4::profileVoice(p.profile)) v4dec->silence(v4pcm.data());   // the talker paused (DTX): the slot carried data
            else v4dec->decode(p.ok ? p.bits : nullptr, p.profile, v4pcm.data());
            if (tap) tap(v4pcm.data(), v4pcm.size());
            play(v4pcm.data(), v4pcm.size());
        }
        kaal::BeaconFields f;
        while (v4rx.pollBeacon(f)) {
            bf = f; beaconValid = true; beacons++;
            beaconAtSec = rate > 0 ? (double)samples / rate : 0;
        }
        std::string t;
        while (v4rx.pollText(t)) { btext = t; v4texts++; }
        int s;
        std::string c;
        while (v4rx.pollSlotCall(s, c)) v4call[s & 3] = c;
    }

    void startStream() {
        resampling = std::fabs(rate - kWR) > 1e-6;
        if (resampling) resampling = rs.configure(rate, kWR);
        if (resampling) rs.reset();
        dec.configure(kWR, 48000.0);
        r48.configure(kWR, 48000.0);
        r48.reset();
        v4rx.configure(48000.0);
        v4locked = false; v4dec.reset(); v4play = -1; v4texts = 0;
        for (auto& c : v4call) c.clear();
        win.resize((size_t)kN);
        for (int k = 0; k < kN; k++) win[(size_t)k] = (float)(0.5 - 0.5 * std::cos(2 * 3.141592653589793 * k / kN));
        seg.clear(); psd.assign((size_t)kN, 0.0); psdCount = 0; carrier = 0; fine = 0; finePhase = 0; decided = false; refined = false; badRestarts = 0; mixPhase = 0; dcm = cf32(0, 0); locked = false; sinceDecision = 0; wide = false;
        configureReceivers();
        scanId = kaal::SweepIdResult();
        samples = 0; nextReport = (uint64_t)(0.25 * rate); level = 0; lastSample = 0;
        beaconValid = false; beacons = 0; beaconAtSec = 0; btext.clear(); bf = kaal::BeaconFields();
        wasLocked = false;
    }

    void report() {
        const kaal::RxStats st = vrx.stats();
        KaalTelemetry t;
        t.seq = ++seq;
        if (v4locked) {
            const kaal::v4::RxStatsV4 s4 = v4rx.stats();
            t.state = 2; t.frameVersion = 4;
            t.cfoHz = s4.cfoHz; t.symRate = kaal::v4::kRs; t.snrDb = s4.esn0Db;
            t.audioLevel = level; level *= 0.7f;
            t.blocksOk = s4.slotsOk; t.blocksBad = s4.slotsBad;
            t.v4SlotsOk = s4.slotsOk; t.v4SlotsBad = s4.slotsBad; t.v4Texts = v4texts; t.v4PlaySlot = v4dec ? v4play : -1;
            for (int i = 0; i < 4; i++) { t.v4Profile[i] = (int)s4.lastProfile[i]; t.v4Call[i] = v4call[i]; }
            t.dataValid = s4.slotsOk > 0;
            t.inputRate = rate;
            t.timeSec = rate > 0 ? (double)samples / rate : 0;
            t.beaconValid = beaconValid; t.beacons = beacons;
            t.beaconAgeSec = beaconValid ? std::max(0.0, t.timeSec - beaconAtSec) : 0;
            t.text = btext;
            if (beaconValid) { t.callsign = bf.callsign; t.grid = bf.grid; t.lat = bf.lat; t.lon = bf.lon; t.powerDbm = bf.powerDbm; t.linkQuality = bf.linkQuality; }
            std::function<void(const std::string&)> lg;
            { std::lock_guard<std::mutex> lk(pubMu); pub = t; lg = log; }
            if (lg && !wasLocked) lg("KAAL v4 locked");
            wasLocked = true;
            return;
        }
        t.state = st.locked ? 2 : samples ? 1 : 0;
        if (st.locked && !refined && std::fabs((double)st.cfoHz) > 15.0) {
            // The coarse estimate (the spectrum edge) is good to a few hundred hertz; the receiver measures what is left to a few hertz at lock. Take it
            // out of the mixer and search once more: with no carrier error left the frames and the beacon decode cleanly.
            refined = true;
            carrier += (double)st.cfoHz * (wide ? 2.0 : 1.0);   // the receiver reads a wide signal at half its frequencies
            vrx.shiftCarrier(-(double)st.cfoHz);   // the lock is kept: only the carrier and the sweep trackers move
            brx.configure(48000.0);
            fine = 0;
            std::function<void(const std::string&)> lg;
            { std::lock_guard<std::mutex> pk(pubMu); lg = log; }
            if (lg) lg("KAAL carrier refined to " + std::to_string((long)std::lround(carrier)) + " Hz");
            return;
        }
        if (st.locked && refined && badRestarts < 4 && st.framesGood + st.framesBad >= 100 && st.framesBad * 100 > 6 * (st.framesGood + st.framesBad)) {
            // locked, but more than one frame in sixteen fails: the block alignment chosen from the window it happened to gather was the wrong one. A later window
            // may pick the right one: search again (the carrier is already right)
            badRestarts++;
            DIAG_DEBUG(514276, "The KAAL receiver is locked but more than 6%% of its frames fail (%d of %d), so it searches again for a better block alignment (try %d of 4).", (int)st.framesBad, (int)(st.framesGood + st.framesBad), badRestarts);
            vrx.configure(48000.0);
            brx.configure(48000.0);
            fine = 0; locked = false;
            return;
        }
        locked = st.locked;
        if (!st.locked) fine = 0.0; else if (fine == 0.0 || std::fabs((double)st.cfoHz - fine) > 3.0) fine = (double)st.cfoHz;
        t.cfoHz = st.cfoHz * (wide ? 2.0 : 1.0) + carrier; t.symRate = wide ? 16000 : 8000; t.snrDb = st.snrDb; t.sweepDb = st.sweepDb;
        t.audioLevel = level;
        level *= 0.7f;
        t.blocksOk = st.framesGood; t.blocksBad = st.framesBad;
        t.bitrateKbps = st.bitrateKbps;
        if (st.idValid) { t.sweepIdValid = true; t.sweepId = st.idCall; t.sweepIdGateDb = st.idGateDb; }
        else if (scanId.valid) { t.sweepIdValid = true; t.sweepId = scanId.callsign; t.sweepIdGateDb = scanId.gateDb; }
        t.dataValid = st.locked && st.framesGood > 0;
        t.inputRate = rate;
        t.timeSec = rate > 0 ? (double)samples / rate : 0;
        t.beaconValid = beaconValid; t.beacons = beacons;
        t.beaconAgeSec = beaconValid ? std::max(0.0, t.timeSec - beaconAtSec) : 0;
        if (beaconValid) {
            t.callsign = bf.callsign; t.grid = bf.grid; t.text = btext;
            t.lat = bf.lat; t.lon = bf.lon; t.powerDbm = bf.powerDbm; t.linkQuality = bf.linkQuality;
        }
        std::function<void(const std::string&)> lg;
        {
            std::lock_guard<std::mutex> lk(pubMu);
            pub = t;
            lg = log;
        }
        if (lg && st.locked != wasLocked) lg(st.locked ? "KAAL locked" : "KAAL lock lost");
        if (st.locked != wasLocked && !st.locked) DIAG_DEBUG(733898, "The KAAL receiver lost its lock after %d good and %d failed frames, so it searches for the sweep again.", (int)st.framesGood, (int)st.framesBad);
        wasLocked = st.locked;
    }
};

KaalLiveReceiver::KaalLiveReceiver() : p_(std::make_unique<Impl>()) {}
KaalLiveReceiver::~KaalLiveReceiver() = default;

void KaalLiveReceiver::configure(double r) {
    Impl& m = *p_;
    std::lock_guard<std::mutex> lk(m.feedMu);
    m.rate = r;
    m.ready = r >= kaalTuning().minSampleRate - 1;
    if (m.ready) m.startStream();
    std::lock_guard<std::mutex> pk(m.pubMu);
    m.pub = KaalTelemetry();
    m.pub.seq = ++m.seq;
    m.pub.inputRate = r;
}

bool KaalLiveReceiver::ready() const {
    std::lock_guard<std::mutex> lk(p_->feedMu);
    return p_->ready;
}

void KaalLiveReceiver::reset() {
    Impl& m = *p_;
    std::lock_guard<std::mutex> lk(m.feedMu);
    if (m.ready) m.startStream();
    std::lock_guard<std::mutex> pk(m.pubMu);
    m.pub = KaalTelemetry();
    m.pub.seq = ++m.seq;
    m.pub.inputRate = m.rate;
}

void KaalLiveReceiver::feed(const cf32* x, size_t n) {
    Impl& m = *p_;
    if (n == 0) return;
    std::lock_guard<std::mutex> lk(m.feedMu);
    if (!m.ready) return;
    if (m.resampling) {
        m.y96.clear();
        m.rs.process(x, n, m.y96);
        if (!m.y96.empty()) m.process96(m.y96.data(), m.y96.size());
    } else {
        m.process96(x, n);
    }
    m.samples += n;
    m.pcm.resize(4096);
    for (;;) {
        const size_t k = m.vrx.popAudio(m.pcm.data(), m.pcm.size());
        if (k == 0) break;
        if (m.tap) m.tap(m.pcm.data(), k);
        m.play(m.pcm.data(), k);
    }
    kaal::BeaconFields f;
    std::string text;
    while (m.brx.poll(f, text)) {
        const bool sameStation = m.beaconValid && m.bf.callsign == f.callsign;
        if (!text.empty() || !sameStation) m.btext = text;   // a header-only frame (every 2 s) must not wipe the bio of the last full frame (every 10 s)
        m.bf = f; m.beaconValid = true; m.beacons++;
        m.beaconAtSec = m.rate > 0 ? (double)m.samples / m.rate : 0;
        std::function<void(const std::string&)> lg;
        { std::lock_guard<std::mutex> pk(m.pubMu); lg = m.log; }
        if (lg) lg("KAAL beacon " + f.callsign + " " + f.grid);
    }
    if (m.samples >= m.nextReport) {
        m.nextReport = m.samples + (uint64_t)(0.25 * m.rate);
        m.report();
    }
}

bool KaalLiveReceiver::telemetry(KaalTelemetry& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> lk(p_->pubMu);
    if (p_->pub.seq == 0 || p_->pub.seq == lastSeq) return false;
    out = p_->pub;
    return true;
}

void KaalLiveReceiver::setLogCallback(std::function<void(const std::string&)> cb) {
    p_->vrx.setLogCallback(cb);   // the receiver's own lines too (acquired, sync lost)
    std::lock_guard<std::mutex> lk(p_->pubMu);
    p_->log = std::move(cb);
}
void KaalLiveReceiver::setVolume(float v) { p_->volume = std::max(0.f, std::min(1.f, v)); }
void KaalLiveReceiver::setMuted(bool m) { p_->muted = m; }
void KaalLiveReceiver::setSilent(bool s) { p_->silent = s; }
void KaalLiveReceiver::setAudioTap(std::function<void(const int16_t*, size_t)> cb) { std::lock_guard<std::mutex> lk(p_->feedMu); p_->tap = std::move(cb); }

ModeTuning kaalTuning() {
    ModeTuning t;
    t.stdMode = 124; t.id = "kaal"; t.name = "KAAL";
    t.minMhz = 0.1; t.maxMhz = 6000; t.defMhz = 433.0;   // modeTuning() opens the range to any frequency; the signal is 10 kHz wide
    t.sampleRate = 240000;        // the radio rate asked for (5 times 48 kHz); the receiver resamples to 48 kHz itself
    t.basebandHz = 0;
    t.bandwidthMhz = 0.024;   // 24 / 32 kbit/s voice is 20 kHz wide
    t.minSampleRate = 48000;
    t.tuneOffsetHz = 0;
    return t;
}
} // namespace dect2
