// Airband receiver (see airband_rx.h): every listed channel of the sample band mixed down, filtered, AM-demodulated and squelched on its
// own; the tuning error measured from all of them together; the open channels mixed (or scanned) to the sound card.
#include "dect2/airband_rx.h"
#include "dect2/airband_tel.h"
#include "dect2/airband_gen.h"
#include "dect2/audioout.h"
#include "dect2/fftutil.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <deque>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace dect2 {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kDefaultDialHz = 118.700e6;
constexpr int kCicOrder = 5;
constexpr int kBlock = 256;               // carrier search FFT at the channel rate
constexpr int kHop = kBlock / 2;
constexpr int kWide = 1024;               // the tuning error spectrum at the CIC rate
constexpr double kDelaySec = 0.06;        // the audio runs this late, so the squelch has decided before a sample plays
constexpr double kMaxTuneErr = 7800;      // the tuning error searched
constexpr double kSingleLimit = 4100;     // what one channel alone may move it by
constexpr double kScanResume = 2.0;       // scan: seconds after the channel closed before it moves on

struct Spacing { double pass, stop, window, tol, noiseBw; };
const Spacing k833{3400, 4900, 1000, 700, 6800};
const Spacing k25k{5000, 8000, 2500, 2500, 10000};

// RBJ biquad (direct form II transposed)
struct Biquad {
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
    void set(bool high, double f, double q, double fs) {
        const double w = 2 * kPi * f / fs, c = std::cos(w), al = std::sin(w) / (2 * q), a0 = 1 + al;
        if (high) { b0 = (1 + c) / 2; b1 = -(1 + c); b2 = (1 + c) / 2; }
        else { b0 = (1 - c) / 2; b1 = 1 - c; b2 = (1 - c) / 2; }
        b0 /= a0; b1 /= a0; b2 /= a0; a1 = -2 * c / a0; a2 = (1 - al) / a0;
        z1 = z2 = 0;
    }
    double run(double x) { const double y = b0 * x + z1; z1 = b1 * x - a1 * y + z2; z2 = b2 * x - a2 * y; return y; }
};

std::vector<float> lowpassTaps(double cut, double trans, double fs) {
    int n = (int)std::ceil(5.5 * fs / trans) | 1;
    n = std::max(31, std::min(n, 1201));
    std::vector<float> h((size_t)n);
    const int m = n / 2;
    double sum = 0;
    for (int i = 0; i < n; i++) {
        const double t = i - m, x = 2 * cut / fs * t;
        const double s = t == 0 ? 2 * cut / fs : std::sin(kPi * x) / (kPi * t);
        const double w = 0.42 - 0.5 * std::cos(2 * kPi * i / (n - 1)) + 0.08 * std::cos(4 * kPi * i / (n - 1));
        h[(size_t)i] = (float)(s * w);
        sum += s * w;
    }
    for (auto& v : h) v = (float)(v / sum);
    return h;
}

// 8 kHz mono -> 48 kHz stereo (polyphase by 6, pass band up to 3.6 kHz)
class Up6 {
public:
    static constexpr int kUp = 6, kPer = 24;
    Up6() {
        const auto lp = lowpassTaps(3800, 800, kUp * kAirbandAudioRate);
        std::vector<float> h((size_t)(kUp * kPer), 0.f);
        const size_t off = (h.size() - std::min(h.size(), lp.size())) / 2;
        for (size_t i = 0; i < lp.size() && i < h.size(); i++) h[i + off] = lp[i + (lp.size() > h.size() ? (lp.size() - h.size()) / 2 : 0)];
        for (int p = 0; p < kUp; p++) for (int k = 0; k < kPer; k++) taps_[p][k] = kUp * h[(size_t)(p + kUp * k)];
    }
    void reset() { for (auto& v : hist_) v = 0; pos_ = 0; }
    void process(const float* x, size_t n, std::vector<float>& st) {
        for (size_t i = 0; i < n; i++) {
            hist_[pos_] = x[i]; hist_[pos_ + kPer] = x[i];
            if (++pos_ >= kPer) pos_ = 0;
            const float* w = &hist_[pos_];
            for (int p = 0; p < kUp; p++) {
                float s = 0;
                for (int k = 0; k < kPer; k++) s += taps_[p][k] * w[kPer - 1 - k];
                st.push_back(s); st.push_back(s);
            }
        }
    }
private:
    float taps_[kUp][kPer] = {};
    float hist_[2 * kPer] = {};
    int pos_ = 0;
};

struct Interval { double t0, t1; };   // gate open in channel samples [t0, t1]

