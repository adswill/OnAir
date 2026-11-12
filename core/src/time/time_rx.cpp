// Time signal receiver (see time_rx.h).
#include "dect2/time/dcf77_phase.h"
#include "dect2/time/wwvb_phase.h"
#include "dect2/time/time_rx.h"
#include "dect2/results/results_io.h"
#include "dect2/time/time_tel.h"
#include "dect2/time/time_proto.h"
#include "../audio/ham_listen.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <ctime>
#include <deque>
#include <mutex>
#include <vector>
#include "dect2/common/timecompat.h"
#include "dect2/common/mathconst.h"

namespace dect2 {

namespace {
using namespace timesig;
using cd = std::complex<double>;
using dect2::math::kPi;
constexpr double kW = 8000;           // working rate
constexpr int kCar = 160;             // 20 ms carrier window
constexpr int kFsk = 27;              // one CHU bit (8000 / 300 = 26.7 samples)
constexpr int kR1 = 4096, kR8 = 32768;   // rings: 4 s at 1 kHz and at 8 kHz
constexpr int kEnvLag = 9;            // a 1 kHz sample i stands for signal time (i - 9) ms
constexpr int kBurstLag = 4;          // BPM coded: a 5 ms burst that starts at ms T peaks in the 5 ms window sample T + 4

double wallNow() { return dect2::systemNowSec(); }

// complex moving sum over a fixed length
struct Box {
    std::vector<cd> r;
    cd s{0, 0};
    size_t i = 0;
    int cnt = 0;
    void init(int n) { r.assign((size_t)n, cd(0, 0)); s = 0; i = 0; cnt = 0; }
    cd push(cd v) {
        s += v - r[i];
        r[i] = v;
        if (++i == r.size()) { i = 0; if (++cnt >= 64) { cnt = 0; s = 0; for (auto& x : r) s += x; } }   // no drift
        return s;
    }
};

struct Tone {   // e^{-j 2 pi f n / 8000}, exact over its period
    std::vector<cd> t;
    size_t i = 0;
    void init(double f, int period) { t.resize((size_t)period); for (int k = 0; k < period; k++) t[(size_t)k] = std::polar(1.0, -2 * kPi * f * k / kW); i = 0; }
    cd next() { const cd v = t[i]; if (++i == t.size()) i = 0; return v; }
};

char symChar(uint8_t s, int st) {
    if (s == SMark) return 'M';
    if (s == SNone) return '-';
    if (s == SUnknown) return '?';
    if (st == Rwm && s == SB) return 't';   // a second of the 10 pulse minutes
    return st == Msf || st == Bpc ? (char)('0' + (s & 3)) : (char)('0' + (s & 1));
}

bool isJjy(int s) { return s == Jjy40 || s == Jjy60; }
bool risesOnSecond(int s) { return isJjy(s) || s == Rwm; }   // the carrier comes up on the second (the others drop it)
bool amAudio(int s) { return !stationKeysCarrier(s); }        // the code is in the AM audio (WWV, CHU, BPM, HLA): otherwise the carrier is keyed
} // namespace

struct TimeReceiver::Impl {
    std::mutex mu;
    std::function<void(const std::string&)> log;
    double rateSet = 0, offsetSet = 0;
    std::atomic<double> dialHz{0};
    std::atomic<int> stationReq{-1}, pref60{Msf}, clockMode{0};
    std::atomic<double> clockOrigin{0}, rxLat{0}, rxLon{0};
    std::atomic<bool> rxLocSet{false};
    std::atomic<bool> resetReq{true}, clearReq{false};
    std::atomic<int> stationNow{Dcf77};

    // running
    double rate = 0, offset = 0;
    int st = Dcf77;
    int64_t nIn = 0;
    cd lo{1, 0}, loStep{1, 0};
    int loCount = 0;
    cd acc{0, 0};
    int accN = 0;
    int64_t k8 = 0;                  // 8 kHz samples made
    double binEnd = 0;
    Box car, sub100, t0, t1, fm, fs;
    Tone m100, m1000, m1200, m1800, m2025, m2225;
    std::atomic<double> clockScale{1}, audioTone{0};   // a recording whose sample clock is off: signal time = samples / (rate * scale); audio instead of IQ
    std::atomic<bool> audioIn{false};
    double rateRaw = 0;
    // BPM coded: the minute frame's values wait for the second frames of that minute
    int mcTenth = -1, mcLeap = -1, mcLs = 0;
    double mcS = -1;                     // signal time of the last M until a second frame names its minute
    int64_t mcUtc = -1;                  // the minute (UTC epoch) the minute frame's values belong to
    int64_t lastLogCount = -1000;
    double dc = 0;
    hfdig::Listen listen;                // "Listen": the AM audio (HF) or a tone keyed by the carrier (LF) on the sound card
    std::vector<float> aud;
    double lfRef = 0, audPh = 0;
    int tickL = 40;
    // 1 kHz rings
    std::vector<float> env, env100;
    std::vector<cd> zc;
    int64_t n1k = 0;
    // 8 kHz rings
    std::vector<float> tk0, tk1, fsk;
    // profiles
    std::vector<float> prof, prof100, pT0, pT1;
    int profSecs = 0;
    // seconds
    bool locked = false;
    double phase = 0, hi = 1, lo2 = 0, depth = 0, top = 0, base = 0;
    bool hawaii = false;
    double nextSec = -1;
    int64_t secCount = 0;
    std::deque<std::pair<int64_t, uint8_t>> syms;
    double lastS = 0;
    int64_t lastCount = -1;
    // time
    bool anchored = false;
    int64_t anchorCount = 0, anchorUtc = 0;
    int framesInRow = 0;
    double lastFrameSec = -1e9, unlockedSince = -1;
    Fields fields;
    // clock
    double wallMinCur = 1e300, wallMinPrev = 1e300, wallWin = 0;
    std::deque<float> offs;
    cd cfoAcc{0, 0};
    double cfo = 0;
    float snr = 0, level = -200, tick0Db = -200, tick1Db = -200;
    std::deque<float> levelH, snrH, tickH, offH;
    std::deque<double> resid;
    uint64_t okN = 0, badN = 0;
    std::deque<TimeFrameLog> flog;
    bool logLoad = false;                // a loaded file for flog (mu): the feed thread takes it
    std::deque<TimeFrameLog> logIn;
    double nextReport = 0;
    TimeTelemetry pub;
    uint64_t seq = 0;

