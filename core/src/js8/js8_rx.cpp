// JS8Call receiver (see js8_rx.h).
#include "dect2/common/thread_name.h"
#include "dect2/js8/js8_rx.h"
#include "dect2/hfdig/clock_comp.h"
#include "dect2/net/pskreporter.h"
#include "dect2/results/results_io.h"
#include "dect2/js8/js8_tel.h"
#include "dect2/js8/js8_proto.h"
#include "dect2/marine/marine_dsp.h"
#include "../audio/ham_listen.h"
#include "dect2/dsp/fftutil.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <complex>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>
#include "dect2/common/mathconst.h"

namespace dect2 {

namespace {
using dect2::math::kPi;
constexpr double kBase = marine::kBaseRate;      // 24 kHz complex
constexpr int kDec = 6;                          // -> 4 kHz
constexpr double kFs = kBase / kDec;
constexpr double kCentreHz = 1750;               // dial + 1750 Hz is 0 Hz of the 4 kHz stream
constexpr double kLoHz = 200, kHiHz = 3200;      // tone 0 .. the top tone of a signal stay in here
constexpr int kTaps = 121;
constexpr int kSpecN = 1024;

double nowUtc() { return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count(); }

// the time-offset search of each submode (s, against the nominal start)
constexpr double kDtMin[3] = {-1.5, -1.2, -0.8}, kDtMax[3] = {2.5, 2.0, 1.2};

double preSec(int m) { return std::max(0.0, -(js8::spec(m).startSec + kDtMin[m])) + 0.1; }
double postSec(int m) { const auto& s = js8::spec(m); return std::max(0.0, s.startSec + kDtMax[m] + s.durSec() - s.period) + 0.1; }

struct Job {
    int mode = 0;
    int64_t slot = 0;
    double startUtc = 0;         // of x[0]
    std::vector<cf32> x;
};

struct Cand { double fb = 0; int s = 0; float m = 0; };

// one slot of one submode
struct SlotDecoder {
    int mode, sps, nfft, hop;
    double baud, binHz;
    std::vector<std::vector<cf32>> tw;   // e^{-j 2 pi t n / sps}, t = 0 .. 7
    Fft fft;
    explicit SlotDecoder(int m) : mode(m), fft(1) {
        sps = js8::spec(m).sps12k / 3;
        baud = kFs / sps;
        nfft = 1;
        while (nfft < 2 * sps) nfft <<= 1;
        fft = Fft(nfft);
        binHz = kFs / nfft;
        hop = sps / 4;
        tw.assign(8, std::vector<cf32>((size_t)sps));
        for (int t = 0; t < 8; t++) for (int n = 0; n < sps; n++) tw[(size_t)t][(size_t)n] = std::polar(1.f, (float)(-2 * kPi * t * n / sps));
    }

    // power of tone t of every symbol: X[i][t] (complex) of the signal mixed so that tone 0 is at 0 Hz
    void symbols(const std::vector<cf32>& y, int s, int count, const int* which, std::vector<std::array<cf32, 8>>& X) const {
        X.assign((size_t)count, {});
        for (int c = 0; c < count; c++) {
            const int i = which ? which[c] : c;
            const int a = s + i * sps;
            for (int t = 0; t < 8; t++) {
                cf32 acc(0, 0);
                if (a >= 0 && a + sps <= (int)y.size()) {
                    const cf32* p = &y[(size_t)a];
                    const cf32* w = tw[(size_t)t].data();
                    for (int n = 0; n < sps; n++) acc += p[n] * w[n];
                }
                X[(size_t)c][(size_t)t] = acc;
            }
        }
    }

    void mix(const std::vector<cf32>& x, double fb, std::vector<cf32>& y) const {
        y.resize(x.size());
        std::complex<double> r(1, 0);
        const std::complex<double> st = std::polar(1.0, -2 * kPi * fb / kFs);
        for (size_t n = 0; n < x.size(); n++) {
            y[n] = cf32((float)(x[n].real() * r.real() - x[n].imag() * r.imag()), (float)(x[n].real() * r.imag() + x[n].imag() * r.real()));
            r *= st;
            if ((n & 1023) == 1023) r /= std::abs(r);
        }
    }

    float syncMetric(const std::vector<cf32>& y, int s) const {
        static const int idx[21] = {0, 1, 2, 3, 4, 5, 6, 36, 37, 38, 39, 40, 41, 42, 72, 73, 74, 75, 76, 77, 78};
        std::vector<std::array<cf32, 8>> X;
        symbols(y, s, 21, idx, X);
        double a = 0, tot = 0;
        for (int c = 0; c < 21; c++) {
            const int cs = idx[c] / 36;
            const int tone = js8::costas(mode, cs)[idx[c] - 36 * cs];
            for (int t = 0; t < 8; t++) { const double p = std::norm(X[(size_t)c][(size_t)t]); tot += p; if (t == tone) a += p; }
        }
        return (float)(a / std::max(1e-30, (tot - a) / 7));
    }