struct Chan {
    AirbandChannel cfg;
    const Spacing* sp = &k25k;
    double posHz = 0;
    bool inBand = false;
    // full rate: mixer and CIC
    double oRe = 1, oIm = 0, sRe = 1, sIm = 0;
    int renorm = 0;
    int64_t integ[kCicOrder][2] = {}, comb[kCicOrder][2] = {};
    int cicPhase = 0;
    // CIC rate: tuning error mixer, channel FIR
    double fPh = 0, fStep = 0;
    const std::vector<float>* taps = nullptr;
    std::vector<cf32> hist;
    int hpos = 0, dPhase = 0;
    std::vector<cf32> wbuf;
    std::vector<float> hold;
    // channel rate
    std::vector<cf32> ring;               // the last kBlock samples
    int64_t nCh = 0;
    double pllPh = 0, pllF = 0;
    double lockI = 0, lockQ = 0, carSign = 1;
    Biquad hp, lp1, lp2;
    double prevAud = 0;
    std::deque<float> aud;                // 8 kHz audio not played yet
    int64_t audBase = 0;
    double powAcc = 0; int64_t powN = 0;
    // squelch
    double floor = -1;
    bool prevDet = false; int prevK = 0, carK = 0;
    bool active = false;
    double tOpen = 0, lastDet = 0;
    double amp = 0;                       // carrier amplitude while open
    std::deque<Interval> done;
    float snrDb = -99, levelDb = -200, peakSnr = 0;
    double levAcc = 0; int levN = 0;
    bool het = false;
    float gate = 0;
    uint64_t txCount = 0;
    double lastEndSec = -1;
    int64_t wallStart = 0;
};

} // namespace

// ---------------------------------------------------------------- channel names

bool airbandOnRaster(double f, bool is833) {
    const double step = is833 ? kAirband833 : 25000.0;
    const double r = f / step;
    return std::fabs(r - std::round(r)) * step < 10;
}

std::string airbandName(double f, bool is833) {
    long khz;
    if (!is833) khz = std::lround(f / 25000.0) * 25;
    else {
        const long k = std::lround(f / kAirband833);
        khz = (k / 3) * 25 + 5 * (k % 3 + 1);
    }
    char b[24];
    snprintf(b, sizeof b, "%ld.%03ld", khz / 1000, khz % 1000);
    return b;
}

bool airbandParse(const std::string& text, double& freqHz, bool& is833) {
    std::string s;
    for (char c : text) if (c != ' ' && c != '\t') s += c;
    if (s.empty()) return false;
    char* end = nullptr;
    const double v = std::strtod(s.c_str(), &end);
    if (!end || *end || !std::isfinite(v)) return false;
    const double mhz = v > 1e6 ? v / 1e6 : v;
    const double khz = mhz * 1000;
    double f;
    bool n833;
    if (std::fabs(khz - std::round(khz)) < 1e-4) {   // a name: whole kHz
        const long n = std::lround(khz), r = n % 25;
        if (r == 0) { f = n * 1000.0; n833 = false; }
        else if (r == 5 || r == 10 || r == 15) { f = (n - r) * 1000.0 + (r / 5 - 1) * kAirband833; n833 = true; }
        else return false;
    } else {
        f = mhz * 1e6;
        const double k = std::round(f / kAirband833);
        if (std::fabs(f - k * kAirband833) > 50) return false;
        f = k * kAirband833; n833 = true;
    }
    if (f < 118e6 - 1 || f > 136.991667e6 + 1 || (!n833 && f > 136.975e6 + 1)) return false;
    freqHz = f; is833 = n833;
    return true;
}

// ---------------------------------------------------------------- the receiver

struct AirbandReceiver::Impl {
    std::mutex mu;
    std::function<void(const std::string&)> log;
    double rate = 0, offsetHz = 0, centerSet = 0;
    std::vector<AirbandChannel> chanSet;
    float sqlSet = 6, hangSet = 0.5f;
    bool scanSet = false;
    double tuneGuess = 0; bool tuneGuessReq = false;
    AirbandTelemetry pub;
    std::atomic<bool> resetReq{true}, chanReq{false}, tapsReq{false};
    std::atomic<float> volume{1.f};
    std::atomic<bool> muted{false}, silent{false};
    std::function<void(int, const float*, size_t)> chanTapSet, chanTap;
    std::function<void(const float*, size_t)> mixTapSet, mixTap;
    // receiver thread
    AirbandTelemetry tel;
    double curRate = 0, center = kDefaultDialHz, fi = 0, fc = 0;
    int R = 1, D2 = 1, cicBits = 20;
    double cicNorm = 1;
    std::vector<Chan> ch;
    std::vector<float> taps833, taps25;
    Fft fftN{kBlock}, fftW{kWide};
    std::vector<float> win, winW;
    double winPow = 1, winPowW = 1;
    double tuneErr = 0; bool tuneKnown = false, tuneEst = false;
    int64_t nIn = 0, nOut8 = 0;
    double nextReport = 0, nextEst = 0;
    double power = 0; int64_t nPower = 0;
    float sql = 6, hang = 0.5f;
    bool scan = false;
    int scanCh = -1; double scanIdleSince = -1;
    std::vector<cf32> clean, wide, nb;
    std::vector<float> mix, pcm;
    Up6 up;
    std::unique_ptr<AudioOut> audioOut;
    std::deque<AirbandActivity> act;

    double sigSec() const { return curRate > 0 ? (double)nIn / curRate : 0; }

