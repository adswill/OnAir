// Helpers shared by the DRM receiver tests: a test signal through the synthetic source, radio impairments applied on top of it, and a receiver run
// that compares every logical frame with what the transmitter sent (audio source 3 of the generator: AAC frames of a noise-like test sound, no two alike; a second
// instance of the encoder makes the reference frames).
// Signals are produced in chunks, so a few seconds at 20 Msps never sit in memory.
#pragma once
#include "dect2/drm_gen.h"
#include "dect2/drm_aacenc.h"
#include "dect2/drm_msg.h"
#include "dect2/drm_rx.h"
#include "dect2/mode_synth.h"
#include <chrono>
#include <ctime>
#include <cmath>
#include <cstdio>
#include <functional>
#include <memory>
#include <random>
#include <vector>

namespace drmtest {
using namespace dect2;

// CPU time of the calling thread: the receiver is timed with this, so that other programs on a busy machine do not change the real time factor
inline double threadCpuSecs() {
    timespec ts;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return (double)ts.tv_sec + 1e-9 * (double)ts.tv_nsec;
}

// The test signal as the synthetic source plays it: complex baseband at `rate`, rms about 0.2.
struct Sig {
    int mode = 2;            // 0 B (default), 1 A, 2 B, 3 C, 4 D
    int occ = 0;             // occupancy code + 1; 0 = 10 kHz
    int qam16 = 0, shortIl = 0, prot = 0, channel = 0, audio = 3, text = 0;
    double snrDb = 40, cfoHz = 0, sroPpm = 0, echoDb = 0;
    int echoDelay = 300;
};

// fills n samples (the stream continues from call to call)
using Gen = std::function<void(cf32* out, size_t n)>;

inline Gen synthGen(const Sig& s, double rate) {
    SynthConfig sc;
    sc.mode = 12;
    sc.snrDb = s.snrDb; sc.cfoHz = s.cfoHz; sc.sroPpm = s.sroPpm; sc.echoDb = s.echoDb; sc.echoDelay = s.echoDelay;
    sc.modeOpt[0] = s.mode; sc.modeOpt[1] = s.occ; sc.modeOpt[2] = s.qam16; sc.modeOpt[3] = s.prot; sc.modeOpt[4] = s.shortIl;
    sc.modeOpt[5] = s.audio; sc.modeOpt[6] = s.channel; sc.modeOpt[7] = s.text;
    std::shared_ptr<ModeSynth> syn = makeModeSynth(12, sc, rate);
    if (!syn) return nullptr;
    return [syn](cf32* out, size_t n) { syn->generate(out, n); };
}

// ---- radio impairments, each wraps a generator
inline Gen withDc(Gen g, cf32 dc) {
    return [g, dc](cf32* o, size_t n) { g(o, n); for (size_t i = 0; i < n; i++) o[i] += dc; };
}
// gain (dB) and phase (degrees) imbalance of the Q branch
inline Gen withIqImbalance(Gen g, double gainDb, double phaseDeg) {
    const float gq = (float)std::pow(10.0, gainDb / 20.0);
    const double ph = phaseDeg * M_PI / 180.0;
    const float c = (float)std::cos(ph), s = (float)std::sin(ph);
    return [g, gq, c, s](cf32* o, size_t n) { g(o, n); for (size_t i = 0; i < n; i++) o[i] = cf32(o[i].real(), gq * (o[i].imag() * c + o[i].real() * s)); };
}
// a steady tone at offsetHz, dbAbove dB above the rms of the signal (0.2)
inline Gen withTone(Gen g, double rate, double offsetHz, double dbAbove) {
    const float a = (float)(0.2 * std::pow(10.0, dbAbove / 20.0));
    auto ph = std::make_shared<double>(0.0);
    const double step = 2 * M_PI * offsetHz / rate;
    return [g, a, ph, step](cf32* o, size_t n) {
        g(o, n);
        for (size_t i = 0; i < n; i++) { o[i] += a * cf32(std::polar(1.0, *ph)); *ph += step; if (*ph > 2 * M_PI) *ph -= 2 * M_PI; }
    };
}
// 8 bit quantisation like a HackRF: scaled to the given rms (the signal has an rms of 0.2), rounded to 256 levels, clipped
inline Gen withQuantize8(Gen g, double rmsTarget) {
    const float gain = (float)(rmsTarget / 0.2 * 128.0);
    return [g, gain](cf32* o, size_t n) {
        g(o, n);
        for (size_t i = 0; i < n; i++) {
            const float re = std::max(-128.f, std::min(127.f, std::round(o[i].real() * gain)));
            const float im = std::max(-128.f, std::min(127.f, std::round(o[i].imag() * gain)));
            o[i] = cf32(re / 128.f, im / 128.f);
        }
    };
}
// bursts of impulse noise: every `period` samples, `len` samples of noise with the given rms
inline Gen withImpulses(Gen g, size_t period, size_t len, double rmsLevel, uint32_t seed) {
    auto pos = std::make_shared<size_t>(0);
    auto rng = std::make_shared<std::mt19937>(seed);
    return [g, period, len, rmsLevel, pos, rng](cf32* o, size_t n) {
        g(o, n);
        std::normal_distribution<float> nd(0.f, (float)(rmsLevel / std::sqrt(2.0)));
        for (size_t i = 0; i < n; i++, (*pos)++) if ((*pos) % period < len) o[i] += cf32(nd(*rng), nd(*rng));
    };
}

struct RunOpt {
    size_t chunk = 65536;
    double secs = 10;
    bool text = true;                         // the stream carries a 4 byte text message unit at the end of the logical frame
    int resetAtFrame = -1;                    // call reset() after this many logical frames
    int audioFrames = 10;                     // frames of the audio super frame (24 kHz core)
    uint32_t seed = 1;
    // stream faults in signal seconds: samples lost (the radio's stream skips) and zeroed (an overflow that was filled)
    std::vector<std::pair<double, double>> cuts, zeros;
    std::function<void(DrmReceiver&)> setup;
};

struct Result {
    DrmTelemetry tel;                         // the last report
    double lockSecs = -1;                     // signal time of the first report with data valid
    uint64_t frames = 0, exact = 0, bad = 0, gaps = 0;
    uint64_t seqBackwards = 0;
    double cpuSecs = 0, sigSecs = 0;
    int telCount = 0;
    int minState = 9, maxState = 0;
    uint64_t lostLocks = 0;                   // log lines "signal lost"
    std::vector<std::pair<double, bool>> frameLog;   // signal time at the end of the chunk that produced a logical frame, and whether it was exact
    uint64_t badAfter(double t) const { uint64_t n = 0; for (const auto& f : frameLog) if (f.first >= t && !f.second) n++; return n; }
    uint64_t exactAfter(double t) const { uint64_t n = 0; for (const auto& f : frameLog) if (f.first >= t && f.second) n++; return n; }
    std::string log;
    double rt() const { return cpuSecs > 0 ? sigSecs / cpuSecs : 0; }
};

inline Result runRx(Gen gen, double rate, const RunOpt& o = RunOpt()) {
    Result r;
    if (!gen) return r;
    DrmReceiver rx;
    rx.setSilent(true);
    rx.configure(rate);
    if (o.setup) o.setup(rx);
    rx.setLogCallback([&](const std::string& s) { r.log += s + "\n"; if (s.find("signal lost") != std::string::npos) r.lostLocks++; });
    double now = 0;                           // signal time at the end of the chunk being processed
    // the reference: the audio super frames the transmitter made (the same encoder, the same sound)
    std::unique_ptr<DrmAudioSource> refSrc;
    std::vector<std::vector<std::vector<uint8_t>>> refFrames;
    std::vector<std::vector<uint8_t>> refCrc;
    int refPayload = -1;
    long lastRef = -1;
    double cbSecs = 0;                        // time spent in this callback (the reference encoder is slow): not the receiver's
    rx.setStreamCallback([&](int, const uint8_t* b, int len, int lenA, const DrmTelemetry&) {
        struct Stop { double& acc; double t0; ~Stop() { acc += threadCpuSecs() - t0; } } stop{cbSecs, threadCpuSecs()};
        r.frames++;
        const int tb = o.text ? 4 : 0;
        drm::AacSuperFrame sf;
        if (len <= tb + 40 || !drm::aacSuperFrameParse(b, len - tb, lenA, o.audioFrames, sf) || sf.frames.empty()) { r.bad++; r.frameLog.push_back({now, false}); return; }
        const int payload = len - tb - drm::aacHeaderBytes(o.audioFrames) - o.audioFrames;
        if (!refSrc || payload != refPayload) {
            DrmTxConfig tc;
            tc.audioRateHz = o.audioFrames == 5 ? 12000 : 24000;
            tc.seed = o.seed;
            refSrc = makeDrmAacSource(tc, 3);
            refPayload = payload;
            refFrames.clear(); refCrc.clear();
            lastRef = -1;
        }
        // find this super frame in the reference: the next one is the likely match
        long found = -1;
        auto matches = [&](long j) {
            while ((size_t)j >= refFrames.size() && refFrames.size() < 4000) {
                std::vector<std::vector<uint8_t>> fr; std::vector<uint8_t> crc;
                refSrc->nextSuperFrame(o.audioFrames, payload, fr, crc);
                refFrames.push_back(std::move(fr)); refCrc.push_back(std::move(crc));
            }
            return (size_t)j < refFrames.size() && sf.frames == refFrames[(size_t)j] && sf.crc == refCrc[(size_t)j];
        };
        for (long j = lastRef + 1; j < lastRef + 60 && found < 0; j++) if (matches(j)) found = j;
        if (found < 0 && lastRef < 0) for (long j = 0; j < 100 && found < 0; j++) if (matches(j)) found = j;
        if (found >= 0) r.exact++; else r.bad++;
        r.frameLog.push_back({now, found >= 0});
        if (found >= 0) {
            if (lastRef >= 0 && found != lastRef + 1) r.gaps++;
            lastRef = found;
        }
    });
    uint64_t seq = 0;
    const size_t total = (size_t)(o.secs * rate);
    std::vector<cf32> buf(o.chunk);
    bool didReset = false;
    size_t done = 0;                          // samples of the signal produced so far
    auto inRange = [&](const std::vector<std::pair<double, double>>& v, size_t i) {
        for (const auto& c : v) if (i >= (size_t)(c.first * rate) && i < (size_t)((c.first + c.second) * rate)) return true;
        return false;
    };
    while (done < total) {
        const size_t n = std::min(o.chunk, total - done);
        gen(buf.data(), n);
        // faults: zero the samples, then leave out the ones that are lost
        size_t kept = 0;
        for (size_t i = 0; i < n; i++) {
            if (!o.zeros.empty() && inRange(o.zeros, done + i)) buf[i] = cf32(0, 0);
            if (!o.cuts.empty() && inRange(o.cuts, done + i)) continue;
            buf[kept++] = buf[i];
        }
        done += n;
        now = (double)done / rate;
        const double t0 = threadCpuSecs();
        rx.feed(buf.data(), kept);
        r.cpuSecs += threadCpuSecs() - t0;
        DrmTelemetry t;
        if (rx.telemetry(t, seq)) {
            if (t.seq < seq) r.seqBackwards++;
            seq = t.seq;
            r.tel = t;
            r.telCount++;
            r.minState = std::min(r.minState, t.state); r.maxState = std::max(r.maxState, t.state);
            if (r.lockSecs < 0 && t.dataValid) r.lockSecs = (double)done / rate;
        }
        if (o.resetAtFrame >= 0 && !didReset && r.frames >= (uint64_t)o.resetAtFrame) { rx.reset(); didReset = true; lastRef = -1; }
    }
    r.cpuSecs = std::max(1e-6, r.cpuSecs - cbSecs);
    r.sigSecs = (double)total / rate;
    return r;
}

} // namespace drmtest