    // candidates from the spectrogram: tone 0 on a half-tone grid, start on a quarter-symbol grid
    std::vector<Cand> candidates(const std::vector<cf32>& x, int s0) {
        const int nFrames = ((int)x.size() - sps) / hop + 1;
        if (nFrames < 4 * js8::kSymbols) return {};
        const double gStep = baud / 2;
        const int G = (int)((kHiHz - kLoHz) / gStep) + 1;
        // Q[j][g]: power at kLoHz + g * gStep (audio), interpolated from the zero-padded FFT
        std::vector<float> Q((size_t)nFrames * (size_t)G);
        std::vector<cf32> b((size_t)nfft);
        for (int j = 0; j < nFrames; j++) {
            std::fill(b.begin(), b.end(), cf32(0, 0));
            std::copy(x.begin() + (long)j * hop, x.begin() + (long)j * hop + sps, b.begin());
            fft.forward(b.data());
            for (int g = 0; g < G; g++) {
                const double fb = kLoHz + g * gStep - kCentreHz;
                double pos = fb / binHz;
                if (pos < 0) pos += nfft;
                const int k0 = (int)pos;
                const double fr = pos - k0;
                const float p0 = std::norm(b[(size_t)(k0 % nfft)]), p1 = std::norm(b[(size_t)((k0 + 1) % nfft)]);
                Q[(size_t)j * G + g] = (float)(p0 + fr * (p1 - p0));
            }
        }
        const int jMin = std::max(0, (int)std::floor((s0 + kDtMin[mode] * kFs) / hop));
        const int jMax = std::min(nFrames - 4 * 78 - 1, (int)std::ceil((s0 + kDtMax[mode] * kFs) / hop));
        if (jMax < jMin) return {};
        std::vector<float> best((size_t)G, 0.f);
        std::vector<int> bestJ((size_t)G, jMin);
        for (int g = 0; g + 14 < G; g++) {
            for (int j = jMin; j <= jMax; j++) {
                double a = 0, tot = 0;
                for (int c = 0; c < 3; c++) {
                    const uint8_t* cs = js8::costas(mode, c);
                    for (int k = 0; k < 7; k++) {
                        const float* q = &Q[(size_t)(j + 4 * (36 * c + k)) * G + g];
                        for (int t = 0; t < 8; t++) tot += q[2 * t];
                        a += q[2 * cs[k]];
                    }
                }
                const float m = (float)(a / std::max(1e-30, (tot - a) / 7));
                if (m > best[(size_t)g]) { best[(size_t)g] = m; bestJ[(size_t)g] = j; }
            }
        }
        std::vector<float> srt(best.begin(), best.end() - 14);
        std::nth_element(srt.begin(), srt.begin() + srt.size() / 2, srt.end());
        const float med = std::max(1e-6f, srt[srt.size() / 2]);
        std::vector<Cand> c;
        for (int g = 0; g + 14 < G; g++) {
            const float v = best[(size_t)g];
            if (v < 2.2f || v < 1.6f * med) continue;
            bool top = true;
            for (int h = std::max(0, g - 3); h <= std::min(G - 15, g + 3); h++) if (h != g && (best[(size_t)h] > v || (best[(size_t)h] == v && h < g))) top = false;
            if (!top) continue;
            c.push_back({kLoHz + g * gStep - kCentreHz, bestJ[(size_t)g] * hop, v});
        }
        std::sort(c.begin(), c.end(), [](const Cand& a, const Cand& b) { return a.m > b.m; });
        if (c.size() > 40) c.resize(40);
        return c;
    }

    struct Result { bool ok = false; uint8_t msg72[72]; int flags = 0; double fb = 0; int s = 0; float snr = 0; std::vector<std::array<cf32, 8>> X; std::vector<int> tones; };