    void design() {
        R = std::max(1, (int)std::floor(curRate / 48000.0));
        fi = curRate / R;
        D2 = std::max(1, (int)std::floor(fi / 16000.0));
        fc = fi / D2;
        const double growth = kCicOrder * std::log2((double)R);
        cicBits = std::max(8, std::min(24, (int)std::floor(61 - growth)));
        cicNorm = 1.0 / (std::pow((double)R, kCicOrder) * std::ldexp(1.0, cicBits - 2));
        taps833 = lowpassTaps((k833.pass + k833.stop) / 2, k833.stop - k833.pass, fi);
        taps25 = lowpassTaps((k25k.pass + k25k.stop) / 2, k25k.stop - k25k.pass, fi);
        win.resize(kBlock); winW.resize(kWide);
        winPow = winPowW = 0;
        for (int i = 0; i < kBlock; i++) { win[(size_t)i] = (float)(0.5 - 0.5 * std::cos(2 * kPi * i / kBlock)); winPow += win[(size_t)i] * win[(size_t)i]; }
        for (int i = 0; i < kWide; i++) { winW[(size_t)i] = (float)(0.5 - 0.5 * std::cos(2 * kPi * i / kWide)); winPowW += winW[(size_t)i] * winW[(size_t)i]; }
    }

    void setFine(Chan& c) { c.fStep = -2 * kPi * tuneErr / fi; }

    void buildChannels(const std::vector<AirbandChannel>& list) {
        ch.clear();
        ch.resize(list.size());
        const double half = 0.45 * curRate;
        for (size_t i = 0; i < list.size(); i++) {
            Chan& c = ch[i];
            c.cfg = list[i];
            c.sp = c.cfg.is833 ? &k833 : &k25k;
            c.posHz = c.cfg.freqHz - center;
            c.inBand = curRate > 0 && std::fabs(c.posHz) + 12500 + kMaxTuneErr < half;
            const double w = -2 * kPi * c.posHz / curRate;
            c.sRe = std::cos(w); c.sIm = std::sin(w);
            c.taps = c.cfg.is833 ? &taps833 : &taps25;
            c.hist.assign(2 * c.taps->size(), cf32(0.f, 0.f));
            c.wbuf.clear(); c.wbuf.reserve(kWide);
            c.hold.assign(kWide, 0.f);
            c.ring.assign(kBlock, cf32(0.f, 0.f));
            c.hp.set(true, 300, 0.7071, fc);
            c.lp1.set(false, 3000, 0.5412, fc);
            c.lp2.set(false, 3000, 1.3066, fc);
            c.audBase = nOut8;
            setFine(c);
        }
    }

    void resetState() {
        std::function<void(const std::string&)> cb;
        std::vector<AirbandChannel> list;
        { std::lock_guard<std::mutex> lk(mu); curRate = rate; cb = log; center = centerSet > 0 ? centerSet : kDefaultDialHz; list = chanSet;
          sql = sqlSet; hang = hangSet; scan = scanSet; }
        const uint64_t s = tel.seq;
        tel = AirbandTelemetry();
        tel.seq = s;
        tel.inputRate = curRate;
        nIn = 0; nOut8 = 0; nextReport = 0; nextEst = 0.5; power = 0; nPower = 0;
        scanCh = -1; scanIdleSince = -1;
        act.clear();
        up.reset();
        if (audioOut) audioOut->flush();
        if (curRate > 0) design();
        buildChannels(list);
        int inb = 0;
        for (const auto& c : ch) inb += c.inBand;
        if (cb && curRate > 0) {
            char b[160];
            snprintf(b, sizeof b, "Airband: %d of %zu channels inside the band around %.3f MHz, demodulated at %.0f Hz", inb, ch.size(), center / 1e6, fc);
            cb(b);
        }
    }

    void applyFlags(const std::vector<AirbandChannel>& list) {
        bool same = list.size() == ch.size();
        for (size_t i = 0; same && i < list.size(); i++) same = list[i].freqHz == ch[i].cfg.freqHz && list[i].is833 == ch[i].cfg.is833;
        if (!same) { buildChannels(list); return; }
        for (size_t i = 0; i < list.size(); i++) ch[i].cfg = list[i];
    }

    // ------------------------------------------------ one channel, one input block

