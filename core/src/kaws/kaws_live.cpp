// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// KAWS for the engine: see kaws_live.h.
#include "dect2/kaws/kaws_live.h"
#include "dect2/kaws/kaws_arq.h"
#include "dect2/kaws/kaws_frame.h"
#include "dect2/kaws/kaws_phy.h"
#include "dect2/kaws/kaws_sched.h"
#include "dect2/kaws/kaws_tel.h"
#include "dect2/marine/marine_dsp.h"
#include "dect2/dsp/fftutil.h"
#include "dect2/common/mathconst.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <thread>

namespace dect2 {

namespace {

using dect2::math::kPi;
constexpr int kAud = kaws::kAudioRate;                      // 12000
constexpr double kTailSec = 3.0;                           // a burst may end up to 3 s after its slot (clock error)
constexpr int kMaxList = 64;
constexpr double kRepeatWindowSec = 1200.0;                // an identical message this soon after one delivered is a repeat
constexpr double kPartialTimeoutSec = 900.0;
constexpr int kSlotted = 6;                                // profiles 0 .. 5 have slots
constexpr size_t kSpecFft = 4096;
constexpr int kSpecBins = 60;

struct AudioDecim {   // 24 kHz complex (upper sideband, dial at 0 Hz) -> 12 kHz real audio
    static constexpr int kTaps = 63;
    std::vector<float> taps, hist;
    int pos = 0;
    bool odd = false;
    void configure() {
        taps.assign(kTaps, 0.f);
        const double fc = 4800.0 / 24000.0;   // cycles per sample, the passband is 0 .. 3 kHz
        double sum = 0;
        for (int i = 0; i < kTaps; i++) {
            const double x = i - (kTaps - 1) / 2.0;
            const double s = x == 0 ? 2 * fc : std::sin(2 * kPi * fc * x) / (kPi * x);
            const double w = 0.42 - 0.5 * std::cos(2 * kPi * i / (kTaps - 1)) + 0.08 * std::cos(4 * kPi * i / (kTaps - 1));
            taps[(size_t)i] = (float)(s * w);
            sum += s * w;
        }
        for (float& t : taps) t = (float)(t / sum);
        hist.assign((size_t)kTaps * 2, 0.f);
        pos = 0; odd = false;
    }
    void process(const std::vector<cf32>& in, std::vector<float>& out) {
        for (const cf32& v : in) {
            float re = v.real();
            if (!std::isfinite(re)) re = 0;
            hist[(size_t)pos] = re; hist[(size_t)(pos + kTaps)] = re;
            if (++pos >= kTaps) pos = 0;
            odd = !odd;
            if (odd) continue;
            const float* w = &hist[(size_t)pos];
            float a = 0;
            for (int k = 0; k < kTaps; k++) a += taps[(size_t)k] * w[k];
            out.push_back(a);
        }
    }
};

double systemUtc() {
    return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
}

uint64_t fnv(const kaws::Bits& m) {
    uint64_t h = 1469598103934665603ull;
    for (uint8_t b : m) { h ^= b; h *= 1099511628211ull; }
    return h;
}

struct Job {
    int profile = 0;
    double windowStartUtc = 0, endStreamSec = 0;
    std::vector<float> audio;
};

} // namespace

struct KawsLiveReceiver::Impl {
    // ---- the feed thread
    std::mutex feedMu;
    marine::Front front;
    AudioDecim dec;
    double rate = 0, offsetHz = 0;
    bool ready = false;
    uint64_t seq = 0, samples = 0, nextReport = 0;
    std::vector<cf32> base;
    std::vector<float> chunk;
    bool anchored = false;
    double t0 = 0;                           // UTC of audio sample 0
    uint64_t audioN = 0;                     // audio samples produced since t0
    uint64_t ringBase = 0;                   // absolute audio index of aud[0]
    std::vector<float> aud;
    int64_t nextSlot[kSlotted] = {};
    std::function<double()> clock;
    bool clockSet = false;
    std::function<void(const std::string&)> log;
    std::mutex pubMu;
    KawsTelemetry pub;
    std::vector<float> spec;
    std::unique_ptr<Fft> fft;
    std::vector<float> win;

