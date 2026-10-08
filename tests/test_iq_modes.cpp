// "Remove DC spike" and "IQ correction" (iq_correct.h) against the receiver of every mode: they must never make a mode worse.
//
// For every mode its own test signal goes straight into its own receiver, with the corrections applied the way the engine applies them
// (blocks of 65536 samples, the engine's per-mode rule of what to skip):
//   clean     the wanted carrier exactly on the centre, and 5, 50 and 500 Hz from it: corrections off / DC removal / DC removal + IQ correction
//   impaired  the same signal (on the centre) with a DC offset 15 dB below the signal and a 1 dB / 5 degree IQ imbalance: off / both on
// The mode's own measure (frames, messages or blocks with a good check; SNR, MER or C/N0; audio S/N) with a correction on must not fall
// below the one without beyond noise. With the impairment the corrections must also take the DC and the mirror image out of the samples
// (unless the mode skips that part), and in the engine the HackRF-style spike must leave the centre bin of the spectrum in every mode whose
// DC removal is not skipped (and stay where it is skipped).
//
// Modes with a tuneOffsetHz run as the engine runs them with a radio (the channel tuneOffsetHz from the centre) and as a recording holds them
// (the channel on the centre: a file is played without the offset). Usage: test_iq_modes [part of a mode name]
#include "dect2/engine.h"
#include "dect2/engine_testkit.h"
#include "dect2/iq_correct.h"
#include "dect2/mode_synth.h"
#include "dect2/mode_tuning.h"
#include "dect2/acars_gen.h"
#include "dect2/atsc_gen.h"
#include "dect2/dvbt_gen.h"
#include "dect2/fm_gen.h"
#include "dect2/gen_util.h"
#include "dect2/marine_gen.h"
#include "dect2/t2.h"
#include "dect2/t2gen.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace dect2;

static std::atomic<int> fails{0};
static std::mutex outMu;
#define FAILF(...) do { std::lock_guard<std::mutex> lk_(outMu); printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } while (0)

static double wallNow() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

// ---------------------------------------------------------------- the rule the engine follows (engine.cpp, next to its two iqFix_ calls)

static bool keepDcFor(int stdMode) {
    if (stdMode == 7) return true;                       // FM: a station tuned to the centre has its carrier there
    const ModeTuning* mt = modeTuning(stdMode);
    return mt && mt->carrierAtCentre;
}
static bool keepIqFor(int stdMode) {
    const ModeTuning* mt = modeTuning(stdMode);
    return mt && mt->notCircular;
}

// ---------------------------------------------------------------- signals

using Sig = std::vector<cf32>;

static double rmsOf(const Sig& v) {
    double p = 0;
    for (const auto& x : v) p += std::norm(x);
    return std::sqrt(p / (double)std::max<size_t>(1, v.size()));
}
static void scaleTo(Sig& v, double rms) {
    const float g = (float)(rms / std::max(1e-12, rmsOf(v)));
    for (auto& x : v) x *= g;
}
static void addNoise(Sig& v, double snrDb, uint32_t seed) {   // noise against the power of v, over the whole band
    const double p = rmsOf(v) * rmsOf(v);
    genutil::NoiseSource ns(seed);
    ns.add(v.data(), v.size(), (float)std::sqrt(p / std::pow(10.0, snrDb / 10) / 2));
}
static void mix(Sig& v, double rate, double hz) {             // moves the whole signal by hz
    if (hz == 0) return;
    const double dph = 2 * M_PI * hz / rate;
    for (size_t o = 0; o < v.size(); o += 4096) {
        std::complex<double> r = std::polar(1.0, std::remainder(dph * (double)o, 2 * M_PI)), st = std::polar(1.0, dph);
        const size_t e = std::min(v.size(), o + 4096);
        for (size_t i = o; i < e; i++) { v[i] = cf32(std::complex<double>(v[i]) * r); r *= st; }
    }
}
static Sig synthEx(std::unique_ptr<ModeSynth> s, size_t n) {
    Sig v(n);
    if (!s) { FAILF("no generator"); return v; }
    for (size_t o = 0; o < n; o += 65536) s->generate(v.data() + o, std::min<size_t>(65536, n - o));
    return v;
}
static Sig synth(int stdMode, const SynthConfig& c, double rate, size_t n) { return synthEx(makeModeSynth(stdMode, c, rate), n); }

// The clean signal of a mode is kept as 16 bit integers (half the memory of floats; the rounding is 80 dB below the signal)
struct Base {
    std::vector<std::complex<int16_t>> v;
    float scale = 1;          // value = int / scale
    double rms = 0;
    size_t size() const { return v.size(); }
    cf32 at(size_t i) const { return cf32(v[i].real() / scale, v[i].imag() / scale); }
};
static std::shared_ptr<const Base> toBase(const Sig& s) {
    auto b = std::make_shared<Base>();
    float peak = 1e-9f;
    for (const auto& x : s) peak = std::max(peak, std::max(std::fabs(x.real()), std::fabs(x.imag())));
    b->scale = 32000.f / peak;
    b->rms = rmsOf(s);
    b->v.resize(s.size());
    for (size_t i = 0; i < s.size(); i++) b->v[i] = std::complex<int16_t>((int16_t)std::lround(s[i].real() * b->scale), (int16_t)std::lround(s[i].imag() * b->scale));
    return b;
}

// ---------------------------------------------------------------- one run: the signal through the corrections into a sink

struct Setup {
    double f0 = 0;          // extra carrier offset, Hz
    bool dc = false, iq = false;
    bool impaired = false;  // DC offset and IQ imbalance added before the corrections
    bool imbalance = true;  // with impaired: the IQ imbalance too (the spectrum check adds the DC alone)
};

struct Residual {           // the impairment before and after the corrections, dB against the signal power
    double dcIn = 0, dcOut = 0, imgIn = 0, imgOut = 0;
    double gainDb = 0, phaseDeg = 0;   // what the IQ correction measured (injected: 1 dB, 5 degrees)
};

constexpr double kDcDb = -15;                 // the injected DC against the signal power
constexpr double kImbDb = 1.0, kImbDeg = 5.0;
constexpr size_t kBlock = 65536;              // the engine reads the ring in blocks of this size

using Sink = std::function<void(const cf32*, size_t)>;
using Play = std::function<void(const Sink&)>;

