// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Globalstar activity monitor: see globalstar_rx.h. Windowed FFT frames of the whole sampled band are averaged into 10 ms slots; each of the
// 1.23 MHz channels gets its power, a noise floor tracker, an on / off decision with hysteresis and, while on, the centroid of its spectrum
// (the Doppler shift against the channel centre). Nothing is demodulated.
#include "dect2/globalstar/globalstar_rx.h"
#include "dect2/dsp/fftutil.h"
#include "dect2/dsp/dsp_compat.h"
#include "dect2/results/results_io.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <deque>
#include <mutex>
#include "dect2/common/mathconst.h"

namespace dect2 {

namespace {

using dect2::math::kPi;
constexpr double kSlotSec = 0.01;
constexpr double kPublishSec = 0.25;
constexpr double kPowerHalf = 0.615e6;       // the channel's own bandwidth: 1.23 MHz
constexpr double kCentroidHalf = 0.64e6;     // the window the centroid is taken over
constexpr int kWarmSlots = 20;               // the first 0.2 s set the noise floor
constexpr double kOnDb = 0.8, kOffDb = 0.4;  // activity threshold above the floor, with hysteresis
constexpr int kOnSlots = 3, kOffSlots = 10;
constexpr double kOccTau = 60.0;             // occupancy window, seconds

struct Chan {
    bool inView = false;
    double fIn = 0;                           // channel centre in the input, Hz
    std::vector<int> pBins;                   // bins of the channel power
    std::vector<int> cBins; std::vector<double> cOff;   // bins of the centroid window and their offsets from the centre
    double warmSum = 0;
    double nf = 0;                            // noise floor, linear
    bool active = false;
    int onCnt = 0, offCnt = 0;
    double occ = 0;
    double power = 0, snr = 0;                // last slot
    double dopSum = 0; int dopN = 0; float snrMax = 0;
    double dop = 0;
    // the stretch under way
    double evStart = 0, evLastOn = 0; float evPeak = 0; float evDop0 = 0; bool evDop0Set = false;
    int64_t evWall = 0;
    double lastActive = -1e9;
    uint32_t events = 0;
};

} // namespace

struct GlobalstarReceiver::Impl {
    std::mutex procMu, telMu;
    std::function<void(const std::string&)> log;
    double fs = 0, offsetHz = 0, dialHz = 0;
    bool isReady = false, wallClock = false;
    int NF = 0, framesPerSlot = 1;
    std::unique_ptr<Fft> fft;
    std::vector<float> win;
    double wNorm = 1;
    std::vector<cf32> blk;
    size_t blkN = 0;
    std::vector<double> acc;                  // power spectrum of the slot under way
    int framesAcc = 0;                        // frames in the slot's average
    int elapsed = 0;                          // frames of the slot gone by (the duty cycle skips some)
    int duty = 1, log2n = 0;                  // one frame in `duty` is transformed: a slot still averages a dozen frames of 250 bins
    size_t skipLeft = 0;
    std::vector<float> fre, fim;
    Chan ch[kGsChannels];
    int slots = 0;
    double now = 0;
    double nextPublish = kPublishSec;
    uint64_t seq = 0;
    double lastAny = -1e9;
    std::deque<GsDopplerPoint> track;
    std::deque<GsEvent> events;
    uint64_t eventsDone = 0;
    GlobalstarTelemetry pub;
    double lastDop = 0; float lastSnr = 0; int activeNow = 0;
    bool localCap = true;                     // 4.1 setLocalFloorCap()

    void say(const std::string& s) { if (log) log(s); }

    double dial() const { return dialHz > 0 ? dialHz : kGsDefaultDialHz; }

    void plan() {
        for (int c = 0; c < kGsChannels; c++) {
            Chan& C = ch[c];
            C.pBins.clear(); C.cBins.clear(); C.cOff.clear();
            C.fIn = offsetHz + (globalstarChannelHz(c) - dial());
            C.inView = std::fabs(C.fIn) + kCentroidHalf + 0.1e6 <= 0.46 * fs;
            if (!C.inView) continue;
            const double df = fs / NF;
            for (int k = 0; k < NF; k++) {
                const double f = (k < NF / 2 ? k : k - NF) * df;
                const double d = f - C.fIn;
                if (std::fabs(d) <= kPowerHalf) C.pBins.push_back(k);
                if (std::fabs(d) <= kCentroidHalf) { C.cBins.push_back(k); C.cOff.push_back(d); }
            }
            if (C.pBins.empty()) C.inView = false;
        }
    }