    double tNow() const { return (double)k8 / kW; }

    int resolveStation() {
        const int r = stationReq.load();
        if (r >= 0 && r < kNumStations) return r;
        const int s = stationForKhz(dialHz.load() / 1e3, pref60.load());
        return s >= 0 ? s : Dcf77;
    }

    void resetState() {
        { std::lock_guard<std::mutex> lk(mu); rateRaw = rateSet; rate = rateSet * clockScale.load(); offset = offsetSet; }
        st = resolveStation();
        stationNow = st;
        if (audioIn.load()) offset = amAudio(st) ? 0.0 : audioTone.load();   // demodulated audio: HF is used as it is, an LF carrier is a tone
        mcTenth = mcLeap = -1; mcLs = 0; mcS = -1; mcUtc = -1; lastLogCount = -1000;
        nIn = 0; lo = 1; loStep = rate > 0 ? std::polar(1.0, -2 * kPi * offset / rate) : cd(1, 0); loCount = 0;
        acc = 0; accN = 0; k8 = 0; binEnd = rate / kW;
        tickL = st == Chu || st == Bpm ? 80 : 40;   // BPM coded and HLA: 5 ms windows (5 cycles of 1 kHz; 9 of 1800 Hz)
        car.init(kCar); sub100.init(kCar); t0.init(tickL); t1.init(tickL); fm.init(kFsk); fs.init(kFsk);
        m100.init(100, 80); m1000.init(1000, 8); m1800.init(1800, 40); m1200.init(1200, 20); m2025.init(2025, 320); m2225.init(2225, 320);
        dc = 0; listen.reset(); aud.clear(); lfRef = 0; audPh = 0;
        env.assign(kR1, 0.f); env100.assign(kR1, 0.f); zc.assign(kR1, cd(0, 0)); n1k = 0;
        tk0.assign(kR8, 0.f); tk1.assign(kR8, 0.f); fsk.assign(kR8, 0.f);
        prof.assign(1000, 0.f); prof100.assign(1000, 0.f); pT0.assign(8000, 0.f); pT1.assign(8000, 0.f); profSecs = 0;
        locked = false; depth = 0; nextSec = -1; secCount = 0; syms.clear(); lastCount = -1;
        anchored = false; framesInRow = 0; lastFrameSec = -1e9; unlockedSince = -1;
        offs.clear(); cfoAcc = 0; levelH.clear(); snrH.clear(); tickH.clear(); offH.clear(); resid.clear();
        wallMinCur = wallMinPrev = 1e300; wallWin = 0;
        nextReport = 0.25;
    }

    template <class T> static T at(const std::vector<T>& r, int64_t i) { return r[(size_t)(i & (int64_t)(r.size() - 1))]; }
    template <class T> static void put(std::vector<T>& r, int64_t i, T v) { r[(size_t)(i & (int64_t)(r.size() - 1))] = v; }

    // ---------------------------------------------------------------- 8 kHz
    Dcf77Phase dcfPhase;
    WwvbPhase wwvbPhase;
    void sample8(cd x) {
        const int64_t j = k8++;
        if (st == Dcf77) dcfPhase.feed(std::complex<float>((float)x.real(), (float)x.imag()));
        if (st == Wwvb) wwvbPhase.feed(std::complex<float>((float)x.real(), (float)x.imag()));
        const cd c = car.push(x);
        const bool hf = amAudio(st);
        if (hf) {
            const double am = std::abs(x);
            dc += (am - dc) * (j < 800 ? 0.01 : 1.0 / 4000);
            const double au = am - dc;
            if (listen.on.load()) aud.push_back((float)std::max(-1.0, std::min(1.0, au / std::max(dc, 1e-9) * 0.8)));
            const cd s1 = sub100.push(au * m100.next());
            const float a0 = (float)(std::abs(t0.push(au * (st == Hla ? m1800.next() : m1000.next()))) / tickL);
            if ((st == Bpm || st == BpmC) && j % 8 == 7) put(env100, n1k, a0);   // BPM: the 1 kHz pulse widths, 1 ms steps
            const float a1 = (float)(std::abs(t1.push(au * m1200.next())) / tickL);
            put(tk0, j, a0); put(tk1, j, a1);
            const size_t b = (size_t)(j % 8000);
            pT0[b] += 0.15f * (a0 - pT0[b]); pT1[b] += 0.15f * (a1 - pT1[b]);
            if (st == Chu) {
                const double mk = std::abs(fm.push(au * m2225.next())), sp = std::abs(fs.push(au * m2025.next()));
                put(fsk, j, (float)(mk - sp));
            }
            if (st != Bpm && st != BpmC && j % 8 == 7) put(env100, n1k, (float)(std::abs(s1) / kCar));
        }
        if (!hf && listen.on.load()) {   // the carrier keyed down for 100 / 200 ms: an 800 Hz tone while it is down
            const double am = std::abs(x);
            lfRef += (am > lfRef ? 0.01 : 1.0 / 16000) * (am - lfRef);
            const double red = lfRef > 1e-9 ? std::max(0.0, std::min(1.0, (1 - am / lfRef) / 0.5)) : 0;
            audPh = std::remainder(audPh + 2 * kPi * 800 / kW, 2 * kPi);
            aud.push_back((float)(0.4 * red * std::sin(audPh)));
        }
        if (j % 8 == 7) {
            const float e = (float)(std::abs(c) / kCar);
            put(env, n1k, e);
            const cd zprev = at(zc, n1k - 1);
            put(zc, n1k, c);
            cfoAcc += c * std::conj(zprev);
            const size_t b = (size_t)(n1k % 1000);
            prof[b] += 0.15f * (e - prof[b]);
            if (hf) prof100[b] += 0.15f * (at(env100, n1k) - prof100[b]);
            n1k++;
            if (n1k % 1000 == 0) second();
        }
    }

    // ---------------------------------------------------------------- the seconds
    static float circMean(const std::vector<float>& p, int a, int b) {   // inclusive, circular
        const int n = (int)p.size();
        double s = 0;
        for (int k = a; k <= b; k++) s += p[(size_t)(((k % n) + n) % n)];
        return (float)(s / (b - a + 1));
    }
    float pc(const std::vector<float>& p, int k) const { const int n = (int)p.size(); return p[(size_t)(((k % n) + n) % n)]; }