    Result tryDecode(const std::vector<cf32>& x, const Cand& c) {
        Result r;
        std::vector<cf32> y;
        // frequency, then time
        double fb = c.fb; int s = c.s;
        float bm = -1;
        for (int k = -4; k <= 4; k++) {
            const double f = c.fb + k * baud / 16;
            mix(x, f, y);
            const float m = syncMetric(y, s);
            if (m > bm) { bm = m; fb = f; }
        }
        mix(x, fb, y);
        int bs = s;
        for (int k = -4; k <= 4; k++) {
            const int ss = s + k * sps / 32;
            const float m = syncMetric(y, ss);
            if (m > bm || k == -4) { if (m > bm) { bm = m; bs = ss; } }
        }
        s = bs;
        symbols(y, s, js8::kSymbols, nullptr, r.X);
        // soft decisions of the three bits of each data symbol, two ways: the difference of the best magnitudes (as WSJT-X), and the
        // non-coherent likelihood of the tones (log-sum of the powers over the noise), which reaches a couple of dB further down
        std::vector<float> pw(58 * 8);
        for (int j = 0; j < 58; j++) for (int t = 0; t < 8; t++) pw[(size_t)(8 * j + t)] = std::norm(r.X[(size_t)js8::dataSymbol(j)][(size_t)t]);
        std::vector<float> srt(pw);
        std::nth_element(srt.begin(), srt.begin() + srt.size() / 2, srt.end());
        const float noise = std::max(1e-30f, srt[srt.size() / 2] / 0.6931f);   // the median of an exponential is its mean x ln 2
        uint8_t cw[js8::kN];
        bool ok = false;
        for (int way = 0; way < 2 && !ok; way++) {
            float llr[js8::kN];
            double sum2 = 0;
            for (int j = 0; j < 58; j++) {
                const float* P = &pw[(size_t)(8 * j)];
                for (int b = 0; b < 3; b++) {
                    float v;
                    if (way == 0) {
                        float m1 = 0, m0 = 0;
                        for (int t = 0; t < 8; t++) { const float m = std::sqrt(P[t]); if ((t >> (2 - b)) & 1) m1 = std::max(m1, m); else m0 = std::max(m0, m); }
                        v = m1 - m0;
                    } else {
                        float hi = -1e30f;
                        for (int t = 0; t < 8; t++) hi = std::max(hi, P[t] / noise);
                        double s1 = 0, s0 = 0;
                        for (int t = 0; t < 8; t++) { const double e = std::exp(std::min(60.f, P[t] / noise - hi)); if ((t >> (2 - b)) & 1) s1 += e; else s0 += e; }
                        v = (float)std::log(std::max(1e-300, s1) / std::max(1e-300, s0));
                    }
                    llr[3 * j + b] = v;
                    sum2 += (double)v * v;
                }
            }
            if (way == 0) {
                const float sd = (float)std::sqrt(sum2 / js8::kN);
                if (sd <= 0) return r;
                for (float& v : llr) v = v / sd * 2.83f;
            } else {
                for (float& v : llr) v = std::max(-25.f, std::min(25.f, v));
            }
            ok = js8::ldpcDecode(llr, 40, cw) == 0 && js8::checkCrc(cw + js8::kM);
        }
        if (!ok) return r;
        const uint8_t* b87 = cw + js8::kM;
        r.ok = true;
        std::copy(b87, b87 + 72, r.msg72);
        r.flags = b87[72] * 4 + b87[73] * 2 + b87[74];
        r.tones = js8::tones(mode, b87);
        r.fb = fb; r.s = s;
        // SNR in 2500 Hz: the sent tone against the other seven
        double ps = 0, pn = 0;
        for (int i = 0; i < js8::kSymbols; i++)
            for (int t = 0; t < 8; t++) { const double p = std::norm(r.X[(size_t)i][(size_t)t]); if (t == r.tones[(size_t)i]) ps += p; else pn += p; }
        ps /= js8::kSymbols; pn /= 7.0 * js8::kSymbols;
        r.snr = (float)(10 * std::log10(std::max(1e-3, (ps - pn) / std::max(1e-30, pn))) - 10 * std::log10(2500 / baud));
        return r;
    }