    void configure(double rate) {
        fs = rate;
        isReady = fs >= globalstarTuning().minSampleRate - 1;
        if (!isReady) return;
        NF = 256;
        while (NF < 4096 && fs / NF > 9000.0) NF *= 2;
        framesPerSlot = std::max(1, (int)std::lround(kSlotSec * fs / NF));
        fft = std::make_unique<Fft>(NF);
        log2n = 0; while ((1 << log2n) < NF) log2n++;
        fre.assign((size_t)NF, 0.f); fim.assign((size_t)NF, 0.f);
        duty = 1;   // 2 skipped alternate frames: Doppler error rms 3.1 kHz in test_globalstar_rx (653 Hz with every frame), so every frame is transformed
        win.resize((size_t)NF);
        wNorm = 0;
        for (int i = 0; i < NF; i++) { win[(size_t)i] = (float)(0.5 - 0.5 * std::cos(2 * kPi * (i + 0.5) / NF)); wNorm += (double)win[(size_t)i] * win[(size_t)i]; }
        blk.assign((size_t)NF, cf32());
        acc.assign((size_t)NF, 0.0);
        plan();
        resetState();
    }

    void resetState() {
        const double shift = now;
        for (auto& p : track) p.timeSec -= shift;
        for (auto& e : events) e.startSec -= shift;
        now = 0; nextPublish = kPublishSec; slots = 0;
        blkN = 0; framesAcc = 0; elapsed = 0; skipLeft = 0;
        std::fill(acc.begin(), acc.end(), 0.0);
        for (auto& C : ch) {
            C.warmSum = 0; C.nf = 0; C.active = false; C.onCnt = C.offCnt = 0; C.occ = 0; C.power = C.snr = 0;
            C.dopSum = 0; C.dopN = 0; C.snrMax = 0; C.dop = 0; C.evStart = C.evLastOn = 0; C.lastActive -= shift;
            C.evDop0Set = false;
        }
        lastAny = -1e9; activeNow = 0;
        publish();
    }

    void closeEvent(int c, double endSec) {
        Chan& C = ch[c];
        const double dur = endSec - C.evStart;
        if (dur < 0.05) return;
        GsEvent e;
        e.startSec = C.evStart; e.durationSec = dur; e.wallTime = C.evWall; e.chan = c;
        e.peakSnrDb = C.evPeak; e.dopplerStartHz = C.evDop0; e.dopplerEndHz = (float)C.dop;
        events.push_front(e);
        while (events.size() > kGsEventsMax) events.pop_back();
        eventsDone++; C.events++;
    }

    void onSlot() {
        const double per = 1.0 / ((double)framesAcc * (double)NF * wNorm);   // |X|^2 -> power per bin, summed over all bins = mean |x|^2
        slots++;
        double warmMin = 1e30;
        for (int c = 0; c < kGsChannels; c++) {
            Chan& C = ch[c];
            if (!C.inView) continue;
            double p = 0;
            for (int k : C.pBins) p += acc[(size_t)k];
            p *= per;
            C.power = p;
            if (slots <= kWarmSlots) { C.warmSum += p; if (slots == kWarmSlots) warmMin = std::min(warmMin, C.warmSum / kWarmSlots); continue; }
            if (C.nf <= 0) continue;
            if (p < C.nf) C.nf += 0.3 * (p - C.nf);
            else if (!C.active && p < C.nf * 1.6) C.nf += 0.01 * (p - C.nf);
            const double snr = 10 * std::log10(std::max(1e-30, p / C.nf));
            C.snr = snr;
            const bool wasActive = C.active;
            if (!C.active) { C.onCnt = snr > kOnDb ? C.onCnt + 1 : 0; if (C.onCnt >= kOnSlots) { C.active = true; C.offCnt = 0; } }
            else { C.offCnt = snr < kOffDb ? C.offCnt + 1 : 0; if (C.offCnt >= kOffSlots) { C.active = false; C.onCnt = 0; } }
            if (C.active && !wasActive) {
                C.evStart = now - kSlotSec * kOnSlots; C.evPeak = 0; C.evDop0Set = false;
                C.evWall = wallClock ? (int64_t)std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count() : 0;
            }
            if (!C.active && wasActive) { closeEvent(c, C.evLastOn); }
            if (C.active) {
                if (C.offCnt == 0) C.evLastOn = now;
                C.lastActive = now; lastAny = now;
                C.evPeak = std::max(C.evPeak, (float)snr);
                // the centroid of the spectrum above the floor against the channel centre
                const double nb = C.nf / (double)C.pBins.size() / per;   // the floor per bin in accumulator units
                if (snr < kOnDb) { C.occ += (kSlotSec / kOccTau) * (1.0 - C.occ); continue; }   // the hold-off after the end: no centroid (the neighbour leaks in)
                double sw = 0, sx = 0;
                for (size_t i = 0; i < C.cBins.size(); i++) {
                    const double v = acc[(size_t)C.cBins[i]] - nb;
                    if (v > 0) { sw += v; sx += v * C.cOff[i]; }
                }
                if (sw > 0) {
                    const double d = sx / sw;
                    C.dopSum += d; C.dopN++; C.snrMax = std::max(C.snrMax, (float)snr);
                    C.dop = d;
                    if (!C.evDop0Set) { C.evDop0 = (float)d; C.evDop0Set = true; }
                }
            }
            C.occ += (kSlotSec / kOccTau) * ((C.active ? 1.0 : 0.0) - C.occ);
        }
        if (slots == kWarmSlots) {
            // a channel already on during the first 0.2 s would take its signal for the floor: its floor is capped 1 dB over a quieter channel's.
            // 4.1: over the quieter of its neighbours (and itself), not over the quietest channel of the whole band: the noise is not flat over the
            // band (the radio's filter or a resampler rolls it off towards the edges), and a cap from an edge channel put the floor of the middle
            // channels under their noise, so they went on and, the floor being frozen while on, never off again (QA-4.0: 0 events at 13.7 Msps with
            // a clock error)
            double wm[kGsChannels];
            for (int c = 0; c < kGsChannels; c++) wm[c] = ch[c].inView ? ch[c].warmSum / kWarmSlots : 1e30;
            for (int c = 0; c < kGsChannels; c++) {
                Chan& C = ch[c];
                if (!C.inView) continue;
                double cap = warmMin;
                if (localCap) {
                    cap = wm[c];
                    if (c > 0) cap = std::min(cap, wm[c - 1]);
                    if (c + 1 < kGsChannels) cap = std::min(cap, wm[c + 1]);
                }
                C.nf = std::min(wm[c], cap * 1.26);
            }
        }
        std::fill(acc.begin(), acc.end(), 0.0);
        framesAcc = 0; elapsed = 0;
    }