    void lfRegions(int b, int& h0, int& h1, int& l0, int& l1) const {
        switch (st) {
        case Dcf77: h0 = 300; h1 = 950; l0 = 20; l1 = 90; break;
        case Msf: h0 = 600; h1 = 950; l0 = 15; l1 = 85; break;
        case Wwvb: h0 = 850; h1 = 985; l0 = 20; l1 = 180; break;
        case Bpc: h0 = 450; h1 = 950; l0 = 20; l1 = 90; break;   // low for 100 - 400 ms from the second
        case Rwm: h0 = 12; h1 = 28; l0 = 400; l1 = 950; break;   // the pulse (40 ms in the 10 pulse minutes); the doubled pulse ends by 300 ms
        default: h0 = 20; h1 = 180; l0 = 850; l1 = 985; break;   // JJY
        }
        h0 += b; h1 += b; l0 += b; l1 += b;
    }

    void estimateLf() {
        const int sgn = risesOnSecond(st) ? -1 : 1;
        // prefix sums for the edge score
        std::vector<double> ps(2001, 0.0);
        for (int k = 0; k < 2000; k++) ps[(size_t)k + 1] = ps[(size_t)k] + prof[(size_t)(k % 1000)];
        auto mean = [&](int a, int b) { a += 1000; b += 1000; a %= 1000; b %= 1000; if (b < a) b += 1000; return (ps[(size_t)b + 1] - ps[(size_t)a]) / (b - a + 1); };
        int best = 0;
        double bs = -1e300;
        for (int b = 0; b < 1000; b++) {
            // RWM: the pulse on the second stays up longest (40 / 100 / 500 ms against the 20 ms pulses of the 10 pulse minutes)
            const double s = st == Rwm ? mean(b + 3, b + 35) - mean(b - 15, b - 3) : sgn * (mean(b - 12, b - 3) - mean(b + 3, b + 12));
            if (s > bs) { bs = s; best = b; }
        }
        int h0, h1, l0, l1;
        lfRegions(best, h0, h1, l0, l1);
        hi = circMean(prof, h0, h1); lo2 = circMean(prof, l0, l1);
        if (st == Rwm) {   // the low level between the 20 ms pulses of the 10 pulse minutes (quiet in the 1 pulse minutes as well)
            lo2 = 0;
            for (int k = 4; k <= 9; k++) lo2 += circMean(prof, best + k * 100 + 50, best + k * 100 + 90) / 6;
        }
        depth = hi > 1e-12 ? (hi - lo2) / hi : 0;
        const double lvl = (hi + lo2) / 2;
        double cross = best, bestD = 1e9;
        for (int k = best - 20; k <= best + 20; k++) {
            const double v0 = pc(prof, k), v1 = pc(prof, k + 1);
            const bool c = sgn > 0 ? (v0 >= lvl && v1 < lvl) : (v0 <= lvl && v1 > lvl);
            if (c && v1 != v0) {
                const double x = k + (v0 - lvl) / (v0 - v1);
                if (std::fabs(x - best) < bestD) { bestD = std::fabs(x - best); cross = x; }
            }
        }
        phase = std::fmod((cross - kEnvLag) / 1000.0 + 10.0, 1.0);
        level = (float)(20 * std::log10(hi + 1e-12));
    }

    // HF: the rising edge of the tick in a profile; returns the onset (8 kHz samples, mod 8000) and the tick's top and base
    double hfEdge(const std::vector<float>& p, double& tp, double& bs) const {
        const int L = tickL;
        int best = 0;
        double bd = -1e300;
        for (int b = 0; b < 8000; b++) {
            const double d = pc(p, b + L / 2) - pc(p, b - L / 2);
            if (d > bd) { bd = d; best = b; }
        }
        if (st == Bpm && bd > 0) {   // the pulse after the tick rises as fast: the tick is the edge 100 ms before it
            const int b0 = ((best - 800) % 8000 + 8000) % 8000;
            if (pc(p, b0 + L / 2) - pc(p, b0 - L / 2) > 0.6 * bd) best = b0;
        }
        tp = 0;
        for (int k = best; k <= best + L + 8; k++) tp = std::max(tp, (double)pc(p, k));
        bs = circMean(p, best - L - 240, best - L - 40);
        const double lvl = (tp + bs) / 2;
        double cross = best;
        for (int k = best - L; k <= best + L; k++) {
            const double v0 = pc(p, k), v1 = pc(p, k + 1);
            if (v0 < lvl && v1 >= lvl) { cross = k + (lvl - v0) / (v1 - v0); break; }
        }
        return cross + 0.5 - L / 2.0;
    }

    // BPM coded: the second's S burst is the one with nothing for 100 ms before it and the empty element after it
    void estimateBpmc() {
        auto P = [&](int k) { return (double)prof100[(size_t)(((k % 1000) + 1000) % 1000)]; };
        int best = 0;
        double bs = -1e300, bt = 0, bb = 0;
        for (int b = 0; b < 1000; b++) {
            const double pk = std::max(P(b), std::max(P(b - 1), P(b + 1)));
            double q = 0;
            for (int k = b - 100; k <= b - 9; k += 2) q = std::max(q, P(k));
            for (int k = b + 8; k <= b + 12; k++) q = std::max(q, P(k));
            if (pk - q > bs) { bs = pk - q; best = b; bt = pk; bb = q; }
        }
        const double a = P(best - 1), b = P(best), c = P(best + 1);
        const double den = a - 2 * b + c;
        const double dl = std::fabs(den) > 1e-12 ? std::max(-1.0, std::min(1.0, 0.5 * (a - c) / den)) : 0.0;   // the burst's peak between the 1 ms samples
        top = bt; base = bb;
        depth = bt > 1e-12 ? (bt - bb) / bt : 0;
        phase = std::fmod((best + dl - kBurstLag) / 1000.0 + 10.0, 1.0);
        tick0Db = (float)(20 * std::log10(bt + 1e-12)); tick1Db = -200.f;
        hawaii = false;
        level = (float)(20 * std::log10(circMean(prof, 0, 999) + 1e-12));
    }