    void runChannel(Chan& c, int idx, const cf32* x, size_t n) {
        wide.clear();
        const double sc = std::ldexp(1.0, cicBits - 2);
        for (size_t i = 0; i < n; i++) {
            const double re = x[i].real() * c.oRe - x[i].imag() * c.oIm, im = x[i].real() * c.oIm + x[i].imag() * c.oRe;
            const double nr = c.oRe * c.sRe - c.oIm * c.sIm; c.oIm = c.oRe * c.sIm + c.oIm * c.sRe; c.oRe = nr;
            if (++c.renorm >= 1024) { const double m = 1.0 / std::sqrt(c.oRe * c.oRe + c.oIm * c.oIm); c.oRe *= m; c.oIm *= m; c.renorm = 0; }
            int64_t v[2] = {(int64_t)std::llround(re * sc), (int64_t)std::llround(im * sc)};
            for (int k = 0; k < 2; k++) {
                uint64_t a = (uint64_t)v[k];
                for (int s = 0; s < kCicOrder; s++) { c.integ[s][k] = (int64_t)((uint64_t)c.integ[s][k] + a); a = (uint64_t)c.integ[s][k]; }
                v[k] = (int64_t)a;
            }
            if (++c.cicPhase < R) continue;
            c.cicPhase = 0;
            double o[2];
            for (int k = 0; k < 2; k++) {
                uint64_t a = (uint64_t)v[k];
                for (int s = 0; s < kCicOrder; s++) { const uint64_t d = a - (uint64_t)c.comb[s][k]; c.comb[s][k] = (int64_t)a; a = d; }
                o[k] = (double)(int64_t)a * cicNorm;
            }
            wide.push_back(cf32((float)o[0], (float)o[1]));
        }
        // the tuning error spectrum
        for (const cf32& w : wide) {
            c.wbuf.push_back(w);
            if ((int)c.wbuf.size() == kWide) {
                for (int i = 0; i < kWide; i++) c.wbuf[(size_t)i] *= winW[(size_t)i];
                fftW.forward(c.wbuf.data());
                const float decay = (float)std::pow(0.5, kWide / (10.0 * fi));   // an average over about 15 s: a carrier stays a line, voice is a smooth hump
                for (int k = 0; k < kWide; k++) c.hold[(size_t)k] = c.hold[(size_t)k] * decay + (1 - decay) * (float)(std::norm(c.wbuf[(size_t)k]) / winPowW);
                c.wbuf.clear();
            }
        }
        // tuning error mixer, channel filter, decimation
        nb.clear();
        const int L = (int)c.taps->size();
        const float* h = c.taps->data();
        for (const cf32& w0 : wide) {
            const cf32 w = w0 * cf32((float)std::cos(c.fPh), (float)std::sin(c.fPh));
            c.fPh = std::remainder(c.fPh + c.fStep, 2 * kPi);
            c.hist[(size_t)c.hpos] = w; c.hist[(size_t)(c.hpos + L)] = w;
            if (++c.hpos >= L) c.hpos = 0;
            if (++c.dPhase < D2) continue;
            c.dPhase = 0;
            const cf32* hw = &c.hist[(size_t)c.hpos];
            float ar = 0, ai = 0;
            for (int k = 0; k < L; k++) { ar += h[k] * hw[k].real(); ai += h[k] * hw[k].imag(); }
            nb.push_back(cf32(ar, ai));
        }
        for (const cf32& z : nb) channelSample(c, idx, z);
    }

    void channelSample(Chan& c, int idx, cf32 z) {
        c.ring[(size_t)(c.nCh % kBlock)] = z;
        c.nCh++;
        c.powAcc += std::norm(z); c.powN++;
        // synchronous AM: Costas-type loop (insensitive to the half-turn of an overmodulated carrier), envelope where it does not hold
        const cf32 r = z * cf32((float)std::cos(-c.pllPh), (float)std::sin(-c.pllPh));
        const double I = r.real(), Q = r.imag();
        const double e = std::fabs(I) > 1e-12 ? std::atan(Q / I) : 0;
        const double wn = 2 * kPi * 25 / fc;
        c.pllF += wn * wn * e;
        c.pllF = std::max(-2 * kPi * 3000 / fc, std::min(2 * kPi * 3000 / fc, c.pllF));
        c.pllPh = std::remainder(c.pllPh + c.pllF + 1.41 * wn * e, 2 * kPi);
        const double a = 1.0 / (0.02 * fc);
        c.lockI += a * (I * I - c.lockI); c.lockQ += a * (Q * Q - c.lockQ);
        c.carSign += a * ((I >= 0 ? 1.0 : -1.0) - c.carSign);
        const double lq = (c.lockI - c.lockQ) / (c.lockI + c.lockQ + 1e-30);
        const double wgt = std::max(0.0, std::min(1.0, (lq - 0.3) / 0.4));
        const double demod = wgt * I * (c.carSign >= 0 ? 1 : -1) + (1 - wgt) * std::abs(z);
        double au = c.hp.run(demod);
        au = c.lp2.run(c.lp1.run(au));
        // to 8 kHz: output j sits at channel sample j * fc / 8000
        const int64_t k = c.nCh - 1;
        while (true) {
            const int64_t j = c.audBase + (int64_t)c.aud.size();
            const double t = (double)j * fc / kAirbandAudioRate;
            if (t > (double)k) break;
            const double fr = t - (double)(k - 1);
            c.aud.push_back((float)(c.prevAud + (au - c.prevAud) * std::max(0.0, std::min(1.0, fr))));
        }
        c.prevAud = au;
        if (c.nCh >= kBlock && c.nCh % kHop == 0) block(c, idx);
    }

    // ------------------------------------------------ squelch, once per hop