    // ---- the worker
    std::mutex jobMu;
    std::condition_variable jobCv, idleCv;
    std::deque<Job> jobs;
    bool busy = false, quit = false;
    std::thread worker;
    uint64_t generation = 0;                 // changes on a reset: results of an older stream are dropped

    // ---- results (worker writes, the reporter reads)
    std::mutex resMu;
    KawsTelemetry cur;
    kaws::ArqReceiver arq{"-"};
    std::map<uint8_t, double> partialSec;
    struct Done { uint64_t h; double sec; };
    std::deque<Done> done;
    struct Pending { double burstUtc, jobEndStream; };
    std::deque<Pending> pendingBad;
    double lastGoodStream = -1e9;
    double nowStream = 0;                    // stream seconds, written by the feed thread

    Impl() { clock = systemUtc; }
    ~Impl() {
        { std::lock_guard<std::mutex> lk(jobMu); quit = true; }
        jobCv.notify_all();
        if (worker.joinable()) worker.join();
    }

    double utc() { return clock ? clock() : systemUtc(); }

    void startStream() {   // under feedMu
        front.configure(rate);
        front.setOffsetHz(offsetHz);
        dec.configure();
        base.clear(); chunk.clear(); aud.clear();
        anchored = false; audioN = 0; ringBase = 0; samples = 0;
        nextReport = (uint64_t)(0.25 * rate);
        win.assign(kSpecFft, 0.f);
        for (size_t i = 0; i < kSpecFft; i++) win[i] = (float)(0.5 - 0.5 * std::cos(2 * kPi * (double)i / (double)(kSpecFft - 1)));
        fft = std::make_unique<Fft>((int)kSpecFft);
        spec.assign(kSpecBins, -60.f);
        {
            std::lock_guard<std::mutex> lk(jobMu);
            jobs.clear();
            generation++;
        }
        std::lock_guard<std::mutex> rk(resMu);
        pendingBad.clear();
        lastGoodStream = -1e9;
        nowStream = 0;
        for (int p = 0; p < 8; p++) { cur.latticeSeen[p] = false; cur.latticeStat[p] = 0; cur.latticeAgeSec[p] = -1; }
        cur.profile = -1; cur.snrDb = -99; cur.clockErrSec = 0;
    }

    void ensureWorker() {
        if (worker.joinable()) return;
        worker = std::thread([this] { run(); });
    }

    void enqueue(Job&& j) {
        ensureWorker();
        {
            std::lock_guard<std::mutex> lk(jobMu);
            if (jobs.size() >= 32) jobs.pop_front();   // the search cannot keep up: the oldest slot is dropped
            jobs.push_back(std::move(j));
        }
        jobCv.notify_one();
    }

    void run() {
        for (;;) {
            Job j;
            uint64_t gen;
            {
                std::unique_lock<std::mutex> lk(jobMu);
                jobCv.wait(lk, [this] { return quit || !jobs.empty(); });
                if (quit) return;
                j = std::move(jobs.front());
                jobs.pop_front();
                gen = generation;
                busy = true;
            }
            process(j, gen);
            {
                std::lock_guard<std::mutex> lk(jobMu);
                busy = false;
            }
            idleCv.notify_all();
        }
    }

    void say(const std::string& s) {
        std::function<void(const std::string&)> lg;
        { std::lock_guard<std::mutex> pk(pubMu); lg = log; }
        if (lg) lg(s);
    }