    // take the decoded signal out of x
    void subtract(std::vector<cf32>& x, const Result& r) const {
        const std::complex<double> st = std::polar(1.0, 2 * kPi * r.fb / kFs);
        for (int i = 0; i < js8::kSymbols; i++) {
            const int a = r.s + i * sps;
            if (a < 0 || a + sps > (int)x.size()) continue;
            const int t = r.tones[(size_t)i];
            const cf32 amp = r.X[(size_t)i][(size_t)t] / (float)sps;
            std::complex<double> rot = std::polar(1.0, 2 * kPi * r.fb * a / kFs);
            for (int n = 0; n < sps; n++) {
                const cf32 v = amp * std::conj(tw[(size_t)t][(size_t)n]);
                x[(size_t)(a + n)] -= cf32((float)(v.real() * rot.real() - v.imag() * rot.imag()), (float)(v.real() * rot.imag() + v.imag() * rot.real()));
                rot *= st;
            }
        }
    }
};

} // namespace

struct Js8Receiver::Impl {
    std::mutex mu;                       // pub, settings
    std::function<void(const std::string&)> log;
    double rate = 0, offsetHz = 0;
    double startReq = -1;                // < 0 the system clock
    Js8Telemetry pub;
    std::atomic<bool> resetReq{true}, offsetReq{false}, flushReq{false};
    std::atomic<bool> enabled[3] = {true, true, true};
    // feed thread
    double curRate = 0;
    bool running = false;
    marine::Front front;
    hfdig::Listen listen;        // "Listen": the sideband on the sound card (off by default)
    std::vector<cf32> base;
    std::vector<float> lp;
    cf32 hist[2 * kTaps] = {};
    int histPos = 0, decPhase = 0;
    std::complex<double> rot{1, 0}, rotStep{1, 0};
    int rotN = 0;
    std::vector<cf32> buf;               // the 4 kHz stream
    int64_t bufStart = 0, n4 = 0;        // absolute index of buf[0], samples so far
    double t0 = 0;                       // slot clock of sample 0
    ClockComp clockComp;                 // automatic correction of a wrong computer clock (docs/EASE-3.8.md); live only (t0 from the system clock)
    double clockShift = 0;               // from the decoder thread, taken before the next slots are scheduled
    int64_t nextSlot[3] = {};
    int64_t nIn = 0;
    double nextReport = 0, power = 0;
    int64_t nPower = 0;
    std::vector<float> win, specAcc;
    int specN = 0;
    Fft specFft{kSpecN};
    int64_t nextSpec = 0;
    // decoder thread
    std::thread th;
    std::condition_variable cv;
    std::mutex qmu;
    std::deque<Job> q;
    bool busy = false, quit = false;
    // decoder thread state (under mu when published)
    struct Open { int mode; float hz; int64_t lastSlot; size_t msg; };
    std::vector<Open> open;

    Impl() {
        lp.resize(kTaps);
        const double fc = 1850.0 / kBase;
        double s = 0;
        for (int i = 0; i < kTaps; i++) {
            const double m = i - (kTaps - 1) / 2.0;
            const double sinc = m == 0 ? 2 * fc : std::sin(2 * kPi * fc * m) / (kPi * m);
            const double w = 0.42 - 0.5 * std::cos(2 * kPi * i / (kTaps - 1)) + 0.08 * std::cos(4 * kPi * i / (kTaps - 1));
            lp[(size_t)i] = (float)(sinc * w); s += sinc * w;
        }
        for (float& v : lp) v = (float)(v / s);
        rotStep = std::polar(1.0, -2 * kPi * kCentreHz / kBase);
        win.resize(kSpecN);
        for (int i = 0; i < kSpecN; i++) win[(size_t)i] = (float)(0.5 - 0.5 * std::cos(2 * kPi * i / kSpecN));
        th = dect2::namedThread("onair-js8", [this] { worker(); });
    }
    ~Impl() {
        { std::lock_guard<std::mutex> lk(qmu); quit = true; }
        cv.notify_all();
        th.join();
    }

    double streamUtc(int64_t i) const { return t0 + (double)i / kFs; }

    void resetState() {
        std::function<void(const std::string&)> cb;
        double off, st;
        { std::lock_guard<std::mutex> lk(mu); curRate = rate; off = offsetHz; cb = log; st = startReq; }
        running = curRate >= js8Tuning().minSampleRate - 1;
        if (running) { front.configure(curRate); front.setOffsetHz(off); }
        listen.reset();
        base.clear(); buf.clear(); bufStart = 0; n4 = 0; histPos = 0; decPhase = 0; rot = 1; rotN = 0;
        std::fill(std::begin(hist), std::end(hist), cf32(0, 0));
        nIn = 0; nextReport = 0; power = 0; nPower = 0;
        specAcc.assign(0, 0.f); specN = 0; nextSpec = kSpecN;
        t0 = st >= 0 ? st : nowUtc();
        for (int m = 0; m < 3; m++) nextSlot[m] = (int64_t)std::floor(t0 / js8::spec(m).period);
        {
            std::lock_guard<std::mutex> lk(mu);
            pub.inputRate = curRate;
            pub.timeMode = st >= 0 ? 1 : 0;
            pub.loHz = kLoHz; pub.hiHz = kHiHz;
        }
        if (cb && curRate > 0) cb(running ? "JS8: decoding normal, fast and turbo from dial + 200 Hz to dial + 3.2 kHz, slots from the system clock"
                                          : "JS8: the sample rate is too low (250 kHz at least)");
    }

    void spectrum() {
        std::vector<cf32> b((size_t)kSpecN);
        const int64_t a = n4 - kSpecN - bufStart;
        if (a < 0) return;
        for (int i = 0; i < kSpecN; i++) b[(size_t)i] = buf[(size_t)(a + i)] * win[(size_t)i];
        specFft.forward(b.data());
        const double bin = kFs / kSpecN;
        const int lo = (int)std::floor((kLoHz - kCentreHz) / bin), hi = (int)std::ceil((kHiHz - kCentreHz) / bin);
        if (specAcc.empty()) specAcc.assign((size_t)(hi - lo + 1), 0.f);
        for (int k = lo; k <= hi; k++) specAcc[(size_t)(k - lo)] += std::norm(b[(size_t)((k + kSpecN) % kSpecN)]) * (4.f / ((float)kSpecN * kSpecN));
        specN++;
    }