    void block(Chan& c, int idx) {
        std::vector<cf32> b(kBlock);
        for (int i = 0; i < kBlock; i++) b[(size_t)i] = c.ring[(size_t)((c.nCh + i) % kBlock)] * win[(size_t)i];
        fftN.forward(b.data());
        const double bin = fc / kBlock;
        auto P = [&](int k) { return std::norm(b[(size_t)(((k % kBlock) + kBlock) % kBlock)]) / winPow; };
        const int kp = (int)(c.sp->pass / bin), kw = (int)(c.sp->window / bin);
        std::vector<double> pb;
        for (int k = -kp; k <= kp; k++) pb.push_back(P(k));
        std::nth_element(pb.begin(), pb.begin() + (long)pb.size() / 2, pb.end());
        const double med = pb[pb.size() / 2] / 0.693;
        if (c.floor < 0) c.floor = med;
        else if (med < c.floor) c.floor += 0.3 * (med - c.floor);
        else if (!c.active) c.floor += 0.004 * (med - c.floor);
        const double nb0 = std::max(c.floor, 1e-30);
        int best = 0; double bp = -1;
        for (int k = -kw; k <= kw; k++) if (P(k) > bp) { bp = P(k); best = k; }
        auto csnr = [&](int k, double& a2) {
            const double s3 = P(k - 1) + P(k) + P(k + 1);
            a2 = std::max(0.0, (s3 - 3 * nb0) / kBlock);
            return 10 * std::log10(a2 / (nb0 * c.sp->noiseBw / fc) + 1e-12);
        };
        double a2 = 0;
        const double snr = csnr(best, a2);
        // a second carrier: two stations at once
        bool het = false;
        for (int k = -kw; k <= kw; k++) {
            if (std::abs(k - best) < 4 || P(k) < P(k - 1) || P(k) < P(k + 1)) continue;
            double a22;
            const double s2 = csnr(k, a22);
            if (s2 > sql && s2 > snr - 20) { het = true; break; }
        }
        c.snrDb = (float)snr;
        const double thr = c.active ? sql - 3 : sql;
        // and a line, not a raised floor: a neighbour's splatter or noise lifts every bin, a carrier stands out of the bins beside it
        const double prom = (P(best - 1) + P(best) + P(best + 1)) / (3 * std::max(med, 1e-30));
        const bool det = snr >= thr && prom >= 8 && (!c.active || std::abs(best - c.carK) <= 2);   // an open channel holds on to its own carrier
        const double s0 = (double)(c.nCh - kBlock);   // this block's first sample
        const double hangS = hang * fc;
        if (det) {
            const bool confirm = c.active || (c.prevDet && std::abs(best - c.prevK) <= 2);
            if (confirm) {
                if (!c.active) {
                    c.active = true;
                    c.tOpen = s0 - kHop + kBlock / 4.0;
                    c.amp = std::sqrt(a2);
                    c.peakSnr = (float)snr; c.levAcc = 0; c.levN = 0; c.het = false;
                    c.wallStart = (int64_t)std::time(nullptr);
                } else c.amp += 0.3 * (std::sqrt(a2) - c.amp);
                c.lastDet = s0 + 0.75 * kBlock;
                c.peakSnr = std::max(c.peakSnr, (float)snr);
                c.levAcc += a2; c.levN++;
                c.het |= het;
                // seed the loop with the carrier found (parabolic interpolation)
                const double y0 = P(best - 1), y1 = P(best), y2 = P(best + 1), dn = y0 - 2 * y1 + y2;
                const double fk = (best + (std::fabs(dn) > 0 ? 0.5 * (y0 - y2) / dn : 0)) * bin;
                if (std::fabs(c.pllF * fc / (2 * kPi) - fk) > 0.5 * bin) c.pllF = 2 * kPi * fk / fc;
            }
            c.prevDet = true; c.prevK = best;
            if (c.active) c.carK = best;
        } else {
            c.prevDet = false;
            if (c.active && (double)c.nCh - c.lastDet > hangS) close(c, idx);
        }
        while (!c.done.empty() && c.done.front().t1 < (double)c.nCh - 3 * fc) c.done.pop_front();
    }

    void close(Chan& c, int idx) {
        c.active = false;
        c.done.push_back({c.tOpen, c.lastDet + hang * fc});
        c.txCount++;
        AirbandActivity e;
        e.wallTime = c.wallStart;
        e.startSec = std::max(0.0, c.tOpen / fc);
        e.durSec = std::max(0.0, (c.lastDet - c.tOpen) / fc);
        e.chan = idx;
        e.name = airbandName(c.cfg.freqHz, c.cfg.is833); e.label = c.cfg.label;
        e.freqHz = c.cfg.freqHz;
        e.snrDb = c.peakSnr;
        e.levelDb = c.levN ? (float)(10 * std::log10(c.levAcc / c.levN + 1e-20)) : -200.f;
        e.heterodyne = c.het;
        c.lastEndSec = c.lastDet / fc;
        act.push_front(e);
        while (act.size() > kAirbandLogMax) act.pop_back();
    }

    bool gateAt(const Chan& c, double t) const {
        if (c.active && t >= c.tOpen && t <= c.lastDet + hang * fc) return true;
        for (const auto& iv : c.done) if (t >= iv.t0 && t <= iv.t1) return true;
        return false;
    }

    // ------------------------------------------------ the radio's tuning error