    void estimateHf() {
        if (st == BpmC) { estimateBpmc(); return; }
        double tpA, bsA, tpB = 0, bsB = 0;
        const double oA = hfEdge(pT0, tpA, bsA);
        double o = oA;
        hawaii = false;
        if (st == Wwv) {
            const double oB = hfEdge(pT1, tpB, bsB);
            if (tpB - bsB > tpA - bsA) { o = oB; hawaii = true; }
        }
        tick0Db = (float)(20 * std::log10(tpA + 1e-12));
        tick1Db = st == Wwv ? (float)(20 * std::log10(tpB + 1e-12)) : -200.f;
        top = hawaii ? tpB : tpA; base = hawaii ? bsB : bsA;
        depth = top > 1e-12 ? (top - base) / top : 0;
        phase = std::fmod(o / kW + (st == Hla ? 0.032 : 0.0) + 10.0, 1.0);   // HLA: the pulse (the second) starts 32 ms after the tick
        level = (float)(20 * std::log10(circMean(prof, 0, 999) + 1e-12));   // the carrier
    }

    void second() {
        profSecs++;
        const double cf = std::arg(cfoAcc) / (2 * kPi * 1e-3);
        cfo = std::abs(cfoAcc) > 0 ? cf : 0;
        cfoAcc = 0;
        if (amAudio(st)) estimateHf(); else estimateLf();
        const bool was = locked;
        const double need = amAudio(st) ? 0.5 : 0.3;
        if (profSecs >= 4) locked = locked ? depth > need * 0.7 : depth > need;
        if (!locked) {
            if (was) unlockedSince = tNow();
            if (anchored && unlockedSince >= 0 && tNow() - unlockedSince > 60) anchored = false;
            nextSec = -1;
            return;
        }
        unlockedSince = -1;
        if (nextSec < 0) {
            nextSec = phase + std::floor(tNow() - 1.1 - phase);
            if (!was && anchored) anchored = false;   // the seconds were lost: count them anew
            syms.clear();
        }
        while (true) {
            const double S = nextSec;
            if ((S + 1.05) * 1000 + kEnvLag + 25 >= (double)n1k || (S + 1.05) * kW >= (double)k8) break;
            if ((double)n1k - S * 1000 > kR1 - 200) { nextSec = -1; break; }
            processSecond(S);
            double nx = S + 1;
            double adj = std::remainder(phase - nx, 1.0);
            if (std::fabs(adj) > 0.05) { syms.clear(); anchored = false; }
            else adj *= 0.5;
            nextSec = nx + adj;
        }
    }

    double envMean(const std::vector<float>& r, double S, int a, int b) const {   // ms a .. b after S
        const int64_t i0 = (int64_t)std::llround(S * 1000) + kEnvLag;
        double s = 0;
        for (int k = a; k <= b; k++) s += at(r, i0 + k);
        return s / (b - a + 1);
    }

    uint8_t classify(double S, float& snrOut) {
        double u[10];
        const bool hf = st == Wwv || st == Bpm || st == Hla;
        const std::vector<float>& r = hf ? env100 : env;
        double H = hi, Lo = lo2;
        if (st == Bpm) {   // the pulse after the tick: always there 100 - 200 ms, quiet in the last part of the second
            H = circMean(prof100, (int)std::llround(phase * 1000) + kEnvLag + 120, (int)std::llround(phase * 1000) + kEnvLag + 190);
            Lo = circMean(prof100, (int)std::llround(phase * 1000) + kEnvLag + 910, (int)std::llround(phase * 1000) + kEnvLag + 985);
        } else if (hf) { H = circMean(prof100, (int)std::llround(phase * 1000) + kEnvLag + 40, (int)std::llround(phase * 1000) + kEnvLag + (st == Hla ? 150 : 190));
                  Lo = circMean(prof100, (int)std::llround(phase * 1000) + kEnvLag + 910, (int)std::llround(phase * 1000) + kEnvLag + 985); }
        const double span = H - Lo;
        if (std::fabs(span) < 1e-12) return SUnknown;
        for (int k = 0; k < 10; k++) u[k] = (envMean(r, S, k * 100 + (hf ? 40 : 15), k * 100 + (hf ? 90 : 85)) - Lo) / span;
        // noise in an always-high part of this second
        if (stationKeysCarrier(st)) {
            int h0, h1, l0, l1;
            lfRegions(0, h0, h1, l0, l1);
            const int64_t i0 = (int64_t)std::llround(S * 1000) + kEnvLag;
            double m = 0, q = 0;
            for (int k = h0; k <= h1; k++) { const double v = at(env, i0 + k); m += v; q += v * v; }
            const int n = h1 - h0 + 1;
            m /= n;
            const double sd = std::sqrt(std::max(1e-24, q / n - m * m));
            snrOut = (float)std::min(60.0, 20 * std::log10(std::max(1e-12, span) / sd));
        }
        if (st == Rwm) return rwmClassify(S);
        auto avg = [&](int a, int b) { double s = 0; for (int k = a; k <= b; k++) s += u[k]; return s / (b - a + 1); };
        switch (st) {
        case Dcf77: return u[0] > 0.5 ? SNone : (u[1] < 0.5 ? S1 : S0);
        case Msf:
            if (u[0] > 0.5) return SUnknown;
            if (avg(3, 4) < 0.5) return SMark;
            return (uint8_t)((u[1] < 0.5 ? 1 : 0) | (u[2] < 0.5 ? 2 : 0));
        case Wwvb:
            if (u[0] > 0.5) return SUnknown;
            if (avg(5, 7) < 0.5) return SMark;
            return avg(2, 4) < 0.5 ? S1 : S0;
        case Bpm:
            if (u[1] < 0.5) return SUnknown;
            if (u[3] >= 0.5) return SMark;
            return u[2] >= 0.5 ? S1 : S0;
        case Bpc:   // the carrier is low for 0.1 - 0.4 s: the symbol counts the low tenths after the first; none at all is P0
            if (u[0] > 0.5) return SNone;
            return u[1] >= 0.5 ? 0 : u[2] >= 0.5 ? 1 : u[3] >= 0.5 ? 2 : 3;
        case Hla:   // 0.17 s, 0.47 s and 0.77 s pulses; nothing in second 0
            if (u[0] < 0.5) return SNone;
            if (avg(5, 6) >= 0.5) return SMark;
            return avg(2, 3) >= 0.5 ? S1 : S0;
        case Wwv:
            if (u[0] < 0.5) return SNone;
            if (avg(5, 7) >= 0.5) return SMark;
            return avg(2, 4) >= 0.5 ? S1 : S0;
        default:   // JJY: the time high
            if (u[0] < 0.5) return SUnknown;
            if (avg(2, 4) < 0.5) return SMark;
            return avg(5, 7) < 0.5 ? S1 : S0;
        }
    }