    void push4k(cf32 v) {
        buf.push_back(v);
        n4++;
        if (n4 >= nextSpec) { spectrum(); nextSpec += kSpecN / 2; }
    }

    void process(const cf32* x, size_t n) {
        for (size_t i = 0; i < n; i++) {
            const std::complex<double> m = std::complex<double>(x[i].real(), x[i].imag()) * rot;
            rot *= rotStep;
            if (++rotN >= 4096) { rot /= std::abs(rot); rotN = 0; }
            const cf32 v((float)m.real(), (float)m.imag());
            hist[histPos] = v; hist[histPos + kTaps] = v;
            if (++histPos >= kTaps) histPos = 0;
            if (++decPhase < kDec) continue;
            decPhase = 0;
            const cf32* h = &hist[histPos];
            cf32 y(0, 0);
            for (int j = 0; j < kTaps; j++) y += h[j] * lp[(size_t)j];
            push4k(y);
        }
        schedule(false);
        // keep about 40 s
        if (buf.size() > (size_t)(48 * kFs)) {
            const size_t drop = buf.size() - (size_t)(24 * kFs);
            buf.erase(buf.begin(), buf.begin() + (long)drop);
            bufStart += (int64_t)drop;
        }
    }

    // slots whose samples are all in: to the decoder
    void schedule(bool all) {
        { std::lock_guard<std::mutex> lk(mu); t0 -= clockShift; clockShift = 0; }
        const double now = streamUtc(n4);
        for (int m = 0; m < 3; m++) {
            const js8::Spec& sp = js8::spec(m);
            while (true) {
                const int64_t k = nextSlot[m];
                const double endUtc = (double)(k + 1) * sp.period + postSec(m);
                const double begUtc = (double)k * sp.period - preSec(m);
                if (now < endUtc && !(all && now > (double)k * sp.period + sp.startSec)) break;
                nextSlot[m]++;
                if (!enabled[m].load()) continue;
                int64_t a = (int64_t)std::llround((begUtc - t0) * kFs), b = std::min(n4, (int64_t)std::llround((endUtc - t0) * kFs));
                a = std::max(a, bufStart);
                if (b - a < (int64_t)(sp.durSec() * kFs * 0.5)) continue;
                Job j;
                j.mode = m; j.slot = k; j.startUtc = streamUtc(a);
                j.x.assign(buf.begin() + (long)(a - bufStart), buf.begin() + (long)(b - bufStart));
                {
                    std::lock_guard<std::mutex> lk(qmu);
                    if (q.size() >= 8) q.pop_front();   // fell behind: the oldest slot goes
                    q.push_back(std::move(j));
                }
                cv.notify_all();
                if (all && now < endUtc) break;
            }
        }
    }

    void report() {
        std::lock_guard<std::mutex> lk(mu);
        pub.seq++;
        pub.timeSec = curRate > 0 ? (double)nIn / curRate : 0;
        pub.levelDb = nPower ? (float)(10 * std::log10(power / (double)nPower + 1e-20)) : -200.f;
        power = 0; nPower = 0;
        pub.nowUtc = streamUtc(n4);
        for (int m = 0; m < 3; m++) pub.enabled[m] = enabled[m].load();
        if (specN > 0) {
            pub.spectrum.resize(specAcc.size());
            for (size_t i = 0; i < specAcc.size(); i++) { pub.spectrum[i] = (float)(10 * std::log10(specAcc[i] / specN + 1e-20)); specAcc[i] = 0; }
            specN = 0;
            pub.specSeq++;
        }
        const double last = pub.decodes.empty() ? -1e18 : pub.decodes.back().slotUtc;
        pub.dataValid = pub.nowUtc - last < 60;
        pub.state = pub.dataValid ? 2 : (pub.slots[0] + pub.slots[1] + pub.slots[2]) ? 1 : 0;
    }

    // ------------------------------------------------------------ decoder thread
    void worker() {
        std::unique_ptr<SlotDecoder> dec[3];
        while (true) {
            Job j;
            {
                std::unique_lock<std::mutex> lk(qmu);
                cv.wait(lk, [&] { return quit || !q.empty(); });
                if (quit) return;
                j = std::move(q.front()); q.pop_front();
                busy = true;
            }
            if (!dec[j.mode]) dec[j.mode] = std::make_unique<SlotDecoder>(j.mode);
            const auto t1 = std::chrono::steady_clock::now();
            std::vector<Js8Decode> out;
            uint64_t bad = 0;
            decodeSlot(*dec[j.mode], j, out, bad);
            const float ms = (float)std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t1).count();
            publish(j, out, bad, ms);
            {
                std::lock_guard<std::mutex> lk(qmu);
                busy = false;
            }
            cv.notify_all();
        }
    }