    // The search of one slot window of one profile (worker thread).
    void process(const Job& j, uint64_t gen) {
        using namespace kaws;
        const int p = j.profile;
        const int K = kProfiles[p].K;
        const std::vector<cf> iq = fromAudio12k(j.audio);
        if (iq.size() < 12000) return;
        const RxBurst r1 = receiveBurst(p, 1, iq, kSearchLo, kSearchHi);
        std::lock_guard<std::mutex> rk(resMu);
        {
            std::lock_guard<std::mutex> jk(jobMu);
            if (gen != generation) return;
        }
        cur.slotsTried++;
        cur.latticeSeen[p] = r1.sync;
        cur.latticeStat[p] = (float)r1.stat;
        cur.latticeAgeSec[p] = j.endStreamSec;   // converted to an age when reported
        if (!r1.sync) return;
        cur.latticeHits++;
        const double burstUtc = j.windowStartUtc + r1.tau / kBaseRate;
        uint8_t msgId = 0, index = 0;
        Bits payload;
        const bool good0 = r1.iters[0] >= 0 && parseBlock(K, r1.blocks[0], msgId, index, payload);
        if (!good0) {
            pendingBad.push_back(Pending{burstUtc, j.endStreamSec});
            return;
        }
        int n = std::max(1, blocksPerSlot(p));
        if (index == 0) {
            const size_t bits = expectedMessageBits(payload);
            if (bits > 0) n = std::min(n, std::max(1, (int)((bits + (size_t)usableBits(p) - 1) / (size_t)usableBits(p))));
        }
        RxBurst r = r1;
        if (n > 1) r = receiveBurst(p, n, iq, kSearchLo, kSearchHi);
        if (!r.sync || (int)r.blocks.size() != n) r = r1, n = 1;
        // a burst of this profile was found: a try of another profile on the same burst is not a failure
        for (auto it = pendingBad.begin(); it != pendingBad.end();) {
            if (it->burstUtc > burstUtc - 6.0 && it->burstUtc < burstUtc + (double)burstSamples(p, n) / kBaseRate) it = pendingBad.erase(it); else ++it;
        }
        int lastGood = -1;
        std::vector<int> ok((size_t)n, 0);
        for (int b = 0; b < n; b++) {
            uint8_t mi, ix;
            Bits pl;
            if (r.iters[(size_t)b] >= 0 && parseBlock(K, r.blocks[(size_t)b], mi, ix, pl)) { ok[(size_t)b] = 1; lastGood = b; }
        }
        std::vector<uint8_t> touched;
        for (int b = 0; b < n; b++) {
            if (ok[(size_t)b]) {
                uint8_t mi, ix;
                Bits pl;
                parseBlock(K, r.blocks[(size_t)b], mi, ix, pl);
                arq.onBlock(K, r.blocks[(size_t)b]);
                partialSec[mi] = j.windowStartUtc;
                if (std::find(touched.begin(), touched.end(), mi) == touched.end()) touched.push_back(mi);
                cur.blocksOk++;
                cur.profileBlocks[p]++;
            } else if (b < lastGood) {
                cur.blocksBad++;   // undecoded blocks after the last good one may simply not have been sent
            }
        }
        cur.profile = p;
        cur.snrDb = (float)r.snrDb;
        cur.cfoHz = (float)r.deltaHz;
        cur.clockErrSec = clockErrorSec(r.tau);
        lastGoodStream = j.endStreamSec;
        const double utc = j.windowStartUtc + (double)j.audio.size() / kAud;
        for (uint8_t id : touched) {
            if (!arq.complete(id)) continue;
            const Bits m = arq.message(id);
            arq.forget(id);
            partialSec.erase(id);
            deliver(m, p, (float)r.snrDb, utc);
        }
        for (auto it = partialSec.begin(); it != partialSec.end();) {
            if (j.windowStartUtc - it->second > kPartialTimeoutSec) { arq.forget(it->first); it = partialSec.erase(it); } else ++it;
        }
    }