    // RWM: the minute pulse, a 10 pulse second, a doubled or a single pulse, against the gaps of this second (300 - 400 and 600 - 1000 ms
    // are quiet in the 1 pulse minutes, and between the 20 ms pulses in the 10 pulse minutes)
    uint8_t rwmClassify(double S) {
        const double P = envMean(env, S, 12, 28);
        double G = 0, T = 0;
        for (int k = 6; k <= 9; k++) { G += envMean(env, S, k * 100 + 50, k * 100 + 90); T += envMean(env, S, k * 100 + 4, k * 100 + 16); }
        G /= 4; T /= 4;
        const double span = P - G;
        if (span < 0.3 * hi) return SUnknown;
        auto u = [&](double v) { return (v - G) / span; };
        if (u(envMean(env, S, 320, 480)) > 0.5) return SMark;
        if (u(T) > 0.4) return SB;
        if (u(envMean(env, S, 120, 180)) > 0.5) return SUnknown;   // a single pulse is down by 120 ms
        return u(envMean(env, S, 215, 285)) > 0.5 ? S1 : S0;
    }

    // the edge of this one second (ms from S), NAN if none
    double edgeOf(double S) const {   // HLA: the tick is 32 ms before the second
        if (amAudio(st)) {
            const std::vector<float>& r = hawaii ? tk1 : tk0;
            const double lvl = (top + base) / 2;
            if (st == Hla) S -= 0.032;
            const int64_t c = (int64_t)std::llround(S * kW - 0.5 + tickL / 2.0);
            for (int64_t k = c - tickL - 40; k <= c + tickL; k++) {
                const double v0 = at(r, k), v1 = at(r, k + 1);
                if (v0 < lvl && v1 >= lvl) return ((k + (lvl - v0) / (v1 - v0) + 0.5 - tickL / 2.0) / kW - S) * 1000;
            }
            return NAN;
        }
        const int sgn = risesOnSecond(st) ? -1 : 1;
        const double lvl = (hi + lo2) / 2;
        const int64_t c = (int64_t)std::llround(S * 1000) + kEnvLag;
        double best = NAN;
        for (int64_t k = c - 25; k <= c + 25; k++) {
            const double v0 = at(env, k), v1 = at(env, k + 1);
            const bool x = sgn > 0 ? (v0 >= lvl && v1 < lvl) : (v0 <= lvl && v1 > lvl);
            if (x && v0 != v1) {
                const double e = k + (v0 - lvl) / (v0 - v1) - kEnvLag - S * 1000;
                if (std::isnan(best) || std::fabs(e) < std::fabs(best)) best = e;
            }
        }
        return best;
    }

    // CHU: the 10 bytes of FSK in this second, false when there are none
    bool chuBytes(double S, uint8_t* out) const {
        const double bit = kW / 300;
        const int64_t j0 = (int64_t)std::llround(S * kW);
        int64_t from = j0 + (int64_t)(0.012 * kW);
        for (int c = 0; c < 10; c++) {
            // a start bit: mark, then space
            int64_t jc = -1;
            const int64_t to = c == 0 ? j0 + (int64_t)(0.15 * kW) : from + (int64_t)(bit * 1.5);
            for (int64_t k = std::max(from, j0 + 8); k < to; k++)
                if (at(fsk, k - 1) > 0 && at(fsk, k) <= 0) { jc = k; break; }
            if (jc < 0) return false;
            int v = 0;
            for (int b = 0; b < 11; b++) {
                const double x = at(fsk, jc + (int64_t)std::llround((b + 0.5) * bit));
                const int bv = x > 0 ? 1 : 0;
                if (b == 0 && bv != 0) return false;
                if (b >= 9 && bv != 1) return false;
                if (b >= 1 && b <= 8) v |= bv << (b - 1);
            }
            out[c] = (uint8_t)v;
            from = jc + (int64_t)std::llround(11 * bit) - (int64_t)(bit / 2);
        }
        return true;
    }

    double wallOf(double S) const {
        const int m = clockMode.load();
        if (m == 2) return clockOrigin.load() + S;
        if (m == 1) { const double a = std::min(wallMinCur, wallMinPrev); return a < 1e299 ? a + S : NAN; }
        return NAN;
    }

    void addLog(bool ok, const std::string& text, const std::string& bits) {
        TimeFrameLog l;
        l.wallTime = (int64_t)time(nullptr); l.ok = ok; l.text = text; l.bits = bits;
        flog.push_front(std::move(l));
        while (flog.size() > kTimeLogMax) flog.pop_back();
        if (log) log(std::string("Time signal: ") + stationName(st) + (ok ? " " : " rejected: ") + text);
    }

    void setAnchor(int64_t count, int64_t utc) {
        if (anchored && anchorUtc + (count - anchorCount) == utc) framesInRow++; else framesInRow = 1;
        anchored = true; anchorCount = count; anchorUtc = utc;
        lastFrameSec = tNow();
    }

    int yearHint() const {
        if (anchored) { int y, m, d; civilFromDays(anchorUtc / 86400, y, m, d); return y; }
        return 0;
    }