    void decodeSlot(SlotDecoder& d, Job& j, std::vector<Js8Decode>& out, uint64_t& bad) {
        const js8::Spec& sp = js8::spec(j.mode);
        const double slotUtc = (double)j.slot * sp.period;
        const int s0 = (int)std::lround((slotUtc + sp.startSec - j.startUtc) * kFs);
        std::vector<std::string> seen;
        for (int pass = 0; pass < 2; pass++) {
            const std::vector<Cand> cs = d.candidates(j.x, s0);
            int got = 0;
            for (const Cand& c : cs) {
                bool nearBy = false;   // a signal already decoded here
                for (const auto& o : out) if (std::fabs(o.hz - (c.fb + kCentreHz)) < 2 * d.baud && std::fabs(o.dt - (c.s - s0) / kFs) < 0.5) nearBy = true;
                if (nearBy) continue;
                SlotDecoder::Result r = d.tryDecode(j.x, c);
                if (!r.ok) { bad++; continue; }
                const std::string chars = js8::bitsToChars(r.msg72) + char('0' + r.flags);
                if (std::find(seen.begin(), seen.end(), chars) != seen.end()) { d.subtract(j.x, r); continue; }
                seen.push_back(chars);
                const js8::Frame f = js8::unpackFrame(r.msg72, r.flags);
                Js8Decode o;
                o.slotUtc = slotUtc; o.submode = j.mode;
                o.hz = (float)(r.fb + kCentreHz); o.snrDb = r.snr; o.dt = (float)((r.s - s0) / kFs);
                o.chars = js8::bitsToChars(r.msg72); o.flags = r.flags; o.type = f.type;
                o.text = f.display(); o.from = f.from; o.to = f.to; o.grid = f.grid; o.pass = pass;
                out.push_back(o);
                frames_.push_back(f);
                d.subtract(j.x, r);
                got++;
            }
            if (got == 0) break;
        }
    }
    std::vector<js8::Frame> frames_;   // the frames of the slot being published (decoder thread only)

    void station(const Js8Decode& d, const js8::Frame& f) {
        if (f.from.empty() || f.from[0] == '@' || f.from[0] == '<') return;
        Js8Station* st = nullptr;
        for (auto& s : pub.stations) if (s.call == f.from) st = &s;
        if (!st) { pub.stations.push_back(Js8Station()); st = &pub.stations.back(); st->call = f.from; }
        st->snrDb = d.snrDb; st->hz = d.hz; st->lastUtc = d.slotUtc; st->submode = d.submode; st->frames++;
        st->lastText = d.text;
        if (f.type == js8::kHeartbeat) { if (f.cq) st->cq = true; else st->heartbeat = true; }
        if (!f.grid.empty()) {
            st->grid = f.grid;
            double la, lo;
            if (js8::gridToLatLon(f.grid, la, lo)) { st->lat = la; st->lon = lo; st->hasPos = true; }
        }
    }