    void estimateTune() {
        struct Line { int chan; double f; };
        std::vector<Line> lines;
        const double bin = fi / kWide;
        for (size_t i = 0; i < ch.size(); i++) {
            const Chan& c = ch[i];
            if (!c.inBand) continue;
            std::vector<float> h(c.hold);
            const int kr = (int)(12000 / bin);
            std::vector<float> v;
            for (int k = -kr; k <= kr; k++) v.push_back(h[(size_t)((k + kWide) % kWide)]);
            std::nth_element(v.begin(), v.begin() + (long)v.size() / 2, v.end());
            const double med = std::max(1e-30, (double)v[v.size() / 2]);
            const int km = (int)((kMaxTuneErr + c.sp->tol) / bin);
            auto H = [&](int k) { return (double)h[(size_t)((k + kWide) % kWide)]; };
            const double dcAt = -c.posHz;   // the radio's DC spike in this channel's spectrum (taken before the correction): ignored
            for (int k = -km; k <= km; k++) {
                if (H(k) < 16 * med) continue;
                double side = 0;   // against the continuum beside it (voice sidebands, a neighbour's skirt)
                for (int d = 6; d <= 14; d++) side += H(k - d) + H(k + d);
                if (H(k) < 10 * side / 18) continue;
                bool mx = true;
                for (int d = -2; d <= 2; d++) if (d && H(k + d) > H(k)) mx = false;
                if (!mx) continue;
                const double fr = k * bin;
                if (std::fabs(fr - dcAt) < 300) continue;
                lines.push_back({(int)i, fr});
            }
        }
        auto match = [&](double d, double& refined) {
            std::vector<double> got;
            for (size_t i = 0; i < ch.size(); i++) {
                double bestd = 1e9, bf = 0;
                for (const auto& l : lines) if (l.chan == (int)i && std::fabs(l.f - d) < bestd) { bestd = std::fabs(l.f - d); bf = l.f; }
                if (bestd <= ch[i].sp->tol) got.push_back(bf);
            }
            if (got.empty()) { refined = d; return 0; }
            std::sort(got.begin(), got.end());
            refined = got[got.size() / 2];
            return (int)got.size();
        };
        double curRef;
        const int curN = match(tuneErr, curRef);
        int bestN = 0; double bestD = 0, bestRef = 0;
        for (const auto& l : lines) {
            if (std::fabs(l.f) > kMaxTuneErr || std::fabs(l.f - tuneErr) <= 1000) continue;
            double ref;
            const int n = match(l.f, ref);
            const bool ok = (n >= 2 && n > curN) || (!tuneKnown && curN == 0 && n >= 1 && std::fabs(ref) <= kSingleLimit);
            if (!ok) continue;
            if (n > bestN || (n == bestN && std::fabs(ref) < std::fabs(bestRef))) { bestN = n; bestD = l.f; bestRef = ref; }
        }
        (void)bestD;
        double nt = tuneErr;
        if (bestN > 0) nt = bestRef;
        else if (curN > 0) nt = tuneErr + 0.3 * (curRef - tuneErr);
        if (bestN > 0 || curN > 0) tuneKnown = true;
        if (std::fabs(nt - tuneErr) > 0.5) {
            const bool jump = std::fabs(nt - tuneErr) > 1000;
            tuneErr = nt;
            for (auto& c : ch) setFine(c);
            if (jump && log) {
                char b[120];
                snprintf(b, sizeof b, "Airband: the radio is %+.1f kHz off, measured from %d channel%s", tuneErr / 1e3, bestN, bestN == 1 ? "" : "s");
                log(b);
            }
        }
    }

    // ------------------------------------------------ audio out

    void output() {
        if (ch.empty()) return;
        int64_t avail = INT64_MAX;
        for (const auto& c : ch) if (c.inBand) avail = std::min(avail, c.audBase + (int64_t)c.aud.size());
        if (avail == INT64_MAX) return;
        const int64_t D8 = (int64_t)(kDelaySec * kAirbandAudioRate);
        const int64_t upto = avail - D8;
        if (upto <= nOut8) return;
        const size_t n = (size_t)(upto - nOut8);
        mix.assign(n, 0.f);
        bool anySolo = false;
        for (const auto& c : ch) anySolo |= c.inBand && c.cfg.solo;
        // which channels play
        const double now = sigSec();
        int prioOpen = -1;
        for (size_t i = 0; i < ch.size(); i++) if (ch[i].inBand && ch[i].active && ch[i].cfg.priority && !ch[i].cfg.muted) prioOpen = (int)i;
        if (scan) {
            const bool keep = scanCh >= 0 && scanCh < (int)ch.size() && (ch[(size_t)scanCh].active || now - ch[(size_t)scanCh].lastEndSec < kScanResume);
            if (prioOpen >= 0 && scanCh != prioOpen) scanCh = prioOpen;
            else if (!keep) {
                scanCh = -1;
                for (size_t i = 0; i < ch.size(); i++) {
                    const Chan& c = ch[i];
                    if (c.inBand && c.active && !c.cfg.muted && (!anySolo || c.cfg.solo)) { scanCh = (int)i; break; }
                }
            }
        }
        std::vector<float> one(n);
        for (size_t i = 0; i < ch.size(); i++) {
            Chan& c = ch[i];
            if (!c.inBand) continue;
            bool plays = !c.cfg.muted && (!anySolo || c.cfg.solo);
            if (scan) plays = plays && (int)i == scanCh;
            const float duck = (!scan && prioOpen >= 0 && (int)i != prioOpen) ? 0.25f : 1.f;
            const float g = (float)(0.7 / std::max(c.amp, 1e-9));
            for (size_t j = 0; j < n; j++) {
                const int64_t jj = nOut8 + (int64_t)j;
                const float s = jj >= c.audBase && jj - c.audBase < (int64_t)c.aud.size() ? c.aud[(size_t)(jj - c.audBase)] : 0.f;
                const bool open = gateAt(c, (double)jj * fc / kAirbandAudioRate);
                c.gate += ((open ? 1.f : 0.f) - c.gate) * 0.1f;
                float v = c.gate > 1e-3f ? s * g * c.gate : 0.f;
                v = std::max(-1.5f, std::min(1.5f, v));
                one[j] = v;
                if (plays) mix[j] += v * duck;
            }
            if (chanTap) chanTap((int)i, one.data(), n);
            const int64_t drop = upto - c.audBase;
            for (int64_t d = 0; d < drop && !c.aud.empty(); d++) c.aud.pop_front();
            c.audBase = upto;
        }
        for (auto& v : mix) v = std::tanh(v);   // soft limit
        nOut8 = upto;
        if (mixTap) mixTap(mix.data(), n);
        if (silent.load()) return;
        pcm.clear();
        up.process(mix.data(), n, pcm);
        if (!audioOut) { audioOut = std::make_unique<AudioOut>(); audioOut->start(48000); audioOut->setStartThreshold(48000 / 4); }
        audioOut->setVolume(volume.load()); audioOut->setMuted(muted.load());
        if (audioOut->bufferedFrames() > 48000 * 3 / 2) audioOut->flush();
        audioOut->write(pcm.data(), (int)(pcm.size() / 2));
    }