    // BPM coded: the 52 code elements after this second's S burst (10 ms apart, a burst in the first 5 ms is a 1). A second frame names the whole
    // time, so one good frame sets the clock; the minute frame (M, 300 ms) only brings the 0.1 ms DUT1 digit and the leap second data.
    uint8_t bpmcSecond(double S, int64_t c) {
        const int64_t i0 = (int64_t)std::llround(S * 1000) + kBurstLag;
        const double thr = 0.4 * top;
        uint8_t e[52];
        std::string bits(52, '0');
        int ones30 = 0;
        for (int k = 0; k < 52; k++) {
            double v = 0;
            for (int d = -1; d <= 1; d++) v += at(env100, i0 + 10 * k + d);
            e[k] = v / 3 > thr ? 1 : 0;
            bits[(size_t)k] = (char)('0' + e[k]);
            if (k < 30) ones30 += e[k];
        }
        if (ones30 >= 28) {   // M: the minute frame
            int tenth, leap;
            bool ls;
            if (!decodeBpmcMinute(e, tenth, leap, ls)) { badN++; addLog(false, "minute frame: parity or a fixed element is wrong", bits.substr(0, 46)); return SMark; }
            mcTenth = tenth; mcLeap = leap; mcLs = ls; mcS = S;
            if (anchored && (anchorUtc + (c - anchorCount)) % 60 == 0) {
                fields.dut1Tenth = tenth; fields.leapCount = leap; fields.leapAnnounce = ls;
                if (fields.hasDut1Ms) fields.dut1Ms = (fields.dut1Neg ? -1.0 : 1.0) * (std::floor(std::fabs(fields.dut1Ms)) + 0.1 * tenth);
            }
            char t[112];
            snprintf(t, sizeof t, "minute frame: DUT1 0.1 ms digit %d, TAI - UTC %d s, leap second %s", tenth, leap, ls ? "at the end of the month" : "none");
            okN++;
            addLog(true, t, bits.substr(0, 46));
            return SMark;
        }
        if (!e[0]) return SUnknown;
        Fields f;
        if (!decodeBpmcSecond(e, f, yearHint())) { badN++; return SUnknown; }
        const int64_t utc = f.utcMinute + f.second;
        if (mcS >= 0 && std::fabs(S - mcS - f.second) < 1.5) { mcUtc = f.utcMinute; mcS = -1; }   // the first second frame after an M names its minute
        if (mcUtc == f.utcMinute) {
            f.dut1Tenth = mcTenth; f.leapCount = mcLeap; f.leapAnnounce = mcLs != 0;
            f.dut1Ms = (f.dut1Neg ? -1.0 : 1.0) * (std::floor(std::fabs(f.dut1Ms)) + 0.1 * mcTenth);
        }
        okN++;
        fields = f;
        setAnchor(c, utc);
        if (lastLogCount < 0 || c - lastLogCount >= 60) { lastLogCount = c; addLog(true, fieldsText(f, st), bits); }
        return S1;
    }

    void processSecond(double S) {
        const int64_t c = secCount++;
        lastS = S; lastCount = c;
        float sn = snr;
        const uint8_t s = st == Chu ? (uint8_t)SUnknown : st == BpmC ? bpmcSecond(S, c) : classify(S, sn);
        if (amAudio(st)) {   // the tick against the noise between ticks
            const std::vector<float>& r = hawaii ? tk1 : tk0;
            const int64_t j0 = (int64_t)std::llround(S * kW);
            double m = 0, q = 0;
            int n = 0;
            for (int64_t k = j0 + 4400; k < j0 + 7600; k += 4) { const double v = at(r, k); m += v; q += v * v; n++; }
            m /= n;
            const double sd = std::sqrt(std::max(1e-24, q / n - m * m));
            sn = (float)std::min(60.0, 20 * std::log10(std::max(1e-12, top - base) / std::max(sd, 1e-9)));
        }
        snr = sn;
        const double e = edgeOf(S);
        if (!std::isnan(e)) { resid.push_back(e); while (resid.size() > 60) resid.pop_front(); tickH.push_back((float)e); }
        else tickH.push_back(NAN);
        levelH.push_back(level); snrH.push_back(snr);
        syms.emplace_back(c, s);
        while (syms.size() > 130) syms.pop_front();

        if (st == Chu) {
            uint8_t b[10];
            if (chuBytes(S, b)) {
                int doy, h, m, sec;
                if (chuParseA(b, doy, h, m, sec)) {
                    int y = yearHint();
                    if (!y) { int mo, d; civilFromDays((int64_t)time(nullptr) / 86400, y, mo, d); }
                    const int64_t utc = (daysFromCivil(y, 1, 1) + doy - 1) * 86400 + h * 3600 + m * 60 + sec;
                    const bool first = !anchored || anchorUtc + (c - anchorCount) != utc;
                    setAnchor(c, utc);
                    if (sec == 39 || first) {
                        okN++;
                        fields = Fields();
                        fields.utcMinute = utc - sec; fields.doy = doy; fields.hour = h; fields.minute = m;
                        civilFromDays(utc / 86400, fields.year, fields.month, fields.day);
                        fields.weekday = weekdayOf(utc / 86400);
                        char t[64];
                        snprintf(t, sizeof t, "%04d-%02d-%02d %02d:%02d:%02d UTC (day %d)", fields.year, fields.month, fields.day, h, m, sec, doy);
                        addLog(true, t, "");
                    }
                } else if (!chuIsB(b)) {
                    badN++;
                }
            }
        } else if (st == BpmC) {
            // decoded above, one second at a time
        } else if (st == Bpc) {   // a 20 s frame, from its first (empty) second
            if (syms.size() >= 20 && syms.back().first - syms[syms.size() - 20].first == 19) {
                uint8_t w[20];
                for (int k = 0; k < 20; k++) w[k] = syms[syms.size() - 20 + (size_t)k].second;
                if (w[0] == SNone) {
                    std::string bits(20, ' ');
                    for (int k = 0; k < 20; k++) bits[(size_t)k] = symChar(w[k], st);
                    Fields f;
                    if (decodeMinute(st, w, f, yearHint())) {
                        okN++;
                        fields = f;
                        setAnchor(c - 19, f.utcMinute);
                        addLog(true, fieldsText(f, st), bits);
                    } else {
                        badN++;
                        addLog(false, "parity, the frame index or a range is wrong", bits);
                    }
                }
            }
        } else if (syms.size() >= 60 && syms.back().first - syms[syms.size() - 60].first == 59) {
            uint8_t w[60];
            for (int k = 0; k < 60; k++) w[k] = syms[syms.size() - 60 + (size_t)k].second;
            bool cand = false;
            switch (st) {
            case Dcf77: cand = w[59] == SNone; break;
            case Msf: cand = w[0] == SMark; break;
            case Wwv: case Hla: cand = w[0] == SNone && w[59] == SMark; break;
            case Rwm: cand = w[0] == SMark; break;
            default: cand = w[0] == SMark && w[59] == SMark; break;
            }
            if (cand) {
                std::string bits(60, ' ');
                for (int k = 0; k < 60; k++) bits[(size_t)k] = symChar(w[k], st);
                Fields f;
                if (st == Rwm && decodeMinute(st, w, f)) {   // no time of day on air: the minute from the computer clock, the second from the pulses
                    okN++;
                    fields = f;
                    const double w0 = wallOf(S - 59) - propMs() * 1e-3;
                    if (!std::isnan(w0)) { f.utcMinute = (int64_t)std::llround(w0 / 60) * 60; fields = f; setAnchor(c - 59, f.utcMinute); }
                    addLog(true, std::isnan(w0) ? "minute pulse" + fieldsText(f, st).substr(std::string("1970-01-01 00:00 UTC").size()) : fieldsText(f, st), bits);
                } else if (st == Rwm) {
                    badN++;
                    addLog(false, "the doubled pulses are not a DUT1 / dUT1 code", bits);
                } else if (decodeMinute(st, w, f, yearHint())) {
                    okN++;
                    fields = f;
                    setAnchor(c - 59, f.utcMinute);
                    addLog(true, fieldsText(f, st), bits);
                } else {
                    badN++;
                    addLog(false, "parity, a fixed bit or a range is wrong", bits);
                }
            }
        }

        // the computer clock against the received second
        if (anchored) {
            const double utc = (double)(anchorUtc + (c - anchorCount));
            const double w = wallOf(S);
            if (!std::isnan(w)) {
                const double ut1Adv = st == Bpm && fields.ut1 && fields.dut1 != 99 ? fields.dut1 * 0.1 : 0;   // BPM UT1 minutes: the ticks come early by DUT1
                const double off = (w - utc + ut1Adv - propMs() * 1e-3) * 1000;
                offs.push_back((float)off);
                while (offs.size() > 60) offs.pop_front();
                offH.push_back((float)off);
            }
        }
        for (auto* h : {&levelH, &snrH, &tickH, &offH}) while (h->size() > kTimeHist) h->pop_front();
    }