    // the frames of a slot into messages: a message is the frames of one transmission at one frequency, first to last
    void assemble(const Js8Decode& d, const js8::Frame& f, int64_t slot) {
        const double tol = 2 * js8::spec(d.submode).baud();
        Open* op = nullptr;
        for (auto& o : open) if (o.mode == d.submode && std::fabs(o.hz - d.hz) < tol && o.lastSlot == slot - 1) op = &o;
        const bool first = (d.flags & js8::kFirst) || f.type == js8::kHeartbeat;
        if (first || !op) {
            open.erase(std::remove_if(open.begin(), open.end(), [&](const Open& o) { return o.mode == d.submode && std::fabs(o.hz - d.hz) < tol; }), open.end());
            Js8Message m;
            m.utc = d.slotUtc; m.submode = d.submode; m.hz = d.hz; m.snrDb = d.snrDb;
            m.from = f.from; m.to = f.to; m.cmd = f.cmd;
            m.text = first ? d.text : "... " + d.text;
            m.relay = f.type == js8::kDirected && f.cmdCode == 5;
            m.frames = 1;
            pub.messages.push_back(m);
            if (pub.messages.size() > kJs8MsgKeep) {
                pub.messages.erase(pub.messages.begin());
                for (auto& o : open) if (o.msg > 0) o.msg--;
            }
            open.push_back({d.submode, d.hz, slot, pub.messages.size() - 1});
            op = &open.back();
        } else {
            Js8Message& m = pub.messages[op->msg];
            m.text += d.text;
            m.frames++;
            op->lastSlot = slot;
            op->hz = d.hz;
        }
        Js8Message& m = pub.messages[op->msg];
        if ((d.flags & js8::kLast) || f.type == js8::kHeartbeat) {
            m.complete = true;
            if (m.relay) {   // FROM: VIA>DEST TEXT
                const size_t gt = m.text.find('>');
                if (gt != std::string::npos) {
                    size_t e = m.text.find(' ', gt + 1);
                    m.relayTo = m.text.substr(gt + 1, e == std::string::npos ? std::string::npos : e - gt - 1);
                }
            }
            if (m.cmd == " GRID") {   // a grid sent as text
                for (auto& s : pub.stations) if (s.call == m.from) {
                    const size_t sp = m.text.rfind("GRID");
                    if (sp == std::string::npos) break;
                    std::string g = m.text.substr(sp + 4);
                    while (!g.empty() && g[0] == ' ') g.erase(g.begin());
                    if (g.size() >= 4) { double la, lo; if (js8::gridToLatLon(g.substr(0, g.size() >= 6 ? 6 : 4), la, lo)) { s.grid = g.substr(0, 6); s.lat = la; s.lon = lo; s.hasPos = true; } }
                }
            }
            open.erase(std::remove_if(open.begin(), open.end(), [&](const Open& o) { return &o == op; }), open.end());
        }
    }

    void publish(const Job& j, std::vector<Js8Decode>& out, uint64_t bad, float ms) {
        std::vector<js8::Frame> fr;
        fr.swap(frames_);
        std::lock_guard<std::mutex> lk(mu);
        // a message that did not go on in this slot is over
        open.erase(std::remove_if(open.begin(), open.end(), [&](const Open& o) { return o.mode == j.mode && o.lastSlot < j.slot - 1; }), open.end());
        std::vector<size_t> order(out.size());
        for (size_t i = 0; i < out.size(); i++) order[i] = i;
        std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return out[a].hz < out[b].hz; });
        for (size_t i : order) {
            pub.decodes.push_back(out[i]);
            if (!out[i].from.empty() && PskReporter::get().want()) PskReporter::get().spot("JS8", out[i].hz, out[i].snrDb, out[i].from, out[i].grid);
            station(out[i], fr[i]);
            assemble(out[i], fr[i], j.slot);
        }
        if (pub.decodes.size() > kJs8Keep) pub.decodes.erase(pub.decodes.begin(), pub.decodes.begin() + (long)(pub.decodes.size() - kJs8Keep));
        std::sort(pub.stations.begin(), pub.stations.end(), [](const Js8Station& a, const Js8Station& b) { return a.lastUtc > b.lastUtc; });
        if (!out.empty()) pub.snrDb = out.back().snrDb;
        pub.blocksOk += out.size(); pub.blocksBad += bad;
        pub.slots[j.mode]++; pub.lastSlotCount[j.mode] = (int)out.size(); pub.decodeMs[j.mode] = ms;
        // the clock: median DT of the recent decodes
        std::vector<float> dts;
        for (size_t i = pub.decodes.size() > 40 ? pub.decodes.size() - 40 : 0; i < pub.decodes.size(); i++) dts.push_back(pub.decodes[i].dt);
        if (!dts.empty()) { std::nth_element(dts.begin(), dts.begin() + dts.size() / 2, dts.end()); pub.clockErr = dts[dts.size() / 2]; }
        if (startReq < 0) {   // the computer clock, from the DT of this slot
            std::vector<float> slotDts;
            for (const auto& d : out) slotDts.push_back(d.dt);
            float med = 0;
            if (!slotDts.empty()) { std::nth_element(slotDts.begin(), slotDts.begin() + slotDts.size() / 2, slotDts.end()); med = slotDts[slotDts.size() / 2]; }
            const double s = clockComp.slotEnded((int)out.size(), med);
            if (s != 0) { clockShift += s; pub.clockTrim = (float)clockComp.total(); }
        }
        pub.seq++;
    }
};

Js8Receiver::Js8Receiver() : p_(std::make_unique<Impl>()) {}
Js8Receiver::~Js8Receiver() = default;

void Js8Receiver::configure(double r) { std::lock_guard<std::mutex> lk(p_->mu); p_->rate = r; p_->resetReq = true; }
void Js8Receiver::setSignalOffset(double hz) { std::lock_guard<std::mutex> lk(p_->mu); p_->offsetHz = hz; p_->offsetReq = true; }
bool Js8Receiver::ready() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->rate >= js8Tuning().minSampleRate - 1; }
void Js8Receiver::reset() { p_->resetReq = true; }