    void report() {
        tel.seq++;
        tel.timeSec = sigSec();
        tel.levelDb = nPower ? (float)(10 * std::log10(power / (double)nPower + 1e-20)) : -200.f;
        power = 0; nPower = 0;
        tel.centerHz = center; tel.channelRate = fc;
        tel.cfoHz = tuneErr; tel.tuneKnown = tuneKnown;
        tel.squelchDb = sql; tel.hangSec = hang; tel.scan = scan; tel.scanChan = scan ? scanCh : -1;
        tel.channels.clear();
        bool anySolo = false, any = false, open = false;
        float best = -99, bestOpen = -99;
        for (const auto& c : ch) anySolo |= c.inBand && c.cfg.solo;
        int prioOpen = -1;
        for (size_t i = 0; i < ch.size(); i++) if (ch[i].inBand && ch[i].active && ch[i].cfg.priority && !ch[i].cfg.muted) prioOpen = (int)i;
        uint64_t tx = 0;
        for (size_t i = 0; i < ch.size(); i++) {
            Chan& c = ch[i];
            AirbandChannelState s;
            s.freqHz = c.cfg.freqHz; s.is833 = c.cfg.is833; s.name = airbandName(c.cfg.freqHz, c.cfg.is833); s.label = c.cfg.label;
            s.inBand = c.inBand; s.open = c.active; s.heterodyne = c.active && c.het;
            s.muted = c.cfg.muted; s.solo = c.cfg.solo; s.priority = c.cfg.priority;
            s.levelDb = c.powN ? (float)(10 * std::log10(c.powAcc / c.powN + 1e-20)) : -200.f;
            c.powAcc = 0; c.powN = 0;
            s.snrDb = c.inBand ? c.snrDb : -99;
            s.carrierHz = (float)(tuneErr + c.pllF * fc / (2 * kPi));
            s.transmissions = c.txCount + (c.active ? 1 : 0);
            s.lastSec = c.active ? tel.timeSec : c.lastEndSec;
            bool plays = c.active && !c.cfg.muted && (!anySolo || c.cfg.solo);
            if (scan) plays = plays && (int)i == scanCh;
            s.playing = plays;
            if (c.inBand) { any = true; best = std::max(best, s.snrDb); }
            if (c.active) { open = true; bestOpen = std::max(bestOpen, s.snrDb); }
            tx += c.txCount;
            tel.channels.push_back(std::move(s));
        }
        (void)prioOpen;
        tel.state = !any ? 0 : open ? 2 : 1;
        tel.dataValid = open;
        tel.snrDb = open ? bestOpen : any ? best : 0;
        tel.blocksOk = tx; tel.blocksBad = 0;
        tel.activity.assign(act.begin(), act.end());
        std::lock_guard<std::mutex> lk(mu);
        pub = tel;
    }
};

AirbandReceiver::AirbandReceiver() : p_(std::make_unique<Impl>()) { p_->chanSet = airbandChannelsFor(airbandTestLayout(), kDefaultDialHz); }   // the test signal's channels until the app sends its list
AirbandReceiver::~AirbandReceiver() = default;

void AirbandReceiver::configure(double inputRateHz) { std::lock_guard<std::mutex> lk(p_->mu); p_->rate = inputRateHz; p_->resetReq = true; }
void AirbandReceiver::setSignalOffset(double hz) { std::lock_guard<std::mutex> lk(p_->mu); p_->offsetHz = hz; }
void AirbandReceiver::setCenterHz(double hz) { std::lock_guard<std::mutex> lk(p_->mu); if (hz != p_->centerSet) { p_->centerSet = hz; p_->resetReq = true; } }
bool AirbandReceiver::ready() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->rate >= airbandTuning().minSampleRate - 1; }
void AirbandReceiver::reset() {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->resetReq = true;
    const uint64_t s = p_->pub.seq;
    p_->pub = AirbandTelemetry();
    p_->pub.seq = s;
}