    void deliver(const kaws::Bits& m, int p, float snr, double utc) {
        using namespace kaws;
        while (!done.empty() && utc - done.front().sec > kRepeatWindowSec) done.pop_front();
        const uint64_t h = fnv(m);
        for (const Done& d : done) if (d.h == h) return;   // a repeat of a message already delivered
        done.push_back(Done{h, utc});
        switch (frameTypeOf(m)) {
        case FrameType::Chat: {
            Chat c;
            if (!decodeChat(m, c)) return;
            KawsChat e;
            e.from = c.from; e.text = c.text; e.utc = utc; e.profile = p; e.snrDb = snr; e.firstMessage = c.firstMessage;
            if (c.firstMessage) e.to = c.to;
            else { char b[24]; snprintf(b, sizeof b, "#%06X", (unsigned)c.toHash); e.to = b; }
            cur.chats.insert(cur.chats.begin(), e);
            if (cur.chats.size() > (size_t)kMaxList) cur.chats.resize(kMaxList);
            cur.messages++;
            say("KAWS message from " + e.from);
            break;
        }
        case FrameType::BeaconShort: {
            BeaconShort b;
            if (!decodeBeaconShort(m, b)) return;
            KawsBeaconEntry e;
            e.call = b.call; e.grid = b.grid; e.powerDbm = -40 + 0.5 * b.powerStep; e.utc = utc; e.profile = p; e.snrDb = snr;
            cur.beaconList.insert(cur.beaconList.begin(), e);
            if (cur.beaconList.size() > (size_t)kMaxList) cur.beaconList.resize(kMaxList);
            cur.beacons++;
            say("KAWS beacon from " + e.call);
            break;
        }
        case FrameType::BeaconFull: {
            BeaconFull b;
            if (!decodeBeaconFull(m, b)) return;
            KawsBeaconEntry e;
            e.call = b.call; e.grid = b.grid; e.text = b.text; e.full = true; e.lat = b.lat; e.lon = b.lon;
            e.powerDbm = -40 + 0.5 * b.powerStep; e.utc = utc; e.profile = p; e.snrDb = snr;
            cur.beaconList.insert(cur.beaconList.begin(), e);
            if (cur.beaconList.size() > (size_t)kMaxList) cur.beaconList.resize(kMaxList);
            cur.beacons++;
            say("KAWS beacon from " + e.call);
            break;
        }
        case FrameType::Data:
            cur.dataFrames++;
            break;
        }
    }

    // Window of one slot of profile p ready: copy the audio and hand it to the worker (feed thread).
    void cutSlots() {
        const double now = t0 + (double)audioN / kAud;
        for (int p = 0; p < kSlotted; p++) {
            const kaws::SlotGrid g(kaws::kProfiles[p].slotSec);
            for (;;) {
                const int64_t idx = nextSlot[p];
                const double wStart = g.windowStart(idx), wEnd = g.start(idx) + g.slotSec + kTailSec;
                if (now < wEnd) break;
                nextSlot[p]++;
                const long long a0 = std::llround((wStart - t0) * kAud), a1 = std::llround((wEnd - t0) * kAud);
                if (a0 < (long long)ringBase || a1 > (long long)(ringBase + aud.size()) || a1 <= a0) continue;
                Job j;
                j.profile = p;
                j.windowStartUtc = wStart;
                j.endStreamSec = (double)audioN / kAud;
                j.audio.assign(aud.begin() + (a0 - (long long)ringBase), aud.begin() + (a1 - (long long)ringBase));
                enqueue(std::move(j));
            }
        }
        // keep only the audio that a later window may still need
        double keepFrom = now;
        for (int p = 0; p < kSlotted; p++) keepFrom = std::min(keepFrom, kaws::SlotGrid(kaws::kProfiles[p].slotSec).windowStart(nextSlot[p]));
        const long long k = std::llround((keepFrom - t0) * kAud) - 2 * kAud;
        if (k > (long long)ringBase) {
            const size_t drop = std::min<size_t>((size_t)(k - (long long)ringBase), aud.size());
            aud.erase(aud.begin(), aud.begin() + (long)drop);
            ringBase += drop;
        }
    }