    void publish() {
        activeNow = 0;
        double dopSum = 0; int dopN = 0; float snrMax = -100;
        for (int c = 0; c < kGsChannels; c++) {
            Chan& C = ch[c];
            if (C.active) {
                activeNow++;
                if (C.dopN > 0) {
                    const double d = C.dopSum / C.dopN;
                    C.dop = d;
                    track.push_back({now, c, (float)d, C.snrMax});
                    dopSum += d; dopN++;
                }
                snrMax = std::max(snrMax, (float)C.snr);
            }
            C.dopSum = 0; C.dopN = 0; C.snrMax = 0;
        }
        while (track.size() > kGsTrackMax) track.pop_front();
        if (dopN) lastDop = dopSum / dopN;
        if (activeNow) lastSnr = snrMax;
        GlobalstarTelemetry o;
        o.timeSec = now; o.inputRate = fs; o.dialMhz = dialHz / 1e6;
        o.activeNow = activeNow;
        o.state = activeNow ? 2 : (now - lastAny < 30 ? 1 : 0);
        o.dataValid = now - lastAny < 5;
        o.cfoHz = activeNow ? lastDop : 0;
        o.snrDb = activeNow ? lastSnr : 0;
        o.blocksOk = eventsDone;
        for (int c = 0; c < kGsChannels; c++) {
            const Chan& C = ch[c];
            GsChannel g;
            g.index = c; g.centreMhz = globalstarChannelHz(c) / 1e6; g.inView = C.inView; g.active = C.active;
            if (C.inView) {
                g.powerDb = (float)(10 * std::log10(std::max(1e-12, C.power)));
                g.noiseDb = C.nf > 0 ? (float)(10 * std::log10(C.nf)) : -120.f;
                g.snrDb = C.active ? (float)C.snr : 0.f;
                g.occupancy = (float)C.occ;
                g.dopplerHz = C.dop;
                g.lastActiveSec = C.lastActive;
                g.events = C.events;
            }
            o.chan.push_back(g);
        }
        o.track.assign(track.begin(), track.end());
        o.events.assign(events.begin(), events.end());
        std::lock_guard<std::mutex> lk(telMu);
        o.seq = ++seq;
        pub = std::move(o);
    }

    // one frame of the slot has gone by (transformed or skipped)
    void frameDone(int frames) {
        now += (double)frames * NF / fs;
        elapsed += frames;
        if (elapsed >= framesPerSlot && framesAcc > 0) onSlot();
        if (now >= nextPublish) { nextPublish += kPublishSec; publish(); }
    }