void Js8Receiver::feed(const cf32* x, size_t n) {
    Impl& m = *p_;
    if (m.resetReq.exchange(false)) { m.offsetReq = false; m.resetState(); }
    if (m.offsetReq.exchange(false) && m.running) { std::lock_guard<std::mutex> lk(m.mu); m.front.setOffsetHz(m.offsetHz); }
    if (m.curRate <= 0) return;
    for (size_t i = 0; i < n; i++) {
        float re = x[i].real(), im = x[i].imag();
        if (!std::isfinite(re) || !std::isfinite(im)) re = im = 0;
        m.power += (double)re * re + (double)im * im;
    }
    m.nPower += (int64_t)n;
    m.nIn += (int64_t)n;
    if (m.running) {
        m.base.clear();
        m.front.process(x, n, m.base);
        for (cf32& v : m.base) if (!std::isfinite(v.real()) || !std::isfinite(v.imag())) v = cf32(0, 0);
        m.listen.playBase(m.base.data(), m.base.size());   // a copy to the sound card only while listening
        m.process(m.base.data(), m.base.size());
        if (m.flushReq.exchange(false)) m.schedule(true);
    }
    while ((double)m.nIn >= m.nextReport) {
        m.nextReport += 0.25 * m.curRate;
        m.report();
    }
}

bool Js8Receiver::telemetry(Js8Telemetry& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (p_->pub.seq <= lastSeq) return false;
    out = p_->pub;
    return true;
}
void Js8Receiver::setListen(bool on) { p_->listen.on = on; }
bool Js8Receiver::listening() const { return p_->listen.on.load(); }
void Js8Receiver::setVolume(float v) { p_->listen.volume = std::max(0.f, std::min(1.f, v)); }
void Js8Receiver::setMuted(bool m) { p_->listen.muted = m; }
void Js8Receiver::setSilent(bool s) { p_->listen.silent = s; }
void Js8Receiver::setListenTap(std::function<void(const float* x, size_t n)> cb) { p_->listen.setTap(std::move(cb)); }
void Js8Receiver::setLogCallback(std::function<void(const std::string&)> cb) { std::lock_guard<std::mutex> lk(p_->mu); p_->log = std::move(cb); }
void Js8Receiver::setEnabled(int m, bool on) { if (m >= 0 && m < 3) p_->enabled[m] = on; }
void Js8Receiver::setStartTime(double utc) { std::lock_guard<std::mutex> lk(p_->mu); p_->startReq = utc; p_->resetReq = true; }
void Js8Receiver::clearDecodes() {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->pub.decodes.clear(); p_->pub.messages.clear(); p_->pub.stations.clear(); p_->open.clear();
    p_->pub.seq++;
}
// the decodes, messages and stations already stay through reset() (their times are the slot clock): only Clear empties them
void Js8Receiver::clearResults() { clearDecodes(); }
std::string Js8Receiver::saveResults() const {
    std::lock_guard<std::mutex> lk(p_->mu);
    return results::pack(p_->pub.decodes, p_->pub.messages, p_->pub.stations);
}
bool Js8Receiver::loadResults(const std::string& blob) {
    Js8Telemetry l;
    if (!results::unpack(blob, l.decodes, l.messages, l.stations)) return false;
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->pub.decodes = std::move(l.decodes); p_->pub.messages = std::move(l.messages); p_->pub.stations = std::move(l.stations);
    p_->open.clear();
    p_->pub.seq++;
    return true;
}
void Js8Receiver::waitIdle() {
    std::unique_lock<std::mutex> lk(p_->qmu);
    p_->cv.wait(lk, [&] { return p_->q.empty() && !p_->busy; });
}
void Js8Receiver::flush() {
    Impl& m = *p_;
    if (m.running) m.schedule(true);
}

ModeTuning js8Tuning() {
    ModeTuning t;
    t.stdMode = 39; t.id = "js8"; t.name = "JS8Call";
    t.minMhz = 1.8; t.maxMhz = 30; t.defMhz = 14.078;   // the 20 m JS8Call dial frequency (upper sideband)
    t.sampleRate = 2000000;
    t.basebandHz = 1750000;
    t.bandwidthMhz = 0.0035;
    t.minSampleRate = 250000;
    t.tuneOffsetHz = 20000;                              // as HF digital: the radio sits 20 kHz above the dial, away from its DC spike
    return t;
}

const Js8Preset* js8Presets(int& count) {
    static const Js8Preset kP[] = {{"80 m", 3.578}, {"40 m", 7.078}, {"30 m", 10.130}, {"20 m", 14.078}, {"17 m", 18.104}, {"15 m", 21.078}, {"10 m", 28.078}};
    count = (int)(sizeof kP / sizeof *kP);
    return kP;
}

} // namespace dect2