    double distKm() const {
        double la, lo;
        stationSite(st, la, lo, hawaii);
        return distanceKm(rxLat.load(), rxLon.load(), la, lo);
    }
    double propMs() const { return propagationMs(st, distKm()); }

    void report() {
        TimeTelemetry t;
        t.seq = ++seq;
        t.station = st; t.hawaii = hawaii;
        t.tick1000Db = tick0Db; t.tick1200Db = tick1Db;
        t.timeSec = tNow(); t.inputRate = rate;
        t.cfoHz = cfo; t.snrDb = snr; t.levelDb = level; t.depth = (float)depth;
        t.secPhaseMs = phase * 1000;
        t.dcfPhaseLocked = dcfPhase.status().locked; t.dcfPhaseCorr = dcfPhase.status().corr; t.dcfPhaseBit = dcfPhase.status().lastBit;
        t.wwvbPhaseLocked = wwvbPhase.status().locked; t.wwvbPhaseUtc = wwvbPhase.status().utcMinute;
        if (resid.size() >= 3) {
            double m = 0, q = 0;
            for (double r : resid) m += r;
            m /= (double)resid.size();
            for (double r : resid) q += (r - m) * (r - m);
            t.tickJitterMs = std::sqrt(q / (double)resid.size());
        }
        t.timeValid = anchored && tNow() - lastFrameSec < 900;
        t.state = t.timeValid ? 2 : locked ? 1 : 0;
        t.dataValid = locked;
        t.confirmed = t.timeValid && framesInRow >= 2;
        t.blocksOk = okN; t.blocksBad = badN;
        t.fields = fields;
        if (anchored && lastCount >= 0) {
            t.utcNow = (double)(anchorUtc + (lastCount - anchorCount)) + (tNow() - lastS);
            t.secondOfMinute = (int)(((anchorUtc + (lastCount - anchorCount)) % 60 + 60) % 60);
        }
        t.secsSinceFrame = anchored ? tNow() - lastFrameSec : 0;
        // the symbols of the minute so far
        if (!syms.empty()) {
            size_t from = syms.size() > 60 ? syms.size() - 60 : 0;
            const int into = st == Bpc ? t.secondOfMinute % 20 : st == BpmC ? 0 : t.secondOfMinute;
            if (t.secondOfMinute >= 0 && st != Chu) from = syms.size() > (size_t)into ? syms.size() - 1 - (size_t)into : 0;
            for (size_t k = from; k < syms.size(); k++) t.bitsNow += symChar(syms[k].second, st);
        }
        t.clockValid = clockMode.load() != 0 && !offs.empty() && t.timeValid;
        if (!offs.empty()) {
            t.clockOffsetMs = offs.back();
            double s = 0;
            for (float o : offs) s += o;
            t.clockOffsetAvgMs = s / (double)offs.size();
        }
        t.locationSet = rxLocSet.load();
        t.distanceKm = distKm(); t.propDelayMs = propMs();
        t.levelHist.assign(levelH.begin(), levelH.end()); t.snrHist.assign(snrH.begin(), snrH.end());
        t.tickHist.assign(tickH.begin(), tickH.end()); t.offsetHist.assign(offH.begin(), offH.end());
        t.log.assign(flog.begin(), flog.end());
        std::lock_guard<std::mutex> lk(mu);
        pub = std::move(t);
    }
};

TimeReceiver::TimeReceiver() : p_(std::make_unique<Impl>()) {}
TimeReceiver::~TimeReceiver() = default;

void TimeReceiver::configure(double r) { std::lock_guard<std::mutex> lk(p_->mu); p_->rateSet = r; p_->resetReq = true; }
void TimeReceiver::setSignalOffset(double hz) { std::lock_guard<std::mutex> lk(p_->mu); if (hz != p_->offsetSet) { p_->offsetSet = hz; p_->resetReq = true; } }
void TimeReceiver::setDialHz(double hz) {
    if (hz == p_->dialHz.load()) return;
    p_->dialHz = hz;
    if (p_->stationReq.load() < 0) p_->resetReq = true;
}
void TimeReceiver::setListen(bool on) { p_->listen.on = on; }
bool TimeReceiver::listening() const { return p_->listen.on.load(); }
void TimeReceiver::setVolume(float v) { p_->listen.volume = std::max(0.f, std::min(1.f, v)); }
void TimeReceiver::setMuted(bool m) { p_->listen.muted = m; }
void TimeReceiver::setSilent(bool s) { p_->listen.silent = s; }
void TimeReceiver::setListenTap(std::function<void(const float* x, size_t n)> cb) { p_->listen.setTap(std::move(cb)); }
void TimeReceiver::setClockScale(double k) {
    if (!(k > 0.5 && k < 2)) k = 1;
    if (k != p_->clockScale.exchange(k)) p_->resetReq = true;
}
void TimeReceiver::setAudioInput(bool on, double toneHz) {
    const bool o = p_->audioIn.exchange(on);
    const double t = p_->audioTone.exchange(toneHz);
    if (o != on || t != toneHz) p_->resetReq = true;
}
void TimeReceiver::setStation(int s) { if (s != p_->stationReq.exchange(s)) p_->resetReq = true; }
void TimeReceiver::setPref60(int s) { if (s != p_->pref60.exchange(s) && p_->stationReq.load() < 0) p_->resetReq = true; }
void TimeReceiver::setClockMode(int mode, double originSec) { p_->clockOrigin = originSec; p_->clockMode = mode; }
void TimeReceiver::setReceiverLocation(double lat, double lon, bool userSet) { p_->rxLat = lat; p_->rxLon = lon; p_->rxLocSet = userSet; }
bool TimeReceiver::ready() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->rateSet >= (p_->audioIn.load() ? 16000.0 : timeTuning().minSampleRate - 1); }
int TimeReceiver::station() const { return p_->stationNow.load(); }
void TimeReceiver::reset() { p_->resetReq = true; }
void TimeReceiver::clearLog() {
    { std::lock_guard<std::mutex> lk(p_->mu); p_->pub.log.clear(); p_->logLoad = false; p_->logIn.clear(); p_->pub.seq++; }
    p_->clearReq = true;
}
// the decoded minutes already stay through reset() (they carry the computer's clock): only Clear empties them
void TimeReceiver::clearResults() { clearLog(); }
std::string TimeReceiver::saveResults() const {
    std::lock_guard<std::mutex> lk(p_->mu);
    return results::pack(p_->pub.log);
}
bool TimeReceiver::loadResults(const std::string& blob) {
    std::vector<TimeFrameLog> l;
    if (!results::unpack(blob, l)) return false;
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->logIn.assign(l.begin(), l.end());
    p_->pub.log = std::move(l);
    p_->logLoad = true;
    p_->pub.seq++;
    return true;
}