    void feed(const cf32* x, size_t n) {
        if (!isReady || n == 0) return;
        size_t i = 0;
        while (i < n) {
            if (skipLeft > 0) {
                const size_t s = std::min(skipLeft, n - i);
                skipLeft -= s; i += s;
                if (skipLeft > 0) break;
                frameDone(duty - 1);
                continue;
            }
            const size_t take = std::min((size_t)NF - blkN, n - i);
            std::copy(x + i, x + i + take, blk.begin() + (long)blkN);
            blkN += take; i += take;
            if (blkN < (size_t)NF) break;
            blkN = 0;
            const float* in = reinterpret_cast<const float*>(blk.data());
            for (int k = 0; k < NF; k++) { fre[(size_t)k] = in[2 * k] * win[(size_t)k]; fim[(size_t)k] = in[2 * k + 1] * win[(size_t)k]; }
            fftSplit(fre.data(), fim.data(), log2n, false);
            for (int k = 0; k < NF; k++) acc[(size_t)k] += (double)(fre[(size_t)k] * fre[(size_t)k] + fim[(size_t)k] * fim[(size_t)k]);
            framesAcc++;
            frameDone(1);
            if (duty > 1) skipLeft = (size_t)(duty - 1) * (size_t)NF;
        }
    }
};

GlobalstarReceiver::GlobalstarReceiver() : p_(std::make_unique<Impl>()) {}
GlobalstarReceiver::~GlobalstarReceiver() = default;

void GlobalstarReceiver::configure(double inputRateHz) { std::lock_guard<std::mutex> lk(p_->procMu); p_->configure(inputRateHz); }
void GlobalstarReceiver::setSignalOffset(double hz) { std::lock_guard<std::mutex> lk(p_->procMu); p_->offsetHz = hz; if (p_->isReady) { p_->plan(); } }
void GlobalstarReceiver::setDialHz(double hz) { std::lock_guard<std::mutex> lk(p_->procMu); if (hz == p_->dialHz) return; p_->dialHz = hz; if (p_->isReady) { p_->plan(); p_->resetState(); } }
void GlobalstarReceiver::setLocalFloorCap(bool on) { std::lock_guard<std::mutex> lk(p_->procMu); p_->localCap = on; }
void GlobalstarReceiver::setWallClock(bool on) { std::lock_guard<std::mutex> lk(p_->procMu); p_->wallClock = on; }
bool GlobalstarReceiver::ready() const { std::lock_guard<std::mutex> lk(p_->procMu); return p_->isReady; }
void GlobalstarReceiver::reset() { std::lock_guard<std::mutex> lk(p_->procMu); if (p_->isReady) p_->resetState(); }
void GlobalstarReceiver::feed(const cf32* x, size_t n) { std::lock_guard<std::mutex> lk(p_->procMu); p_->feed(x, n); }
bool GlobalstarReceiver::telemetry(GlobalstarTelemetry& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> lk(p_->telMu);
    if (p_->pub.seq <= lastSeq) return false;
    out = p_->pub;
    return true;
}
void GlobalstarReceiver::clearResults() {
    std::lock_guard<std::mutex> lk(p_->procMu);
    p_->track.clear(); p_->events.clear(); p_->eventsDone = 0;
    for (auto& C : p_->ch) C.events = 0;
    std::lock_guard<std::mutex> tl(p_->telMu);
    p_->pub.track.clear(); p_->pub.events.clear(); p_->pub.blocksOk = 0;
    p_->pub.seq = ++p_->seq;
}
std::string GlobalstarReceiver::saveResults() const {
    std::lock_guard<std::mutex> lk(p_->procMu);
    return results::pack(p_->now, p_->events);
}
bool GlobalstarReceiver::loadResults(const std::string& blob) {
    double t0 = 0;
    std::deque<GsEvent> ev;
    if (!results::unpack(blob, t0, ev)) return false;
    std::lock_guard<std::mutex> lk(p_->procMu);
    for (auto& e : ev) e.startSec += p_->now - t0;
    p_->events = std::move(ev);
    p_->eventsDone = p_->events.size();
    std::lock_guard<std::mutex> tl(p_->telMu);
    p_->pub.events.assign(p_->events.begin(), p_->events.end());
    p_->pub.blocksOk = p_->eventsDone;
    p_->pub.seq = ++p_->seq;
    return true;
}
void GlobalstarReceiver::setLogCallback(std::function<void(const std::string&)> cb) { std::lock_guard<std::mutex> lk(p_->procMu); p_->log = std::move(cb); }

ModeTuning globalstarTuning() {
    ModeTuning t;
    t.stdMode = 64; t.id = "globalstar"; t.name = "Globalstar";
    t.minMhz = 1600; t.maxMhz = 1640; t.defMhz = 1618.25;
    t.sampleRate = 10000000;
    t.basebandHz = 0;
    t.bandwidthMhz = 10;
    t.minSampleRate = 3000000;
    t.tuneOffsetHz = 0;
    return t;
}

} // namespace dect2