// res: the residual is measured over the last quarter, as the output against the clean signal through the same DC removal (what the
// DC removal does to the signal itself is not counted: that part shows in the receiver's measure)
static void playThrough(const Base& base, double rate, const Setup& s, bool keepDc, bool keepIq, const Sink& sink, Residual* res, double dcDb = kDcDb) {
    IqCorrector cor, ref;
    cor.dc = s.dc; cor.iq = s.iq;
    ref.dc = s.dc;
    const float dcA = (float)(base.rms * std::pow(10.0, dcDb / 20));
    const cf32 dc(0.8f * dcA, -0.6f * dcA);
    const float g = (float)std::pow(10.0, kImbDb / 20), cph = (float)std::cos(kImbDeg * M_PI / 180), sph = (float)std::sin(kImbDeg * M_PI / 180);
    const double dph = 2 * M_PI * s.f0 / rate;
    const size_t tail = base.size() * 3 / 4;
    std::vector<cf32> x(kBlock), y(kBlock), yin(kBlock);
    // over the tail: sums for a least-squares complex gain c (z = c x + e): a change of gain or phase is not an impairment
    struct Fit { std::complex<double> zx = 0, z = 0, x = 0; double zz = 0, xx = 0; };
    Fit fIn, fOut;
    size_t nt = 0;
    for (size_t o = 0; o < base.size(); o += kBlock) {
        const size_t n = std::min(kBlock, base.size() - o);
        if (s.f0 != 0) {
            std::complex<double> r = std::polar(1.0, std::remainder(dph * (double)o, 2 * M_PI)), st = std::polar(1.0, dph);
            for (size_t i = 0; i < n; i++) {
                x[i] = cf32(std::complex<double>(base.at(o + i)) * r); r *= st;
                if ((i & 1023) == 1023) r = std::polar(1.0, std::remainder(dph * (double)(o + i + 1), 2 * M_PI));
            }
        } else for (size_t i = 0; i < n; i++) x[i] = base.at(o + i);
        if (s.impaired && s.imbalance) for (size_t i = 0; i < n; i++) y[i] = cf32(x[i].real() + dc.real(), g * (x[i].imag() * cph + x[i].real() * sph) + dc.imag());
        else if (s.impaired) for (size_t i = 0; i < n; i++) y[i] = x[i] + dc;
        else std::copy(x.begin(), x.begin() + (long)n, y.begin());
        if (res) std::copy(y.begin(), y.begin() + (long)n, yin.begin());
        cor.process(y.data(), n, keepDc, keepIq);
        if (res) {
            ref.process(x.data(), n, keepDc, keepIq);   // the clean signal through the DC removal alone
            if (o + n > tail) {
                for (size_t i = o >= tail ? 0 : tail - o; i < n; i++) {
                    const std::complex<double> xi(x[i]), zi(yin[i]), zo(y[i]);
                    for (int k = 0; k < 2; k++) {
                        Fit& f = k ? fOut : fIn;
                        const std::complex<double> z = k ? zo : zi;
                        f.zx += z * std::conj(xi); f.z += z; f.x += xi; f.zz += std::norm(z); f.xx += std::norm(xi);
                    }
                    nt++;
                }
            }
        }
        sink(y.data(), n);
    }
    if (res && nt && fIn.xx > 0) {
        auto db = [](double v) { return 10 * std::log10(std::max(v, 1e-15)); };
        const double N = (double)nt, P = fIn.xx / N;
        // the DC: mean(z) - mean(x); the image: what is left of z - c x around their means, c fitted there (a kept DC is not scaled with the signal)
        auto split = [&](const Fit& f, double& dcDb, double& imgDb) {
            const std::complex<double> mz = f.z / N, mx = f.x / N;
            const std::complex<double> czx = f.zx / N - mz * std::conj(mx);
            const double vz = f.zz / N - std::norm(mz), vx = f.xx / N - std::norm(mx);
            dcDb = db(std::norm(mz - mx) / P);
            imgDb = db(vx > 0 ? (vz - std::norm(czx) / vx) / P : 0);
        };
        split(fIn, res->dcIn, res->imgIn);
        split(fOut, res->dcOut, res->imgOut);
        res->gainDb = cor.gainDb(); res->phaseDeg = cor.phaseDeg();
    }
}

// ---------------------------------------------------------------- what a receiver gives back

struct Metric {
    double q = 0;           // the main measure: frames, blocks or messages with a good check, or a share of them in % (more is better)
    double bad = 0;         // the ones that failed their check (shown, not compared)
    double snr = NAN;       // SNR, MER or C/N0 the receiver reports, dB
    double aux = NAN;       // a second quality in dB where the mode has one (analog TV: the sound's S/N)
    std::string info;
};

struct Mode {
    std::string name;
    int stdMode = 0;
    double rate = 2e6;
    double secs = 1;
    std::function<Sig(size_t n)> gen;                       // the clean signal, the wanted carrier exactly on the centre (or where the radio has it)
    std::function<Metric(const Play&, double rate)> rx;     // the mode's receiver, set up as the engine sets it up
    double minQ = 1;                                        // the clean signal must give at least this (or the comparison means nothing)
    double tolAbs = 1, tolRel = 0.03, tolSnr = 1.0;         // "beyond noise": the loss of q (absolute or relative) and of snr / aux (dB) allowed
    bool qIsDb = false;                                     // q is a ratio in dB or a share in %: only tolAbs applies
    bool radio = true;                                      // the representative of its mode in the engine's spectrum check
    // results
    Metric clean[4][3];
    Metric impOff, impOn;
    Residual res;
    double cpu = 0;
    int maxJobs = 14;                                       // receiver runs of this mode at once (the big receivers hold a lot of memory)
    int pending = 0, running = 0;                           // receiver runs not finished yet, and running now
};

static std::string fmt(const Metric& r, bool qIsDb);
static const double kOffsets[4] = {0, 5, 50, 500};
static const char* kCfgName[3] = {"off", "DC", "DC+IQ"};

// ---------------------------------------------------------------- the receivers

template <class Tel>
static Metric fromTel(const Tel& t) { Metric m; m.q = (double)t.blocksOk; m.bad = (double)t.blocksBad; m.snr = t.snrDb; return m; }

template <class Rx, class Tel, class Prep, class Done>
static Metric runSimple(const Play& p, double rate, Prep prep, Done done) {
    Rx rx;
    prep(rx, rate);
    p([&](const cf32* x, size_t n) { rx.feed(x, n); });
    done(rx);
    Tel t;
    rx.telemetry(t, 0);
    return fromTel(t);
}

// audio: the 1 kHz tone against everything else (a 3 kHz tone, the right channel of the FM test signal leaking over, is not noise), dB
static double toneSnrDb(const std::vector<float>& a, double secs) {
    const size_t n = std::min(a.size(), (size_t)(secs * 48000));
    if (n < 4800) return -99;
    const float* x = a.data() + a.size() - n;
    double mean = 0, tot = 0;
    for (size_t i = 0; i < n; i++) mean += x[i];
    mean /= (double)n;
    for (size_t i = 0; i < n; i++) tot += (x[i] - mean) * (x[i] - mean);
    tot /= (double)n;
    auto tone = [&](double hz) {
        std::complex<double> c = 0;
        for (size_t i = 0; i < n; i++) c += (x[i] - mean) * std::polar(1.0, -2 * M_PI * hz * (double)i / 48000.0);
        return 2 * std::norm(c / (double)n);   // power of the tone (amplitude^2 / 2)
    };
    const double p1 = tone(1000), p3 = tone(3000);
    return 10 * std::log10(p1 / std::max(1e-15, tot - p1 - p3));
}