void AirbandReceiver::feed(const cf32* x, size_t n) {
    Impl& m = *p_;
    if (m.tapsReq.exchange(false)) { std::lock_guard<std::mutex> lk(m.mu); m.chanTap = m.chanTapSet; m.mixTap = m.mixTapSet; }
    if (m.resetReq.exchange(false)) { m.chanReq = false; m.resetState(); }
    {
        std::lock_guard<std::mutex> lk(m.mu);
        m.sql = m.sqlSet; m.hang = m.hangSet; m.scan = m.scanSet;
        if (m.tuneGuessReq) { m.tuneGuessReq = false; m.tuneErr = m.tuneGuess; m.tuneKnown = false; for (auto& c : m.ch) m.setFine(c); }
    }
    if (m.chanReq.exchange(false)) { std::vector<AirbandChannel> l; { std::lock_guard<std::mutex> lk(m.mu); l = m.chanSet; } m.applyFlags(l); }
    if (m.curRate <= 0 || n == 0) return;
    // bad samples (NaN, infinity, wild values) become silence
    m.clean.resize(n);
    for (size_t i = 0; i < n; i++) {
        float re = x[i].real(), im = x[i].imag();
        if (!std::isfinite(re) || !std::isfinite(im)) re = im = 0;
        re = std::max(-2.f, std::min(2.f, re)); im = std::max(-2.f, std::min(2.f, im));
        m.clean[i] = cf32(re, im);
        m.power += (double)re * re + (double)im * im;
    }
    m.nPower += (int64_t)n;
    // in pieces, so the tuning error and the reports keep their time
    const size_t piece = (size_t)std::max(1024.0, 0.02 * m.curRate);
    for (size_t o = 0; o < n; o += piece) {
        const size_t k = std::min(piece, n - o);
        for (size_t i = 0; i < m.ch.size(); i++) if (m.ch[i].inBand) m.runChannel(m.ch[i], (int)i, m.clean.data() + o, k);
        m.nIn += (int64_t)k;
        m.output();
        if (m.sigSec() >= m.nextEst) { m.nextEst += 0.5; m.estimateTune(); }
        while ((double)m.nIn >= m.nextReport) { m.nextReport += 0.25 * m.curRate; m.report(); }
    }
}

bool AirbandReceiver::telemetry(AirbandTelemetry& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (p_->pub.seq <= lastSeq) return false;
    out = p_->pub;
    return true;
}
void AirbandReceiver::setLogCallback(std::function<void(const std::string&)> cb) { std::lock_guard<std::mutex> lk(p_->mu); p_->log = std::move(cb); }
void AirbandReceiver::setChannels(const std::vector<AirbandChannel>& c) { std::lock_guard<std::mutex> lk(p_->mu); p_->chanSet = c; p_->chanReq = true; }
void AirbandReceiver::setSquelchDb(float db) { std::lock_guard<std::mutex> lk(p_->mu); p_->sqlSet = std::max(-5.f, std::min(30.f, db)); }
void AirbandReceiver::setHangSec(float s) { std::lock_guard<std::mutex> lk(p_->mu); p_->hangSet = std::max(0.f, std::min(3.f, s)); }
void AirbandReceiver::setScan(bool on) { std::lock_guard<std::mutex> lk(p_->mu); p_->scanSet = on; }
void AirbandReceiver::setTuneErrorHz(double hz) { std::lock_guard<std::mutex> lk(p_->mu); p_->tuneGuess = hz; p_->tuneGuessReq = true; }
void AirbandReceiver::setVolume(float v) { p_->volume = std::max(0.f, std::min(1.f, v)); }
void AirbandReceiver::setMuted(bool m) { p_->muted = m; }
void AirbandReceiver::setSilent(bool s) { p_->silent = s; }
void AirbandReceiver::setChannelTap(std::function<void(int, const float*, size_t)> cb) { std::lock_guard<std::mutex> lk(p_->mu); p_->chanTapSet = std::move(cb); p_->tapsReq = true; }
void AirbandReceiver::setMixTap(std::function<void(const float*, size_t)> cb) { std::lock_guard<std::mutex> lk(p_->mu); p_->mixTapSet = std::move(cb); p_->tapsReq = true; }

ModeTuning airbandTuning() {
    ModeTuning t;
    t.stdMode = 28; t.id = "airband"; t.name = "Airband";
    t.minMhz = 108; t.maxMhz = 140; t.defMhz = 118.700;
    t.sampleRate = 2000000;
    t.basebandHz = 1750000;
    t.bandwidthMhz = 1.6;                    // the band the channels are taken from
    t.minSampleRate = 250000;
    t.tuneOffsetHz = 25000.0 * 13 / 6;       // 54.167 kHz: with the dial on the raster the DC spike falls half way between two 8.33 kHz channels
    return t;
}

} // namespace dect2