void TimeReceiver::feed(const cf32* x, size_t n) {
    Impl& m = *p_;
    if (m.resetReq.exchange(false)) m.resetState();
    if (m.clearReq.exchange(false)) { m.flog.clear(); m.okN = m.badN = 0; }
    { std::lock_guard<std::mutex> lk(m.mu); if (m.logLoad) { m.flog = std::move(m.logIn); m.logIn.clear(); m.logLoad = false; } }
    if (m.rateRaw < (m.audioIn.load() ? 16000.0 : timeTuning().minSampleRate - 1) || n == 0) return;
    const double r = m.rate;
    const bool audioIn = m.audioIn.load();
    for (size_t i = 0; i < n; i++) {
        float re = x[i].real(), im = x[i].imag();
        if (!std::isfinite(re) || !std::isfinite(im)) re = im = 0;
        if (audioIn) { im = 0; if (amAudio(m.st)) re += 1.f; }   // demodulated audio: an AM envelope around 1 (HF) or a real tone (LF)
        m.acc += cd(re, im) * m.lo;
        m.accN++;
        m.lo *= m.loStep;
        if (++m.loCount >= 4096) { m.loCount = 0; m.lo /= std::abs(m.lo); }
        m.nIn++;
        if ((double)m.nIn >= m.binEnd) {
            m.sample8(m.acc / (double)m.accN);
            m.acc = 0; m.accN = 0;
            m.binEnd = (double)(m.k8 + 1) * r / kW;
        }
        if (m.tNow() >= m.nextReport) { m.nextReport += 0.25; m.report(); }
    }
    if (!m.aud.empty()) { m.listen.play(m.aud.data(), m.aud.size()); m.aud.clear(); }
    if (m.clockMode.load() == 1) {   // the earliest arrival over the last half minute stands for sample 0's time
        const double w = wallNow();
        const double v = w - (double)m.nIn / r;
        if (m.wallWin == 0) m.wallWin = w;
        if (w - m.wallWin > 30) { m.wallMinPrev = m.wallMinCur; m.wallMinCur = 1e300; m.wallWin = w; }
        m.wallMinCur = std::min(m.wallMinCur, v);
    }
}

bool TimeReceiver::telemetry(TimeTelemetry& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (p_->pub.seq <= lastSeq) return false;
    out = p_->pub;
    return true;
}
void TimeReceiver::setLogCallback(std::function<void(const std::string&)> cb) { std::lock_guard<std::mutex> lk(p_->mu); p_->log = std::move(cb); }

// 4.0: the stations with a mode-list entry of their own (family, active standard family + 1, engine standard family + 2, poll slot = family); CHU is not offered (removed, #150)
static const TimeView kTimeViews[] = {
    {607, "dcf77", "DCF77 (Germany, 77.5 kHz)", 0},
    {610, "msf", "MSF (United Kingdom, 60 kHz)", 1},
    {613, "wwvb", "WWVB (United States, 60 kHz)", 2},
    {616, "jjy40", "JJY 40 kHz (Japan)", 3},
    {628, "jjy60", "JJY 60 kHz (Japan)", 4},
    {631, "wwv", "WWV / WWVH (United States, HF)", 5},
    {634, "bpm", "BPM (China, HF)", 7},
    {637, "bpc", "BPC (China, 68.5 kHz)", 8},
    {642, "hla", "HLA (Korea, 5 MHz)", 9},
    {645, "bpmcoded", "BPM coded (China, HF)", 10},
    {648, "rwm", "RWM (Russia, 9996 kHz)", 11},
};
const TimeView* timeViews(int& count) { count = (int)(sizeof kTimeViews / sizeof *kTimeViews); return kTimeViews; }
const TimeView* timeViewForActive(int a) { for (const auto& v : kTimeViews) if (v.family + 1 == a) return &v; return nullptr; }
const TimeView* timeViewForStd(int s) { for (const auto& v : kTimeViews) if (v.family + 2 == s) return &v; return nullptr; }
ModeTuning timeViewTuning(int i) {
    ModeTuning t = timeTuning();
    const TimeView& v = kTimeViews[i];
    t.stdMode = v.family + 2; t.id = v.id; t.name = v.name; t.defMhz = timesig::stationKhz(v.station) / 1000.0;
    return t;
}

ModeTuning timeTuning() {
    ModeTuning t;
    t.stdMode = 60; t.id = "time"; t.name = "Time signals";
    t.minMhz = 0.03; t.maxMhz = 30; t.defMhz = 0.0775;   // DCF77
    t.sampleRate = 256000;                   // a rate most LF / HF radios offer (RTL-SDR direct sampling, Airspy HF+, SDRplay)
    t.basebandHz = 0;
    t.bandwidthMhz = 0.01;
    t.minSampleRate = 48000;
    t.tuneOffsetHz = 12000;                  // the radio sits 12 kHz above the station, away from its DC spike
    return t;
}

} // namespace dect2