static Metric fmRx(const Play& p, double rate) {
    FmReceiver rx;
    rx.setSilent(true);
    rx.configure(rate);
    std::vector<float> l;
    rx.setAudioTap([&](const float* L, const float*, size_t n) { l.insert(l.end(), L, L + n); });
    p([&](const cf32* x, size_t n) { rx.feed(x, n); });
    FmTelemetry t;
    rx.telemetry(t, 0);
    Metric m;
    m.q = toneSnrDb(l, 1.5);
    m.snr = t.snrDb;
    char b[64]; snprintf(b, sizeof b, "state %d", t.state); m.info = b;
    return m;
}

// ---------------------------------------------------------------- the modes

static std::vector<Mode> modes() {
    std::vector<Mode> v;
    auto add = [&](Mode m) { v.push_back(std::move(m)); };

    {   // DVB-T2: 8K, GI 1/8, 64-QAM 2/3 rotated, a transport stream in PLP 0. The decoder runs on its own threads and drops a frame when
        // they are busy (the other runs of this test), so the measure is the share of the decoded FEC blocks that are good, in %
        Mode m; m.name = "DVB-T2"; m.stdMode = 1; m.rate = nativeRateHz(8); m.secs = 1.5; m.minQ = 99; m.qIsDb = true; m.tolAbs = 1;
        m.gen = [](size_t n) {
            TxParams tp;
            tp.s1 = 0; tp.s2field1 = 1; tp.giIdx = 2; tp.payload = true; tp.plpMod = 2; tp.plpCod = 2; tp.plpRot = true; tp.plpTi = 3;
            T2Generator g(tp);
            Sig v, f;
            while (v.size() < n) { g.nextFrame(f); v.insert(v.end(), f.begin(), f.end()); }
            v.resize(n);
            scaleTo(v, 0.2); addNoise(v, 30, 11);
            return v;
        };
        m.rx = [](const Play& p, double rate) {
            T2Receiver rx;
            rx.setComputeMode(0);
            rx.configure(rate, 8);
            p([&](const cf32* x, size_t n) { rx.feed(x, n); });
            RxTelemetry t;
            uint64_t last = ~0ull;
            for (int i = 0, still = 0; i < 2000 && still < 150; i++) {   // the decoder finishes on its own threads
                rx.telemetry(t, 0);
                const uint64_t k = t.blocksOk + t.blocksBad;
                still = k == last ? still + 1 : 0;
                last = k;
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            Metric r;
            const double all = (double)(t.blocksOk + t.blocksBad);
            r.q = all >= 30 ? 100.0 * (double)t.blocksOk / all : 0; r.bad = (double)t.blocksBad; r.snr = t.plpMerDb;
            char b[96]; snprintf(b, sizeof b, "blocks %llu, frames dropped %llu", (unsigned long long)t.blocksOk, (unsigned long long)t.plpFramesDropped); r.info = b;
            return r;
        };
        add(m);
    }
    {   // DVB-T: 8K, GI 1/8, 64-QAM 2/3 (the engine's synthetic DVB-T)
        Mode m; m.name = "DVB-T"; m.stdMode = 2; m.rate = nativeRateHz(8); m.secs = 1.0; m.minQ = 2000;
        m.gen = [](size_t n) {
            dvbt::Params p; p.mode = dvbt::k8K; p.guard = dvbt::kGi8; p.mod = dvbt::k64Qam; p.crHp = p.crLp = dvbt::kR23;
            dvbt::Generator g(p, dvbt::testTsSource());
            Sig v, s;
            while (v.size() < n) { g.nextSymbol(s); v.insert(v.end(), s.begin(), s.end()); }
            v.resize(n);
            scaleTo(v, 0.2); addNoise(v, 30, 12);
            return v;
        };
        m.rx = [](const Play& p, double rate) {
            DvbtReceiver rx;
            rx.configure(rate, 8);
            p([&](const cf32* x, size_t n) { rx.feed(x, n); });
            RxTelemetry t;
            rx.telemetry(t, 0);
            Metric r; r.q = (double)(t.dvbt.rsClean + t.dvbt.rsCorrected); r.bad = (double)t.dvbt.rsFailed; r.snr = t.dataSnrDb;
            return r;
        };
        add(m);
    }
    {   // ATSC 8-VSB
        Mode m; m.name = "ATSC"; m.stdMode = 3; m.rate = 8e6; m.secs = 1.0; m.minQ = 5000;
        m.gen = [](size_t n) {
            atsc::ChannelConfig cc; cc.snrDb = 30;
            atsc::Generator g(dvbt::testTsSource(), cc, 8e6, 3);
            Sig v; v.reserve(n);
            g.generate(n, v);
            v.resize(n);
            scaleTo(v, 0.22);
            return v;
        };
        m.rx = [](const Play& p, double rate) {
            AtscReceiver rx;
            rx.configure(rate);
            rx.setBlocking(true);
            p([&](const cf32* x, size_t n) { rx.feed(x, n); });
            rx.flush();
            AtscTelemetry t;
            rx.telemetry(t, 0);
            Metric r; r.q = (double)(t.rsClean + t.rsCorrected); r.bad = (double)t.rsFailed; r.snr = t.dataSnrDb;
            return r;
        };
        add(m);
    }
    {   // DAB mode I: the FIBs with a good CRC
        Mode m; m.name = "DAB"; m.stdMode = 4; m.rate = 2.048e6; m.secs = 3.0; m.minQ = 300;
        m.gen = [](size_t n) { SynthConfig c; c.snrDb = 30; return synth(4, c, 2.048e6, n); };
        m.rx = [](const Play& p, double rate) {
            DabReceiver rx;
            rx.configure(rate);
            p([&](const cf32* x, size_t n) { rx.feed(x, n); });
            DabTelemetry t;
            rx.telemetry(t, 0);
            Metric r; r.q = (double)t.fibOk; r.bad = (double)t.fibBad; r.snr = t.snrDb;
            return r;
        };
        add(m);
    }
    {   // ATSC 3.0
        Mode m; m.name = "ATSC 3.0"; m.stdMode = 5; m.rate = 8e6; m.secs = 4.0; m.minQ = 99; m.qIsDb = true; m.tolAbs = 1; m.maxJobs = 4;
        m.gen = [](size_t n) { SynthConfig c; c.snrDb = 25; return synth(5, c, 8e6, n); };
        m.rx = [](const Play& p, double rate) {
            Atsc3Rx rx;
            rx.setBlocking(true);
            rx.configure(rate);
            p([&](const cf32* x, size_t n) { rx.feed(x, n); });
            Atsc3Telemetry t;
            long last = -1;
            for (int i = 0, still = 0; i < 600 && still < 20; i++) {   // the decoder works through the queued samples on its own thread
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                rx.telemetry(t, 0);
                still = t.frames == last ? still + 1 : 0;
                last = t.frames;
            }
            rx.telemetry(t, 0);
            rx.stop();
            // the share of the baseband packets that are good, in %: how many bootstraps the receiver finds in a few seconds differs from run
            // to run by a few (its threads), with or without the corrections
            Metric r; const double all = (double)(t.bbPackets + t.bbBad);
            r.q = all >= 50 ? 100.0 * (double)t.bbPackets / all : 0; r.bad = (double)t.bbBad;
            char b[128]; snprintf(b, sizeof b, "bootstraps %ld frames %ld failed %ld dropped %ld bb %ld bad %ld", t.bootstraps, t.frames, t.framesFailed, t.droppedBlocks, t.bbPackets, t.bbBad); r.info = b;
            return r;
        };
        add(m);
    }
    {   // ISDB-T: a one-segment layer A and a twelve-segment layer B (the engine's synthetic ISDB-T)
        Mode m; m.name = "ISDB-T"; m.stdMode = 6; m.rate = 10e6; m.secs = 2.0; m.minQ = 1500; m.maxJobs = 6;
        m.gen = [](size_t n) { SynthConfig c; c.snrDb = 30; return synth(6, c, 10e6, n); };
        m.rx = [](const Play& p, double rate) {
            IsdbtReceiver rx;
            rx.configure(rate);
            p([&](const cf32* x, size_t n) { rx.feed(x, n); });
            RxTelemetry t;
            rx.telemetry(t, 0);
            Metric r;
            for (const auto& L : t.isdbt.layer) { r.q += (double)(L.rsClean + L.rsCorrected); r.bad += (double)L.rsFailed; }
            r.snr = t.dataSnrDb;
            return r;
        };
        add(m);
    }
    {   // FM broadcast: stereo with RDS, 1 kHz left and 3 kHz right; the measure is the audio S/N of the left channel
        Mode m; m.name = "FM"; m.stdMode = 7; m.rate = 2e6; m.secs = 3.0; m.minQ = 45; m.qIsDb = true; m.tolAbs = 1.0; m.tolSnr = 1.0;
        m.gen = [](size_t n) { FmGenConfig c; c.rate = 2e6; c.cnrDb = 45; FmGenerator g(c); Sig v(n); g.generate(v.data(), n, 0.3f); return v; };
        m.rx = fmRx;
        add(m);
    }
    {   // FM with little modulation: nearly all the power in the carrier, the hard case for both corrections
        Mode m; m.name = "FM quiet"; m.stdMode = 7; m.rate = 2e6; m.secs = 3.0; m.minQ = 35; m.qIsDb = true; m.tolAbs = 1.0; m.tolSnr = 1.0; m.radio = false;
        m.gen = [](size_t n) {
            FmGenConfig c; c.rate = 2e6; c.cnrDb = 45; c.leftAmp = c.rightAmp = 0.02f; c.pilotPct = 1; c.rdsDevKhz = 0.2;
            FmGenerator g(c); Sig v(n); g.generate(v.data(), n, 0.3f); return v;
        };
        m.rx = fmRx;
        add(m);
    }
    {   // DVB-S2 QPSK 2/3, 5 Msym/s
        Mode m; m.name = "DVB-S2"; m.stdMode = 8; m.rate = 10e6; m.secs = 1.0; m.minQ = 100;
        m.gen = [](size_t n) { SynthConfig c; c.snrDb = 15; return synth(8, c, 10e6, n); };
        m.rx = [](const Play& p, double rate) {
            DvbsReceiver rx;
            rx.setBlocking(true);
            rx.configure(rate);
            p([&](const cf32* x, size_t n) { rx.feed(x, n); });
            rx.flush();
            DvbsTelemetry t;
            rx.telemetry(t, 0);
            Metric r = fromTel(t); r.snr = t.merDb;
            return r;
        };
        add(m);
    }
    {   // DTMB PN945 64-QAM 0.6: the share of good blocks in % (the decoder threads may drop work when the machine is busy) and the MER
        Mode m; m.name = "DTMB"; m.stdMode = 9; m.rate = 10e6; m.secs = 2.0; m.minQ = 99; m.qIsDb = true; m.tolAbs = 1;
        m.gen = [](size_t n) { SynthConfig c; c.snrDb = 32; return synth(9, c, 10e6, n); };
        m.rx = [](const Play& p, double rate) {
            DtmbReceiver rx;
            rx.configure(rate);
            p([&](const cf32* x, size_t n) { rx.feed(x, n); });
            rx.flush();
            DtmbTelemetry t;
            rx.telemetry(t, 0);
            Metric r;
            const double all = (double)(t.blocksOk + t.blocksBad);
            r.q = all >= 1000 ? 100.0 * (double)t.blocksOk / all : 0; r.bad = (double)t.blocksBad; r.snr = t.merDb;
            char b[64]; snprintf(b, sizeof b, "blocks %llu", (unsigned long long)t.blocksOk); r.info = b;
            return r;
        };
        add(m);
    }
    {   // analog TV B/G PAL with a steady 1 kHz tone: fields with all their lines, the picture's SNR, the sound's S/N
        Mode m; m.name = "Analog TV"; m.stdMode = 10; m.rate = 10e6; m.secs = 1.5; m.minQ = 50;
        m.gen = [](size_t n) { SynthConfig c; c.snrDb = 40; c.modeOpt[3] = 4; return synth(10, c, 10e6, n); };
        m.rx = [](const Play& p, double rate) {
            AtvReceiver rx;
            rx.setSilent(true);
            rx.configure(rate);
            std::vector<float> l;
            rx.setAudioTap([&](const float* L, const float*, size_t n) { l.insert(l.end(), L, L + n); });
            p([&](const cf32* x, size_t n) { rx.feed(x, n); });
            AtvTelemetry t;
            rx.telemetry(t, 0);
            Metric r = fromTel(t);
            r.aux = toneSnrDb(l, 0.6);
            return r;
        };
        add(m);
    }
    {   // DMR base station, busy: FEC blocks with a good check
        Mode m; m.name = "DMR"; m.stdMode = 11; m.rate = 2.4e6; m.secs = 4.0; m.minQ = 150;
        m.gen = [](size_t n) { SynthConfig c; c.snrDb = 25; c.modeOpt[3] = 2; return synth(11, c, 2.4e6, n); };
        m.rx = [](const Play& p, double rate) {
            return runSimple<DmrReceiver, DmrTelemetry>(p, rate, [](DmrReceiver& rx, double r) { rx.setSilent(true); rx.configure(r); }, [](DmrReceiver&) {});
        };
        add(m);
    }
    {   // DRM mode B, 10 kHz, short interleaving, a steady tone: audio frames received well
        Mode m; m.name = "DRM"; m.stdMode = 12; m.rate = 2e6; m.secs = 8.0; m.minQ = 100;
        m.gen = [](size_t n) { SynthConfig c; c.snrDb = 28; c.modeOpt[4] = 1; c.modeOpt[5] = 1; return synth(12, c, 2e6, n); };
        m.rx = [](const Play& p, double rate) {
            return runSimple<DrmReceiver, DrmTelemetry>(p, rate, [](DrmReceiver& rx, double r) { rx.setSilent(true); rx.configure(r); }, [](DrmReceiver&) {});
        };
        add(m);
    }
    {   // ADS-B: 12 aircraft, each with a carrier error of its own
        Mode m; m.name = "ADS-B"; m.stdMode = 13; m.rate = 4e6; m.secs = 3.0; m.minQ = 150;
        m.gen = [](size_t n) { SynthConfig c; c.snrDb = 30; c.modeOpt[0] = 12; return synth(13, c, 4e6, n); };
        m.rx = [](const Play& p, double rate) {
            return runSimple<AdsbReceiver, AdsbTelemetry>(p, rate, [](AdsbReceiver& rx, double r) { rx.configure(r); }, [](AdsbReceiver&) {});
        };
        add(m);
    }
    {   // GNSS GPS L1 C/A: satellites tracked and their mean C/N0
        Mode m; m.name = "GNSS"; m.stdMode = 14; m.rate = 4e6; m.secs = 6.0; m.minQ = 6; m.tolAbs = 0; m.tolRel = 0; m.tolSnr = 0.7;
        m.gen = [](size_t n) { SynthConfig c; c.modeVal[3] = 44; return synth(14, c, 4e6, n); };
        m.rx = [](const Play& p, double rate) {
            GnssReceiver rx;
            rx.configure(rate);
            p([&](const cf32* x, size_t n) { rx.feed(x, n); });
            GnssTelemetry t;
            rx.telemetry(t, 0);
            Metric r;
            double s = 0; int k = 0;
            for (const auto& c : t.channels) if (c.state >= GnssChLocked && c.cn0 > 0) { s += c.cn0; k++; }
            r.q = k; r.snr = k ? s / k : NAN;
            char b[64]; snprintf(b, sizeof b, "subframes %llu", (unsigned long long)t.blocksOk); r.info = b;
            return r;
        };
        add(m);
    }
    {   // radiosondes: an RS41 exactly on the centre (403 MHz), a DFM-17 and an M10 elsewhere in the band
        Mode m; m.name = "Radiosonde"; m.stdMode = 15; m.rate = 8e6; m.secs = 5.0; m.minQ = 5;
        m.gen = [](size_t n) { SynthConfig c; c.snrDb = 25; return synth(15, c, 8e6, n); };
        m.rx = [](const Play& p, double rate) {
            return runSimple<SondeReceiver, SondeTelemetry>(p, rate, [](SondeReceiver& rx, double r) { rx.setSynchronous(true); rx.setSignalOffset(0); rx.configure(r); rx.setCenterMhz(403); },
                                                            [](SondeReceiver& rx) { rx.flush(); });
        };
        add(m);
    }
    auto aisRx = [](double off) {
        return [off](const Play& p, double rate) {
            return runSimple<AisReceiver, AisTelemetry>(p, rate, [off](AisReceiver& rx, double r) { rx.setSignalOffset(off); rx.configure(r); }, [](AisReceiver&) {});
        };
    };
    {   // AIS: AIS 1 and AIS 2 at -25 and +25 kHz, the centre between them (each channel the mirror image of the other)
        Mode m; m.name = "AIS"; m.stdMode = 16; m.rate = 2e6; m.secs = 12.0; m.minQ = 20; m.tolAbs = 2;
        m.gen = [](size_t n) { SynthConfig c; c.snrDb = 34; return synth(16, c, 2e6, n); };
        m.rx = aisRx(0);
        add(m);
    }
    {   // AIS as a recording made on AIS 1: that channel on the centre
        Mode m; m.name = "AIS (AIS 1 on centre)"; m.stdMode = 16; m.rate = 2e6; m.secs = 12.0; m.minQ = 20; m.tolAbs = 2; m.radio = false;
        m.gen = [](size_t n) { SynthConfig c; c.snrDb = 34; Sig v = synth(16, c, 2e6, n); mix(v, 2e6, 25e3); return v; };
        m.rx = aisRx(25e3);
        add(m);
    }
    auto marineRx = [](double off) {
        return [off](const Play& p, double rate) {
            return runSimple<MarineReceiver, MarineTelemetry>(p, rate, [off](MarineReceiver& rx, double r) { rx.setSignalOffset(off); rx.configure(r); }, [](MarineReceiver&) {});
        };
    };
    const double mOff = marineTuning().tuneOffsetHz;
    {   // NAVTEX (a short message after 1 s of idle, again and again: the receiver lists it once), as the radio has it: tuneOffsetHz from the centre
        Mode m; m.name = "Marine NAVTEX"; m.stdMode = 17; m.rate = 2e6; m.secs = 25.0; m.minQ = 1; m.tolAbs = 0; m.tolRel = 0;
        m.gen = [](size_t n) { SynthConfig c; c.snrDb = 25; c.modeOpt[0] = 1; c.modeOpt[2] = 4; c.modeVal[1] = 1; return synth(17, c, 2e6, n); };
        m.rx = marineRx(-mOff);
        add(m);
    }
    {   // the same as a recording: the channel on the centre
        Mode m; m.name = "Marine NAVTEX (centre)"; m.stdMode = 17; m.rate = 2e6; m.secs = 25.0; m.minQ = 1; m.tolAbs = 0; m.tolRel = 0; m.radio = false;
        m.gen = [](size_t n) { SynthConfig c; c.snrDb = 25; c.modeOpt[0] = 1; c.modeOpt[2] = 4; c.modeVal[1] = 1; return synthEx(makeMarineSynthAt(c, 2e6, 0), n); };
        m.rx = marineRx(0);
        add(m);
    }
    {   // DSC on VHF channel 70 (FM with AFSK): an FM carrier on the centre
        Mode m; m.name = "Marine DSC VHF (centre)"; m.stdMode = 17; m.rate = 2e6; m.secs = 25.0; m.minQ = 10; m.radio = false;
        m.gen = [](size_t n) { SynthConfig c; c.snrDb = 25; c.modeOpt[0] = 4; return synthEx(makeMarineSynthAt(c, 2e6, 0), n); };
        m.rx = marineRx(0);
        add(m);
    }
    auto acarsRx = [](std::vector<double> chans) {
        return [chans](const Play& p, double rate) {
            return runSimple<AcarsReceiver, AcarsTelemetry>(p, rate, [chans](AcarsReceiver& rx, double r) {
                rx.setSignalOffset(0); rx.setCenterHz(131.5e6); rx.configure(r); if (!chans.empty()) rx.setChannels(chans); }, [](AcarsReceiver&) {});
        };
    };
    {   // ACARS: six aircraft on 131.525, 131.725 and 131.825 MHz, tuned to 131.5 MHz
        Mode m; m.name = "ACARS"; m.stdMode = 18; m.rate = 2e6; m.secs = 12.0; m.minQ = 30;
        m.gen = [](size_t n) { SynthConfig c; c.snrDb = 30; c.modeVal[0] = 6; return synth(18, c, 2e6, n); };
        m.rx = acarsRx({});
        add(m);
    }
    {   // ACARS with the channel on the tuned frequency: the AM carrier exactly on the centre (the channel has to be listed: the receiver
        // leaves a carrier on the centre alone otherwise, it takes it for the DC spike)
        Mode m; m.name = "ACARS (channel on centre)"; m.stdMode = 18; m.rate = 2e6; m.secs = 12.0; m.minQ = 15; m.radio = false;
        m.gen = [](size_t n) {
            SynthConfig c; c.snrDb = 30;
            AcarsGenOptions o; o.channelsHz = {131.5e6}; o.cfoSpreadHz = 0; o.rateFactor = 6; o.aircraft = 6;
            return synthEx(makeAcarsSynthEx(o, c, 2e6), n);
        };
        m.rx = acarsRx({131.5e6});
        add(m);
    }
    auto inmcRx = [](double off) {
        return [off](const Play& p, double rate) {
            return runSimple<InmcReceiver, InmcTelemetry>(p, rate, [off](InmcReceiver& rx, double r) { rx.setSignalOffset(off); rx.configure(r); }, [](InmcReceiver&) {});
        };
    };
    const double iOff = inmcTuning().tuneOffsetHz;
    {   // Inmarsat-C: BPSK 1200 sym/s, 8.64 s frames
        Mode m; m.name = "Inmarsat-C"; m.stdMode = 19; m.rate = 2e6; m.secs = 36.0; m.minQ = 3; m.tolRel = 0;
        m.gen = [](size_t n) { SynthConfig c; c.snrDb = 30; return synth(19, c, 2e6, n); };
        m.rx = inmcRx(-iOff);
        add(m);
    }
    {   // the same with the carrier on the centre: BPSK there is not circular
        Mode m; m.name = "Inmarsat-C (centre)"; m.stdMode = 19; m.rate = 2e6; m.secs = 36.0; m.minQ = 3; m.tolRel = 0; m.radio = false;
        m.gen = [iOff](size_t n) { SynthConfig c; c.snrDb = 30; Sig v = synth(19, c, 2e6, n); mix(v, 2e6, iOff); return v; };
        m.rx = inmcRx(0);
        add(m);
    }
    auto aeroRx = [](double off) {
        return [off](const Play& p, double rate) {
            return runSimple<AeroReceiver, AeroTelemetry>(p, rate, [off](AeroReceiver& rx, double r) { rx.setSignalOffset(off); rx.configure(r); }, [](AeroReceiver&) {});
        };
    };
    const double aOff = aeroTuning().tuneOffsetHz;
    {   // Inmarsat Aero: 10500 and 1200 bit/s P channels
        Mode m; m.name = "Inmarsat Aero"; m.stdMode = 20; m.rate = 2e6; m.secs = 6.0; m.minQ = 100;
        m.gen = [](size_t n) { SynthConfig c; return synth(20, c, 2e6, n); };
        m.rx = aeroRx(-aOff);
        add(m);
    }
    {   // a recording with the 10500 bit/s channel exactly on the centre (no carrier offsets of its own; played without an offset, so the
        // receiver does not skip the centre as the DC spike of a radio)
        Mode m; m.name = "Inmarsat Aero (centre)"; m.stdMode = 20; m.rate = 2e6; m.secs = 6.0; m.minQ = 100; m.radio = false;
        m.gen = [aOff](size_t n) { SynthConfig c; c.modeOpt[2] = 1; Sig v = synth(20, c, 2e6, n); mix(v, 2e6, 50e3 + aOff); return v; };
        m.rx = aeroRx(0);
        add(m);
    }
    {   // Iridium: bursts all over 10 MHz
        Mode m; m.name = "Iridium"; m.stdMode = 21; m.rate = 10e6; m.secs = 2.0; m.minQ = 100;
        m.gen = [](size_t n) { SynthConfig c; c.snrDb = 25; return synth(21, c, 10e6, n); };
        m.rx = [](const Play& p, double rate) {
            return runSimple<IridiumReceiver, IridiumTelemetry>(p, rate, [](IridiumReceiver& rx, double r) { rx.setSignalOffset(0); rx.setOffline(true); rx.configure(r); rx.setCenterMhz(1622); },
                                                                [](IridiumReceiver& rx) { rx.flush(); });
        };
        add(m);
    }
    auto meshRx = [](double off) {
        return [off](const Play& p, double rate) {
            return runSimple<MeshReceiver, MeshTelemetry>(p, rate, [off](MeshReceiver& rx, double r) { rx.setSignalOffset(off); rx.configure(r); }, [](MeshReceiver&) {});
        };
    };
    const double meOff = meshTuning().tuneOffsetHz;
    {   // Meshtastic LongFast and MeshCore
        Mode m; m.name = "Mesh"; m.stdMode = 22; m.rate = 2e6; m.secs = 15.0; m.minQ = 8;
        m.gen = [](size_t n) { SynthConfig c; c.snrDb = 25; c.modeVal[0] = 5; return synth(22, c, 2e6, n); };
        m.rx = meshRx(-meOff);
        add(m);
    }
    {   // LongFast on the centre
        Mode m; m.name = "Mesh (centre)"; m.stdMode = 22; m.rate = 2e6; m.secs = 15.0; m.minQ = 8; m.radio = false;
        m.gen = [meOff](size_t n) { SynthConfig c; c.snrDb = 25; c.modeVal[0] = 5; Sig v = synth(22, c, 2e6, n); mix(v, 2e6, meOff); return v; };
        m.rx = meshRx(0);
        add(m);
    }
    return v;
}

// ---------------------------------------------------------------- comparison

static bool worse(const Mode& m, const Metric& ref, const Metric& x, std::string& why) {
    char b[200];
    const double allow = m.qIsDb ? m.tolAbs : std::max(m.tolAbs, m.tolRel * ref.q);
    if (x.q < ref.q - allow) { snprintf(b, sizeof b, "%.1f against %.1f", x.q, ref.q); why = b; return true; }
    if (ref.q > 0 && std::isfinite(ref.snr) && !(x.snr >= ref.snr - m.tolSnr)) { snprintf(b, sizeof b, "SNR %.1f dB against %.1f", x.snr, ref.snr); why = b; return true; }
    if (ref.q > 0 && std::isfinite(ref.aux) && !(x.aux >= ref.aux - m.tolSnr)) { snprintf(b, sizeof b, "sound S/N %.1f dB against %.1f", x.aux, ref.aux); why = b; return true; }
    return false;
}

static std::string fmt(const Metric& r, bool qIsDb) {
    char b[64];
    if (qIsDb) snprintf(b, sizeof b, "%.1f", r.q);
    else snprintf(b, sizeof b, "%.0f", r.q);
    std::string s = b;
    if (std::isfinite(r.snr)) { snprintf(b, sizeof b, "/%.1f", r.snr); s += b; }
    if (std::isfinite(r.aux)) { snprintf(b, sizeof b, "/%.1f", r.aux); s += b; }
    return s;
}

// ---------------------------------------------------------------- the engine: the spike in the centre bin of its spectrum

struct SpecRun { bool ok = false; double off = 0, on = 0; };   // centre bin against its neighbours, corrections off and on
struct SpecResult { SpecRun clean, imp; };

// the centre bin against the bins around it (3 to 12 bins away), dB
static double centreSpike(const SpectrumFrame& f) {
    const size_t N = f.dbfs.size(), c = N / 2;
    std::vector<float> nb;
    for (size_t k = 3; k <= 12; k++) { nb.push_back(f.dbfs[c - k]); nb.push_back(f.dbfs[c + k]); }
    std::nth_element(nb.begin(), nb.begin() + (long)nb.size() / 2, nb.end());
    return f.dbfs[c] - nb[nb.size() / 2];
}

// The signal (as a recording: the channel where the generator puts it, played without an offset) from a file through the engine: the centre
// bin of its spectrum with the corrections off, then after they are turned on and have settled
static SpecRun engineSpike(const Mode& m, const Base& base, bool impaired, const std::string& path) {
    SpecRun r;
    {
        FILE* f = fopen(path.c_str(), "wb");
        if (!f) { FAILF("%s: cannot write %s", m.name.c_str(), path.c_str()); return r; }
        Setup s; s.impaired = impaired; s.imbalance = false;
        playThrough(base, m.rate, s, false, false, [&](const cf32* x, size_t n) { fwrite(x, sizeof(cf32), n, f); }, nullptr, 0);   // a strong spike: it must stand out of a narrow signal on the centre
        fclose(f);
    }
    Engine e;
    e.setSpectrumEnabled(true);
    DeviceInfo dev; dev.kind = DeviceInfo::File; dev.name = "iq test file";
    TuneSettings tune; tune.sampleRate = m.rate; tune.centerHz = 100e6;
    const ModeTuning* mt = modeTuning(m.stdMode);
    tune.bandwidthMhz = m.stdMode <= 2 ? 8 : mt ? mt->bandwidthMhz : 8;
    FileOptions fo; fo.path = path; fo.format = FileFormat::CF32; fo.sampleRate = m.rate; fo.loop = true;
    e.setStandard(m.stdMode);
    if (!e.start(dev, tune, fo)) { FAILF("%s: the engine did not start", m.name.c_str()); std::remove(path.c_str()); return r; }
    // the spectrum averaged over a second of signal from sigSecs on (a whole frame cycle of the bursty modes: an RS41 on the centre sends 0.6 s a second)
    auto spectrumAfter = [&](double sigSecs, SpectrumFrame& out) {
        const double t0 = enginetest::signalSecs(e);
        enginetest::waitFor(30, [&] { return enginetest::signalSecs(e) > t0 + sigSecs; });
        uint64_t seq = 0;
        SpectrumFrame f;
        if (e.latestSpectrum(f, 0)) seq = f.seq;
        std::vector<double> acc;
        int frames = 0;
        const double t1 = enginetest::signalSecs(e);
        enginetest::waitFor(30, [&] {
            if (e.latestSpectrum(f, seq) && !f.dbfs.empty()) {
                seq = f.seq;
                if (acc.size() != f.dbfs.size()) { acc.assign(f.dbfs.size(), 0.0); frames = 0; }
                for (size_t k = 0; k < f.dbfs.size(); k++) acc[k] += std::pow(10.0, f.dbfs[k] / 10.0);
                frames++;
            }
            return frames > 0 && enginetest::signalSecs(e) > t1 + 1.0;
        });
        if (!frames) return false;
        out.dbfs.resize(acc.size());
        for (size_t k = 0; k < acc.size(); k++) out.dbfs[k] = (float)(10 * std::log10(acc[k] / frames + 1e-30));
        return true;
    };
    SpectrumFrame f;
    const bool a = spectrumAfter(0.1, f);
    if (a) r.off = centreSpike(f);
    e.iqFix().dc = true; e.iqFix().iq = true;
    const bool b = spectrumAfter(0.3, f);
    if (b) r.on = centreSpike(f);
    r.ok = a && b;
    e.stop();
    std::remove(path.c_str());
    return r;
}

// ---------------------------------------------------------------- main

int main(int argc, char** argv) {
    const double t0 = wallNow();
    std::vector<Mode> all = modes();
    const std::string only = argc > 1 ? argv[1] : "";
    std::vector<Mode*> run;
    for (auto& m : all) if (only.empty() || m.name.find(only) != std::string::npos) run.push_back(&m);
    const unsigned hw = std::max(2u, std::thread::hardware_concurrency());

    // every receiver run of a mode is a job; a mode's signal is made once and freed when its jobs are done (at most three signals in memory)
    struct Job { Mode* m; int kind; int fi; int ci; };   // kind 0 clean (offset fi, corrections ci), 1 impaired off, 2 impaired on
    std::mutex mu;
    std::condition_variable cv;
    std::deque<std::pair<Job, std::shared_ptr<const Base>>> queue;
    int live = 0;
    bool doneAdding = false;
    std::vector<std::thread> workers;
    for (unsigned w = 0; w < hw; w++) {
        workers.emplace_back([&] {
            for (;;) {
                std::pair<Job, std::shared_ptr<const Base>> it;
                {
                    std::unique_lock<std::mutex> lk(mu);
                    auto pick = [&] { for (auto q = queue.begin(); q != queue.end(); ++q) if (q->first.m->running < q->first.m->maxJobs) return q; return queue.end(); };
                    cv.wait(lk, [&] { return pick() != queue.end() || (queue.empty() && doneAdding); });
                    if (queue.empty()) return;
                    auto q = pick();
                    it = *q; queue.erase(q);
                    it.first.m->running++;
                }
                const Job& j = it.first;
                Mode& m = *j.m;
                const Base& base = *it.second;
                const bool keepDc = keepDcFor(m.stdMode), keepIq = keepIqFor(m.stdMode);
                Setup s;
                Residual res;
                if (j.kind == 0) { s.f0 = kOffsets[j.fi]; s.dc = j.ci >= 1; s.iq = j.ci >= 2; }
                else { s.impaired = true; s.dc = s.iq = j.kind == 2; }
                const double c0 = wallNow();
                Play play = [&](const Sink& sink) { playThrough(base, m.rate, s, keepDc, keepIq, sink, j.kind == 2 ? &res : nullptr); };
                Metric r = m.rx(play, m.rate);
                it.second.reset();
                if (getenv("IQ_MODES_VERBOSE")) {
                    std::lock_guard<std::mutex> lk(outMu);
                    printf("    %s: %s %+.0f Hz %s: %s %s\n", m.name.c_str(), j.kind == 0 ? "clean" : "impaired", s.f0, s.dc && s.iq ? "DC+IQ" : s.dc ? "DC" : "off", fmt(r, m.qIsDb).c_str(), r.info.c_str());
                }
                std::lock_guard<std::mutex> lk(mu);
                m.cpu += wallNow() - c0;
                if (j.kind == 0) m.clean[j.fi][j.ci] = r;
                else if (j.kind == 1) m.impOff = r;
                else { m.impOn = r; m.res = res; }
                m.running--;
                if (--m.pending == 0) live--;   // the last run of this signal
                cv.notify_all();
            }
        });
    }
    for (Mode* m : run) {
        {
            std::unique_lock<std::mutex> lk(mu);
            cv.wait(lk, [&] { return live < 3; });
            live++;
        }
        const double g0 = wallNow();
        std::shared_ptr<const Base> base = toBase(m->gen((size_t)(m->secs * m->rate)));
        {
            std::lock_guard<std::mutex> lk(outMu);
            printf("  %-26s %.1f s of signal at %.3f Msps made in %.1f s\n", m->name.c_str(), m->secs, m->rate / 1e6, wallNow() - g0);
            fflush(stdout);
        }
        std::lock_guard<std::mutex> lk(mu);
        m->pending = 14;
        for (int fi = 0; fi < 4; fi++) for (int ci = 0; ci < 3; ci++) queue.push_back({Job{m, 0, fi, ci}, base});
        queue.push_back({Job{m, 1, 0, 0}, base});
        queue.push_back({Job{m, 2, 0, 0}, base});
        cv.notify_all();
    }
    { std::lock_guard<std::mutex> lk(mu); doneAdding = true; }
    cv.notify_all();
    for (auto& w : workers) w.join();
    const double tRx = wallNow() - t0;

    // ---- the table and the checks
    printf("\nmeasure per mode: good frames / blocks / messages (DVB-T2, DTMB, ATSC 3.0: %% of them good; FM: audio S/N dB) / SNR, MER or C/N0 dB (/ sound S/N dB)\n");
    printf("%-26s %-6s| %-38s| %-38s| %-38s| %-38s| %-26s| residual DC, image dB: in -> out; IQ measured\n", "mode", "keeps", "0 Hz: off  DC  DC+IQ", "5 Hz", "50 Hz", "500 Hz", "impaired: off  on");
    for (Mode* mp : run) {
        Mode& m = *mp;
        const bool keepDc = keepDcFor(m.stdMode), keepIq = keepIqFor(m.stdMode);
        std::string line;
        char b[300];
        for (int fi = 0; fi < 4; fi++) {
            snprintf(b, sizeof b, "%-12s %-12s %-12s| ", fmt(m.clean[fi][0], m.qIsDb).c_str(), fmt(m.clean[fi][1], m.qIsDb).c_str(), fmt(m.clean[fi][2], m.qIsDb).c_str());
            line += b;
        }
        snprintf(b, sizeof b, "%-12s %-12s| %5.1f -> %5.1f, %5.1f -> %5.1f; %.2f dB %.1f deg", fmt(m.impOff, m.qIsDb).c_str(), fmt(m.impOn, m.qIsDb).c_str(),
                 m.res.dcIn, m.res.dcOut, m.res.imgIn, m.res.imgOut, m.res.gainDb, m.res.phaseDeg);
        line += b;
        const char* keeps = keepDc && keepIq ? "DC IQ" : keepDc ? "DC" : keepIq ? "IQ" : "-";
        printf("%-26s %-6s| %s  (%.0f s) %s\n", m.name.c_str(), keeps, line.c_str(), m.cpu, m.clean[0][0].info.c_str());

        if (m.clean[0][0].q < m.minQ) FAILF("%s: the clean signal gives only %.1f (at least %.1f expected): the comparison means nothing", m.name.c_str(), m.clean[0][0].q, m.minQ);
        std::string why;
        for (int fi = 0; fi < 4; fi++)
            for (int ci = 1; ci < 3; ci++)
                if (worse(m, m.clean[fi][0], m.clean[fi][ci], why)) FAILF("%s, carrier %+.0f Hz from the centre: %s makes it worse: %s", m.name.c_str(), kOffsets[fi], kCfgName[ci], why.c_str());
        // with the impairment the corrections may not do worse than leaving it alone (or, where the impairment happens to cost nothing, than the clean signal)
        Metric refImp = m.impOff;
        if (m.clean[0][0].q < refImp.q) refImp.q = m.clean[0][0].q;
        if (std::isfinite(m.clean[0][0].snr) && m.clean[0][0].snr < refImp.snr) refImp.snr = m.clean[0][0].snr;
        if (std::isfinite(m.clean[0][0].aux) && m.clean[0][0].aux < refImp.aux) refImp.aux = m.clean[0][0].aux;
        if (worse(m, refImp, m.impOn, why)) FAILF("%s, DC offset and IQ imbalance: the corrections make it worse: %s", m.name.c_str(), why.c_str());
        // the impairment comes out of the samples, except the part the mode keeps
        if (!keepDc && m.res.dcOut > m.res.dcIn - 20) FAILF("%s: DC %.1f dB before, %.1f dB after the correction", m.name.c_str(), m.res.dcIn, m.res.dcOut);
        if (keepDc && std::fabs(m.res.dcOut - m.res.dcIn) > 1.5) FAILF("%s: the DC is kept for this mode, but it went from %.1f to %.1f dB", m.name.c_str(), m.res.dcIn, m.res.dcOut);
        if (!keepIq && m.res.imgOut > m.res.imgIn - 15) FAILF("%s: the mirror image %.1f dB before, %.1f dB after the correction", m.name.c_str(), m.res.imgIn, m.res.imgOut);
    }

    // ---- the engine's spectrum: per mode the clean and the impaired signal, a few modes at a time (each run plays a file at the radio's speed)
    const double t1 = wallNow();
    std::vector<Mode*> spec;
    for (Mode* m : run) if (m->radio) spec.push_back(m);
    std::vector<SpecResult> sr(spec.size());
    {
        std::atomic<size_t> next{0};
        std::vector<std::thread> th;
        for (int w = 0; w < 6; w++)
            th.emplace_back([&, w] {
                for (size_t i; (i = next++) < spec.size();) {
                    const Mode& m = *spec[i];
                    auto base = toBase(m.gen((size_t)(1.0 * m.rate)));
                    const std::string path = "test_iq_modes_" + std::to_string(w) + ".cf32";
                    sr[i].clean = engineSpike(m, *base, false, path);
                    sr[i].imp = engineSpike(m, *base, true, path);
                }
            });
        for (auto& t : th) t.join();
    }
    printf("\nengine spectrum, centre bin against the bins 3 to 12 away (dB), corrections off -> on; a DC spike as strong as the signal:\n");
    for (size_t i = 0; i < spec.size(); i++) {
        const Mode& m = *spec[i];
        const bool keepDc = keepDcFor(m.stdMode);
        const SpecResult& s = sr[i];
        printf("  %-26s clean %+6.1f -> %+6.1f   with the spike %+6.1f -> %+6.1f   %s\n", m.name.c_str(), s.clean.off, s.clean.on, s.imp.off, s.imp.on, keepDc ? "(DC removal skipped for this mode)" : "");
        if (!s.clean.ok || !s.imp.ok) { FAILF("%s: no spectrum from the engine", m.name.c_str()); continue; }
        if (s.imp.off < s.clean.off + 6) FAILF("%s: the injected spike does not show in the engine's spectrum (%.1f dB, %.1f dB without it)", m.name.c_str(), s.imp.off, s.clean.off);
        // gone: no higher than the clean signal there (or than the bins around, where the signal has a hole on the centre, as DAB's unused carrier)
        if (!keepDc && s.imp.on > std::max(s.clean.on, 0.0) + 3) FAILF("%s: the spike is still in the engine's spectrum with DC removal on (%.1f dB, %.1f dB without it)", m.name.c_str(), s.imp.on, s.clean.on);
        if (keepDc && s.imp.on < s.imp.off - 2) FAILF("%s: the engine removed the DC of a mode that keeps it (%.1f -> %.1f dB)", m.name.c_str(), s.imp.off, s.imp.on);
    }
    printf("\nreceiver runs %.0f s, engine spectrum %.0f s\n", tRx, wallNow() - t1);
    printf(fails ? "iq modes: %d FAILED\n" : "iq modes: all passed\n", fails.load());
    return fails ? 1 : 0;
}