    void spectrum() {
        if (aud.size() < kSpecFft || !fft) return;
        std::vector<cf32> x(kSpecFft);
        const size_t s0 = aud.size() - kSpecFft;
        for (size_t i = 0; i < kSpecFft; i++) x[i] = cf32(aud[s0 + i] * win[i], 0.f);
        fft->forward(x.data());
        const double binHz = (double)kAud / (double)kSpecFft;
        std::vector<float> pw(kSpecBins, 1e-20f);
        for (size_t k = 1; k < kSpecFft / 2; k++) {
            const double f = (double)k * binHz;
            if (f >= 3000) break;
            const int b = std::min(kSpecBins - 1, (int)(f / 50.0));
            pw[(size_t)b] = std::max(pw[(size_t)b], std::norm(x[k]));
        }
        float mx = 1e-20f;
        for (float v : pw) mx = std::max(mx, v);
        for (int b = 0; b < kSpecBins; b++) spec[(size_t)b] = std::max(-60.f, 10.f * std::log10(pw[(size_t)b] / mx));
    }

    void report() {   // under feedMu
        const double nowS = (double)audioN / kAud;
        spectrum();
        KawsTelemetry o;
        {
            std::lock_guard<std::mutex> rk(resMu);
            nowStream = nowS;
            for (auto it = pendingBad.begin(); it != pendingBad.end();) {
                if (nowS - it->jobEndStream > 130.0) { cur.blocksBad++; it = pendingBad.erase(it); } else ++it;
            }
            o = cur;
            o.lastGoodAgeSec = lastGoodStream > -1e8 ? std::max(0.0, nowS - lastGoodStream) : 0.0;
            o.state = samples == 0 ? 0 : (lastGoodStream > -1e8 && nowS - lastGoodStream <= 300.0) ? 2 : 1;
            o.dataValid = o.state == 2 && o.blocksOk > 0;
            o.syncLock = false;
            for (int p = 0; p < kSlotted; p++) {
                if (o.latticeAgeSec[p] >= 0) {
                    o.latticeAgeSec[p] = std::max(0.0, nowS - o.latticeAgeSec[p]);
                    if (o.latticeSeen[p] && o.latticeAgeSec[p] <= 2.0 * kaws::kProfiles[p].slotSec) o.syncLock = true;
                }
            }
        }
        o.seq = ++seq;
        o.inputRate = rate;
        o.timeSec = nowS;
        o.utcNow = anchored ? t0 + nowS : utc();
        o.clockSet = clockSet;
        o.spectrum = spec;
        o.voicePresent = false;
        o.voiceNote = "Voice (P6, P7) is not received here. A speech codec is installed by the user (docs/CODECS.md); none is loaded: voice codec not installed.";
        std::lock_guard<std::mutex> pk(pubMu);
        pub = std::move(o);
    }
};

KawsLiveReceiver::KawsLiveReceiver() : p_(std::make_unique<Impl>()) {}
KawsLiveReceiver::~KawsLiveReceiver() = default;

void KawsLiveReceiver::configure(double r) {
    Impl& m = *p_;
    std::lock_guard<std::mutex> lk(m.feedMu);
    m.rate = r;
    m.ready = r >= kawsTuning().minSampleRate - 1;
    if (m.ready) m.startStream();
    std::lock_guard<std::mutex> pk(m.pubMu);
    m.pub = KawsTelemetry();
    m.pub.seq = ++m.seq;
    m.pub.inputRate = r;
}

void KawsLiveReceiver::setSignalOffset(double hz) {
    Impl& m = *p_;
    std::lock_guard<std::mutex> lk(m.feedMu);
    m.offsetHz = hz;
    if (m.rate > 0) m.front.setOffsetHz(hz);
}

bool KawsLiveReceiver::ready() const {
    std::lock_guard<std::mutex> lk(p_->feedMu);
    return p_->ready;
}

void KawsLiveReceiver::reset() {
    Impl& m = *p_;
    std::lock_guard<std::mutex> lk(m.feedMu);
    if (m.ready) m.startStream();
    std::lock_guard<std::mutex> pk(m.pubMu);
    m.pub.seq = ++m.seq;
}

void KawsLiveReceiver::clearLists() {
    Impl& m = *p_;
    std::lock_guard<std::mutex> rk(m.resMu);
    m.cur.chats.clear(); m.cur.beaconList.clear();
    m.cur.messages = m.cur.beacons = m.cur.dataFrames = 0;
    m.cur.blocksOk = m.cur.blocksBad = 0;
    for (auto& v : m.cur.profileBlocks) v = 0;
    m.arq = kaws::ArqReceiver("-");
    m.partialSec.clear();
    m.done.clear();
}

void KawsLiveReceiver::setClock(std::function<double()> utcNow) {
    Impl& m = *p_;
    std::lock_guard<std::mutex> lk(m.feedMu);
    m.clockSet = (bool)utcNow;
    m.clock = utcNow ? std::move(utcNow) : std::function<double()>(systemUtc);
}

void KawsLiveReceiver::flush() {
    Impl& m = *p_;
    std::unique_lock<std::mutex> lk(m.jobMu);
    m.idleCv.wait(lk, [&m] { return m.jobs.empty() && !m.busy; });
}

void KawsLiveReceiver::feed(const cf32* x, size_t n) {
    Impl& m = *p_;
    if (n == 0) return;
    std::lock_guard<std::mutex> lk(m.feedMu);
    if (!m.ready) return;
    m.samples += n;
    m.base.clear();
    m.front.process(x, n, m.base);
    m.chunk.clear();
    m.dec.process(m.base, m.chunk);
    if (!m.chunk.empty()) {
        if (!m.anchored) {   // the first audio sample is as old as the chunk: stamp it with the clock now
            m.anchored = true;
            m.t0 = m.utc() - (double)m.chunk.size() / kAud;
            m.audioN = 0; m.ringBase = 0;
            for (int p = 0; p < kSlotted; p++) m.nextSlot[p] = kaws::SlotGrid(kaws::kProfiles[p].slotSec).nextIndex(m.t0 + kaws::kWindowBeforeSec);
        }
        m.aud.insert(m.aud.end(), m.chunk.begin(), m.chunk.end());
        m.audioN += m.chunk.size();
        m.cutSlots();
    }
    if (m.samples >= m.nextReport) {
        m.nextReport = m.samples + (uint64_t)(0.25 * m.rate);
        m.report();
    }
}

bool KawsLiveReceiver::telemetry(KawsTelemetry& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> lk(p_->pubMu);
    if (p_->pub.seq == 0 || p_->pub.seq == lastSeq) return false;
    out = p_->pub;
    return true;
}

void KawsLiveReceiver::setLogCallback(std::function<void(const std::string&)> cb) { std::lock_guard<std::mutex> lk(p_->pubMu); p_->log = std::move(cb); }

ModeTuning kawsTuning() {
    ModeTuning t;
    t.stdMode = 135; t.id = "kaws"; t.name = "KAWS";
    t.minMhz = 1.8; t.maxMhz = 30; t.defMhz = 14.1;   // upper sideband dial frequency; the mode has no allocated frequency
    t.sampleRate = 2000000;
    t.basebandHz = 1750000;
    t.bandwidthMhz = 0.0024;
    t.minSampleRate = 250000;
    t.tuneOffsetHz = 20000;   // as MT63: the radio sits 20 kHz above the dial, away from its DC spike
    return t;
}

} // namespace dect2
