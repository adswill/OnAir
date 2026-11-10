// VOR / ILS receiver (see vor_rx.h).
#include "dect2/vor/vor_rx.h"
#include "dect2/audio/audioout.h"
#include "dect2/ils/ils.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <vector>

namespace dect2 {

namespace {
using cd = std::complex<double>;
constexpr double kPi = 3.141592653589793, kTwoPi = 2 * kPi;
constexpr double kBlockSec = 0.5;       // a measurement: 15 cycles of 30 Hz, 45 of 90 Hz, 75 of 150 Hz
constexpr int kHist = 20;               // measurements kept for the scatter (5 s)

// windowed-sinc low-pass (Blackman), cutoff in cycles per sample, odd length
std::vector<double> lowpass(double fc, int n) {
    if (n % 2 == 0) n++;
    std::vector<double> h((size_t)n);
    const int m = n / 2;
    double s = 0;
    for (int i = 0; i < n; i++) {
        const int k = i - m;
        const double sinc = k == 0 ? 2 * fc : std::sin(kTwoPi * fc * k) / (kPi * k);
        const double w = 0.42 - 0.5 * std::cos(kTwoPi * i / (n - 1)) + 0.08 * std::cos(2 * kTwoPi * i / (n - 1));
        h[(size_t)i] = sinc * w;
        s += h[(size_t)i];
    }
    for (auto& v : h) v /= s;
    return h;
}

// FIR decimator: push every input sample, the filter is evaluated only for the outputs
struct DecimFir {
    std::vector<double> h;
    std::vector<cd> buf;        // twice the length, so the window is always contiguous
    size_t pos = 0;
    int dec = 1, cnt = 0;
    void init(const std::vector<double>& taps, int d) { h = taps; buf.assign(h.size() * 2, cd(0, 0)); pos = 0; dec = std::max(1, d); cnt = 0; }
    void clear() { std::fill(buf.begin(), buf.end(), cd(0, 0)); pos = 0; cnt = 0; }
    bool push(cd x, cd& out) {
        const size_t L = h.size();
        buf[pos] = x; buf[pos + L] = x;
        pos = (pos + 1) % L;
        if (++cnt < dec) return false;
        cnt = 0;
        cd acc(0, 0);
        const cd* w = &buf[pos];   // oldest first
        for (size_t i = 0; i < L; i++) acc += w[i] * h[L - 1 - i];
        out = acc;
        return true;
    }
};

struct Biquad {
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
    void set(int type, double f, double q, double fs) {   // 0 low-pass, 1 high-pass, 2 notch (RBJ)
        const double w = kTwoPi * f / fs, cw = std::cos(w), al = std::sin(w) / (2 * q), a0 = 1 + al;
        if (type == 0) { b0 = (1 - cw) / 2; b1 = 1 - cw; b2 = b0; }
        else if (type == 1) { b0 = (1 + cw) / 2; b1 = -(1 + cw); b2 = b0; }
        else { b0 = 1; b1 = -2 * cw; b2 = 1; }
        b0 /= a0; b1 /= a0; b2 /= a0; a1 = -2 * cw / a0; a2 = (1 - al) / a0;
        z1 = z2 = 0;
    }
    double run(double x) { const double y = b0 * x + z1; z1 = b1 * x - a1 * y + z2; z2 = b2 * x - a2 * y; return y; }
};

void fft(std::vector<cd>& a) {   // radix 2, in place, size a power of two
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; i++) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        const cd wl = std::polar(1.0, -kTwoPi / (double)len);
        for (size_t i = 0; i < n; i += len) {
            cd w(1, 0);
            for (size_t k = 0; k < len / 2; k++) {
                const cd u = a[i + k], v = a[i + k + len / 2] * w;
                a[i + k] = u + v; a[i + k + len / 2] = u - v;
                w *= wl;
            }
        }
    }
}

double circStdDeg(const std::deque<double>& v) {   // circular standard deviation of angles in degrees
    if (v.size() < 2) return 0;
    double s = 0, c = 0;
    for (double d : v) { s += std::sin(d * kPi / 180); c += std::cos(d * kPi / 180); }
    const double r = std::min(1.0, std::hypot(s, c) / (double)v.size());
    return std::sqrt(std::max(0.0, -2 * std::log(std::max(r, 1e-12)))) * 180 / kPi;
}
} // namespace

NavFunction navFunctionFor(int setting, double dialHz) {
    const double mhz = dialHz / 1e6;
    if (setting == 1) return NavFunction::Vor;
    if (mhz >= 328.6 && mhz <= 335.4) return NavFunction::Glideslope;
    if (mhz >= 74.5 && mhz <= 75.5) return NavFunction::Marker;
    if (setting == 2) return NavFunction::Localizer;
    if (ilsIsLocalizer(mhz)) return NavFunction::Localizer;
    return NavFunction::Vor;
}

struct VorReceiver::Impl {
    std::mutex mu;
    VorTelemetry pub;
    std::function<void(const std::string&)> logCb;
    std::function<void(const float*, size_t, double)> audioTap, audioTapSet;
    std::atomic<bool> tapReq{false}, resetReq{false};
    std::atomic<double> offsetSet{0}, centerSet{0};
    std::atomic<int> funcSet{0};
    std::atomic<float> volume{1.f};
    std::atomic<bool> muted{false}, silent{false}, identFilter{true};
    std::unique_ptr<AudioOut> audioOut;

    // set-up
    double rate = 0;               // input
    double r1 = 0, fs1 = 0, fs2 = 0, fs3 = 0;
    int d1 = 1;
    double offset = 0;
    cd rot{1, 0}, rotStep{1, 0};
    uint64_t nIn = 0;
    DecimFir fir0;                 // input -> r1 (about 200 kHz): wide transition, the next stage does the channel
    DecimFir fir1;                 // r1 -> fs1
    // carrier
    std::vector<cd> fftBuf;
    size_t fftN = 8192;
    bool acquired = false;
    double cfo = 0;
    int farCount = 0;
    float cnrDb = -99;
    cd rot1{1, 0}, rot1Step{1, 0};
    std::vector<double> h2;        // channel filter at fs1 (+-12.5 kHz)
    std::vector<cd> h2buf;
    size_t h2pos = 0;
    double carrier = 0;            // slow mean of the envelope
    uint64_t n1 = 0;               // samples at fs1
    // tone paths
    DecimFir lpA, lpB, lpId;
    double subHz = 9960;
    uint64_t n2 = 0;               // samples at fs2
    cd prevB{0, 0};
    double prevA = 0;
    std::vector<double> bufA, bufAavg, bufFm;   // the last block at fs2
    std::vector<double> magB;
    size_t blockLen = 0, hop = 0, sinceBlock = 0;
    uint64_t blockStart = 0;       // fs2 index of bufA[0]
    // results
    std::deque<double> radials, ddms;
    cd radSmooth{0, 0};
    double ddmSmooth = 0;
    bool haveDdm = false;
    // marker
    cd mkAcc[3];
    int mkN = 0, mkLen = 0;
    float mkDepth[3] = {}, mkHold[3] = {};
    double mkHoldT = 0;
    // marker keying (Annex 10 3.1.7.4: outer dashes 2/s, middle dot-dash, inner dots 6/s): the keyed lengths of each tone, in 25 ms blocks
    bool mkOn[3] = {};
    int mkRun[3] = {};
    std::deque<int> mkOnLen[3];
    // ident
    std::deque<float> idMag;       // last 10 s at fs3
    float idThr = 0, idP90 = 0, idP10 = 0;
    bool idPresent = false, key = false;
    int keyRun = 0, sinceStats = 0;
    double pending = 0, carry = 0;   // a mark waiting for its gap; the part of a mark before a short drop
    float idS1 = 0, idS2 = 0;
    std::deque<double> marks;      // recent mark lengths (s)
    std::vector<double> letterMarks;
    std::string word, lastWord, confirmed;
    double unit = 0.15;
    // audio
    Biquad hp1, hp2, lp1, lp2, lp3, notch;
    double audPh = 0;
    float audPrev = 0;
    std::vector<float> audFs1, pcm;
    // report
    double power = 0;
    uint64_t nPower = 0;
    VorTelemetry tel;

    void log(const std::string& s) { std::function<void(const std::string&)> cb; { std::lock_guard<std::mutex> lk(mu); cb = logCb; } if (cb) cb(s); }

    void setup(double r) {
        rate = r;
        d1 = std::max(1, (int)std::floor(r / 200e3));
        r1 = r / d1;
        // pass +-20 kHz, stop from r1 - 20 kHz (what folds onto the channel)
        fir0.init(d1 > 1 ? lowpass(0.5 * r1 / r, (int)(5.5 * r / std::max(20e3, r1 - 40e3))) : std::vector<double>{1.0}, d1);
        const int d2 = std::max(1, (int)std::floor(r1 / 48e3));
        fs1 = r1 / d2;
        fir1.init(lowpass(17e3 / r1, (int)(5.5 * r1 / 10e3)), d2);
        h2 = lowpass(12.5e3 / fs1, (int)(5.5 * fs1 / 4.5e3));
        const int d3 = std::max(1, (int)std::lround(fs1 / 2500));
        fs2 = fs1 / d3;
        const auto hA = lowpass(800 / fs1, (int)(5.5 * fs1 / 500));
        lpA.init(hA, d3); lpB.init(hA, d3);
        const int d4 = std::max(1, (int)std::lround(fs1 / 200));
        fs3 = fs1 / d4;
        lpId.init(lowpass(70 / fs1, (int)(5.5 * fs1 / 70)), d4);
        blockLen = (size_t)std::lround(kBlockSec * fs2);
        hop = blockLen / 2;
        mkLen = (int)std::lround(0.025 * fs1);
        fftN = 8192;
        while ((double)fftN > fs1 * 0.25 && fftN > 1024) fftN /= 2;
        hp1.set(1, 300, 0.707, fs1); hp2.set(1, 300, 0.707, fs1);
        lp1.set(0, 3000, 0.707, fs1); lp2.set(0, 3000, 0.707, fs1); lp3.set(0, 3400, 0.6, fs1);
        notch.set(2, 1020, 4, fs1);
        resetState();
    }
    void resetState() {
        fir0.clear(); fir1.clear(); lpA.clear(); lpB.clear(); lpId.clear();
        fftBuf.clear();
        acquired = false; cfo = 0; farCount = 0; cnrDb = -99;
        rot1 = cd(1, 0);
        h2buf.assign(h2.size() * 2, cd(0, 0)); h2pos = 0;
        carrier = 0; n1 = 0; n2 = 0; prevB = 0; prevA = 0;
        bufA.clear(); bufAavg.clear(); bufFm.clear(); magB.clear(); sinceBlock = 0; blockStart = 0;
        radials.clear(); ddms.clear(); radSmooth = 0; ddmSmooth = 0; haveDdm = false;
        for (auto& v : mkAcc) v = 0;
        mkN = 0; for (int i = 0; i < 3; i++) mkDepth[i] = mkHold[i] = 0;
        mkHoldT = 0;
        for (int i = 0; i < 3; i++) { mkOn[i] = false; mkRun[i] = 0; mkOnLen[i].clear(); }
        idMag.clear(); idThr = idP90 = idP10 = 0; idPresent = false; key = false; keyRun = 0; pending = carry = 0; sinceStats = 0; idS1 = idS2 = 0;
        marks.clear(); letterMarks.clear(); word.clear(); lastWord.clear(); confirmed.clear(); unit = 0.15;
        audPh = 0; audPrev = 0;
        const uint64_t s = tel.seq;
        tel = VorTelemetry();
        tel.seq = s;
    }

    NavFunction func() const {
        const double c = centerSet.load();
        return navFunctionFor(funcSet.load(), c > 0 ? c - vorTuning().tuneOffsetHz : 0.0);
    }

    // ------------------------------------------------------------ carrier search (at fs1, before the carrier is moved to 0 Hz)
    void carrierSearch() {
        std::vector<cd> a(fftBuf);
        double sw2 = 0;
        for (size_t i = 0; i < fftN; i++) { const double w = 0.5 - 0.5 * std::cos(kTwoPi * (double)i / (double)fftN); a[i] *= w; sw2 += w * w; }
        fft(a);
        std::vector<double> p(fftN);
        for (size_t i = 0; i < fftN; i++) p[i] = std::norm(a[i]);
        const double bin = fs1 / (double)fftN;
        const int kmax = (int)std::min((double)fftN / 2 - 3, 8000 / bin);
        int best = 0;
        double bp = -1;
        for (int k = -kmax; k <= kmax; k++) { const double v = p[(size_t)((k + (int)fftN) % (int)fftN)]; if (v > bp) { bp = v; best = k; } }
        auto at = [&](int k) { return p[(size_t)((k + (int)fftN) % (int)fftN)]; };
        const double y0 = at(best - 1), y1 = at(best), y2 = at(best + 1);
        const double den = y0 - 2 * y1 + y2;
        const double frac = den != 0 ? 0.5 * (y0 - y2) / den : 0;
        const double f = (best + std::max(-0.5, std::min(0.5, frac))) * bin;
        double pc = 0;
        for (int k = best - 2; k <= best + 2; k++) pc += at(k);
        const double A2 = pc / ((double)fftN * sw2);
        std::vector<double> s(p);
        std::nth_element(s.begin(), s.begin() + (ptrdiff_t)(s.size() / 2), s.end());
        const double sigma2 = s[s.size() / 2] / std::log(2.0) / sw2;   // noise power over the whole fs1 band
        const double n25 = sigma2 * 25e3 / fs1;
        cnrDb = (float)(10 * std::log10(A2 / std::max(n25, 1e-30)));
        if (cnrDb < 6) { if (acquired && cnrDb < 0) { acquired = false; } return; }
        if (!acquired) { cfo = f; acquired = true; farCount = 0; rot1 = cd(1, 0); return; }
        if (std::fabs(f - cfo) < 150) { cfo += 0.3 * (f - cfo); farCount = 0; }
        else if (++farCount >= 3) { cfo = f; farCount = 0; }
    }

    // ------------------------------------------------------------ one sample at fs1
    void atFs1(cd y) {
        n1++;
        if (fftBuf.size() < fftN) fftBuf.push_back(y);
        if (fftBuf.size() >= fftN) { carrierSearch(); fftBuf.clear(); }
        if (!acquired) return;
        rot1Step = std::polar(1.0, -kTwoPi * cfo / fs1);
        const cd z = y * rot1;
        rot1 *= rot1Step;
        if ((n1 & 1023) == 0) rot1 /= std::abs(rot1);
        // channel filter
        const size_t L = h2.size();
        h2buf[h2pos] = z; h2buf[h2pos + L] = z;
        h2pos = (h2pos + 1) % L;
        cd acc(0, 0);
        const cd* w = &h2buf[h2pos];
        for (size_t i = 0; i < L; i++) acc += w[i] * h2[L - 1 - i];
        const double e = std::abs(acc);
        carrier = carrier <= 0 ? e : carrier + (e - carrier) * (1.0 / (0.2 * fs1));
        // ident (1020 Hz) and the two tone paths
        const double tn = (double)n1 / fs1;
        cd out;
        if (lpId.push(e * std::polar(1.0, -kTwoPi * 1020 * tn), out)) atFs3(out);
        cd oa, ob;
        const bool ga = lpA.push(cd(e, 0), oa);
        lpB.push(e * std::polar(1.0, -kTwoPi * subHz * tn), ob);
        if (ga) atFs2(oa.real(), ob);
        // marker tones
        const NavFunction fn = func();
        if (fn != NavFunction::Vor) {
            static const double mf[3] = {400, 1300, 3000};
            for (int k = 0; k < 3; k++) mkAcc[k] += (e - carrier) * std::polar(1.0, -kTwoPi * mf[k] * tn);
            if (++mkN >= mkLen) {
                for (int k = 0; k < 3; k++) {
                    mkDepth[k] = (float)(2 * std::abs(mkAcc[k]) / mkN / std::max(carrier, 1e-9));
                    mkHold[k] = std::max(mkHold[k] * 0.97f, mkDepth[k]);
                    mkAcc[k] = 0;
                    // keying: on above 0.35 of the carrier, off below 0.2 (a block that holds an edge counts as on when more than about a third keyed)
                    const bool on = mkOn[k] ? mkDepth[k] > 0.2f : mkDepth[k] > 0.35f;
                    if (on != mkOn[k]) {
                        if (mkOn[k]) { mkOnLen[k].push_back(mkRun[k]); if (mkOnLen[k].size() > 8) mkOnLen[k].pop_front(); }
                        mkOn[k] = on; mkRun[k] = 0;
                    }
                    if (mkRun[k] < 1000) mkRun[k]++;
                }
                mkN = 0;
            }
        }
        // voice
        double x = e / std::max(carrier, 1e-9) - 1;
        x = hp2.run(hp1.run(x));
        x = lp3.run(lp2.run(lp1.run(x)));
        if (identFilter.load()) x = notch.run(x);
        audFs1.push_back((float)(1.5 * x));
    }

    // ------------------------------------------------------------ ident (1020 Hz) at fs3
    void atFs3(cd z) {
        const float m = (float)(2 * std::abs(z) / std::max(carrier, 1e-9));
        const float ms = (m + idS1 + idS2) / 3;
        idS2 = idS1; idS1 = m;
        idMag.push_back(ms);
        if (idMag.size() > (size_t)(10 * fs3)) idMag.pop_front();
        if (++sinceStats >= (int)(fs3 / 10) && idMag.size() > (size_t)(fs3 / 4)) {
            sinceStats = 0;
            std::vector<float> s(idMag.begin(), idMag.end());
            std::sort(s.begin(), s.end());
            idP10 = s[s.size() / 10]; idP90 = s[s.size() * 9 / 10];
            idPresent = idP90 > 0.03f && idP90 > 2.5f * idP10;
            idThr = 0.5f * (idP10 + idP90);
        }
        const bool was = key;
        if (!idPresent) key = false;
        else if (key) key = ms > idThr * 0.85f;
        else key = ms > idThr * 1.15f;
        const double dt = 1.0 / fs3;
        if (key != was) {
            if (!key) pending = keyRun * dt + carry;          // a mark ended (kept until the gap is long enough to be real)
            else carry = pending > 0 ? pending + keyRun * dt : 0;   // a short drop inside a mark: the mark goes on
            if (key) pending = 0;
            keyRun = 0;
        }
        keyRun++;
        if (!key) {
            const double off = keyRun * dt;
            if (pending > 0 && off > 0.45 * unit) {
                if (pending > std::max(0.02, 0.45 * unit)) {   // shorter: a noise blip, not a dot
                    letterMarks.push_back(pending);
                    marks.push_back(pending);
                    if (marks.size() > 30) marks.pop_front();
                    estimateUnit();
                }
                pending = 0; carry = 0;
            }
            if (!letterMarks.empty() && pending == 0 && off > 2.2 * unit) endLetter();
            if (!word.empty() && off > 5.5 * unit) endWord();
        }
        tel.identKey = key;
        tel.identDepth = idP90;
    }
    void estimateUnit() {
        if (marks.size() < 3) return;
        std::vector<double> s(marks.begin(), marks.end());
        std::sort(s.begin(), s.end());
        size_t split = 0;
        double bestR = 1.7;
        for (size_t i = 1; i < s.size(); i++) if (s[i] / s[i - 1] > bestR) { bestR = s[i] / s[i - 1]; split = i; }
        if (split) {
            double dot = 0, dash = 0;
            for (size_t i = 0; i < split; i++) dot += s[i];
            for (size_t i = split; i < s.size(); i++) dash += s[i];
            dot /= (double)split; dash /= (double)(s.size() - split);
            unit = 0.5 * (dot + dash / 3);
        } else {
            const double med = s[s.size() / 2];
            unit = med < 0.25 ? med : med / 3;
        }
        unit = std::max(0.03, std::min(0.4, unit));
    }
    void endLetter() {
        std::string pat;
        for (double d : letterMarks) pat += d < 2 * unit ? '.' : '-';
        if (getenv("VORDBG")) { fprintf(stderr, "t=%.2f u=%.3f thr=%.3f p10=%.3f p90=%.3f:", (double)n1 / fs1, unit, idThr, idP10, idP90); for (double d : letterMarks) fprintf(stderr, " %.3f", d); fprintf(stderr, "\n"); }
        letterMarks.clear();
        char found = '?';
        for (char c = 'A'; c <= 'Z' && found == '?'; c++) if (pat == navMorse(c)) found = c;
        for (char c = '0'; c <= '9' && found == '?'; c++) if (pat == navMorse(c)) found = c;
        word += found;
    }
    void endWord() {
        if (word.find('?') == std::string::npos && word.size() <= 5) {
            if (word == lastWord) {
                if (confirmed != word) log("navaid ident " + word);
                confirmed = word;
            }
            lastWord = word;
            tel.identCount++;
        }
        word.clear();
    }

    // ------------------------------------------------------------ 30 / 90 / 150 Hz at fs2
    void atFs2(double a, cd b) {
        n2++;
        const double fm = std::arg(b * std::conj(prevB)) * fs2 / kTwoPi;
        const double aAvg = 0.5 * (a + prevA);   // the FM is the phase step between two samples: half a sample late
        prevB = b; prevA = a;
        if (n2 < 3) return;
        if (bufA.empty()) blockStart = n2;
        bufA.push_back(a); bufAavg.push_back(aAvg); bufFm.push_back(fm); magB.push_back(std::abs(b));
        if (bufA.size() < blockLen) return;
        measure();
        const size_t drop = hop;
        bufA.erase(bufA.begin(), bufA.begin() + (ptrdiff_t)drop);
        bufAavg.erase(bufAavg.begin(), bufAavg.begin() + (ptrdiff_t)drop);
        bufFm.erase(bufFm.begin(), bufFm.begin() + (ptrdiff_t)drop);
        magB.erase(magB.begin(), magB.begin() + (ptrdiff_t)drop);
        blockStart += drop;
    }
    cd tone(const std::vector<double>& v, double f, double mean, double& sw) const {
        cd q(0, 0);
        sw = 0;
        const size_t L = v.size();
        for (size_t i = 0; i < L; i++) {
            const double w = 0.5 - 0.5 * std::cos(kTwoPi * ((double)i + 0.5) / (double)L);
            q += w * (v[i] - mean) * std::polar(1.0, -kTwoPi * f * (double)(blockStart + i) / fs2);
            sw += w;
        }
        return q;
    }
    // the keying of tone k from its last keyed lengths: dashes (0.375 s), dots (0.083 s) or both; 0 until four keyed lengths were seen
    void markerKeying(int k) {
        tel.markerKeying = 0; tel.markerKeyingOk = false;
        if (k < 0 || mkOnLen[k].size() < 4) return;
        int dots = 0, dashes = 0;
        for (int b : mkOnLen[k]) { const double sec = b * 0.025; if (sec < 0.18) dots++; else if (sec > 0.25 && sec < 0.6) dashes++; }
        if (dots + dashes < (int)mkOnLen[k].size() - 1) return;   // lengths that are neither: not keyed as a marker
        tel.markerKeying = dashes && dots ? 2 : dashes ? 1 : 3;
        tel.markerKeyingOk = tel.markerKeying == k + 1;
    }
    void measure() {
        const NavFunction fn = func();
        tel.function = fn;
        double mA = 0, mAvg = 0, mF = 0, mB = 0;
        for (size_t i = 0; i < bufA.size(); i++) { mA += bufA[i]; mAvg += bufAavg[i]; mF += bufFm[i]; mB += magB[i]; }
        const double L = (double)bufA.size();
        mA /= L; mAvg /= L; mF /= L; mB /= L;
        const double c = std::max(mA, 1e-12);
        double sw;
        bool valid = false;
        if (fn == NavFunction::Vor) {
            const cd qa = tone(bufAavg, 30, mAvg, sw);
            const cd qb = tone(bufFm, 30, mF, sw);
            const double am = 2 * std::abs(qa) / sw / c, dev = 2 * std::abs(qb) / sw, sub = 2 * mB / c;
            tel.am30 = (float)am; tel.fmDevHz = (float)dev; tel.sub9960 = (float)sub;
            tel.subcarrierHz = (float)(subHz + mF);
            tel.tone30Hz = 30;
            valid = am > 0.05 && sub > 0.05 && dev > 150;
            if (valid) {
                const double r = navWrap360(std::arg(qb * std::conj(qa)) * 180 / kPi);
                radials.push_back(r);
                if (radials.size() > kHist) radials.pop_front();
                const cd u = std::polar(1.0, r * kPi / 180);
                radSmooth = std::abs(radSmooth) < 1e-9 ? u : radSmooth + 0.12 * (u - radSmooth);
                tel.radialRawDeg = r;
                tel.radialDeg = navWrap360(std::arg(radSmooth) * 180 / kPi);
                tel.radialStdDeg = circStdDeg(radials);
            }
            tel.vorValid = valid;
            tel.ilsValid = false;
        } else if (fn == NavFunction::Marker) {
            int best = -1;
            for (int k = 0; k < 3; k++) if (mkHold[k] > 0.3f && (best < 0 || mkHold[k] > mkHold[best])) best = k;
            tel.marker = best + 1;
            tel.markerOn = best >= 0 && mkDepth[best] > 0.3f;
            tel.markerDepth = best >= 0 ? mkHold[best] : 0;
            markerKeying(best);
            valid = best >= 0;
            tel.vorValid = tel.ilsValid = false;
        } else {
            const cd q90 = tone(bufA, 90, mA, sw), q150 = tone(bufA, 150, mA, sw);
            const double m90 = 2 * std::abs(q90) / sw / c, m150 = 2 * std::abs(q150) / sw / c;
            tel.m90 = m90; tel.m150 = m150; tel.sdm = m90 + m150;
            valid = m90 + m150 > 0.1;
            if (valid) {
                const double d = m90 - m150;
                ddms.push_back(d);
                if (ddms.size() > kHist) ddms.pop_front();
                ddmSmooth = haveDdm ? ddmSmooth + 0.25 * (d - ddmSmooth) : d;
                haveDdm = true;
                tel.ddm = ddmSmooth;
                double s = 0, s2 = 0;
                for (double v : ddms) { s += v; s2 += v * v; }
                const double n = (double)ddms.size();
                tel.ddmStd = n > 1 ? std::sqrt(std::max(0.0, s2 / n - (s / n) * (s / n))) : 0;
            }
            tel.ilsValid = valid;
            tel.vorValid = false;
            // a marker can be heard on 75 MHz only, but say so whatever the frequency
            int best = -1;
            for (int k = 0; k < 3; k++) if (mkHold[k] > 0.3f && (best < 0 || mkHold[k] > mkHold[best])) best = k;
            tel.marker = best + 1; tel.markerOn = best >= 0 && mkDepth[best] > 0.3f; tel.markerDepth = best >= 0 ? mkHold[best] : 0;
            markerKeying(best);
        }
        if (valid) tel.blocksOk++; else tel.blocksBad++;
        tel.dataValid = valid;
        report();
    }

    void report() {
        tel.seq++;
        tel.timeSec = (double)nIn / std::max(1.0, rate);
        tel.levelDb = nPower ? (float)(10 * std::log10(power / (double)nPower + 1e-20)) : -200.f;
        power = 0; nPower = 0;
        tel.cfoHz = cfo;
        tel.snrDb = cnrDb;
        tel.state = !acquired ? 0 : tel.dataValid ? 2 : 1;
        tel.inputRate = rate;
        const double ce = centerSet.load();
        tel.dialHz = ce > 0 ? ce - vorTuning().tuneOffsetHz : 0;
        tel.demodRate = fs1;
        tel.ident = confirmed; tel.identLast = lastWord;
        tel.morse.clear();
        for (double d : letterMarks) tel.morse += d < 2 * unit ? '.' : '-';
        tel.identWpm = (float)(1.2 / unit);
        std::lock_guard<std::mutex> lk(mu);
        pub = tel;
    }

    void audio() {
        if (audFs1.empty()) return;
        std::function<void(const float*, size_t, double)> tap;
        { std::lock_guard<std::mutex> lk(mu); tap = audioTap; }
        if (tap) tap(audFs1.data(), audFs1.size(), fs1);
        if (!silent.load()) {
            pcm.clear();
            const double step = fs1 / 48000.0;
            for (float v : audFs1) {
                while (audPh < 1.0) { const float s = audPrev + (v - audPrev) * (float)audPh; const float o = std::tanh(s); pcm.push_back(o); pcm.push_back(o); audPh += step; }
                audPh -= 1.0;
                audPrev = v;
            }
            if (!audioOut) { audioOut = std::make_unique<AudioOut>(); audioOut->start(48000); audioOut->setStartThreshold(48000 / 4); }
            audioOut->setVolume(volume.load()); audioOut->setMuted(muted.load());
            if (audioOut->bufferedFrames() > 48000 * 3 / 2) audioOut->flush();
            audioOut->write(pcm.data(), (int)(pcm.size() / 2));
        }
        audFs1.clear();
    }
};

VorReceiver::VorReceiver() : p_(std::make_unique<Impl>()) {}
VorReceiver::~VorReceiver() = default;

void VorReceiver::configure(double r) { std::lock_guard<std::mutex> lk(p_->mu); p_->rate = r; p_->resetReq = true; }
void VorReceiver::setSignalOffset(double hz) { p_->offsetSet = hz; }
void VorReceiver::setCenterHz(double hz) { p_->centerSet = hz; }
void VorReceiver::setFunction(int f) { p_->funcSet = std::max(0, std::min(2, f)); }
bool VorReceiver::ready() const { return p_->rate >= vorTuning().minSampleRate - 1; }
void VorReceiver::reset() {
    p_->resetReq = true;
    std::lock_guard<std::mutex> lk(p_->mu);
    const uint64_t s = p_->pub.seq;
    p_->pub = VorTelemetry();
    p_->pub.seq = s;
}

void VorReceiver::feed(const cf32* x, size_t n) {
    Impl& m = *p_;
    if (m.tapReq.exchange(false)) { std::lock_guard<std::mutex> lk(m.mu); m.audioTap = m.audioTapSet; }
    if (m.resetReq.exchange(false)) { double r; { std::lock_guard<std::mutex> lk(m.mu); r = m.rate; } if (r > 0) m.setup(r); }
    if (m.fs1 <= 0 || n == 0) return;
    const double off = m.offsetSet.load();
    if (off != m.offset || m.nIn == 0) { m.offset = off; m.rotStep = std::polar(1.0, -kTwoPi * off / m.rate); }
    for (size_t i = 0; i < n; i++) {
        float re = x[i].real(), im = x[i].imag();
        if (!std::isfinite(re) || !std::isfinite(im)) re = im = 0;
        re = std::max(-2.f, std::min(2.f, re)); im = std::max(-2.f, std::min(2.f, im));
        m.power += (double)re * re + (double)im * im; m.nPower++;
        cd v = cd(re, im) * m.rot;
        m.rot *= m.rotStep;
        if ((++m.nIn & 4095) == 0) m.rot /= std::abs(m.rot);
        if (m.d1 > 1 && !m.fir0.push(v, v)) continue;
        cd y;
        if (m.fir1.push(v, y)) m.atFs1(y);
    }
    m.audio();
}

bool VorReceiver::telemetry(VorTelemetry& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (p_->pub.seq == lastSeq) return false;
    out = p_->pub;
    return true;
}
void VorReceiver::setLogCallback(std::function<void(const std::string&)> cb) { std::lock_guard<std::mutex> lk(p_->mu); p_->logCb = std::move(cb); }
void VorReceiver::setVolume(float v) { p_->volume = std::max(0.f, std::min(1.f, v)); }
void VorReceiver::setMuted(bool m) { p_->muted = m; }
void VorReceiver::setSilent(bool s) { p_->silent = s; }
void VorReceiver::setIdentFilter(bool on) { p_->identFilter = on; }
void VorReceiver::setAudioTap(std::function<void(const float*, size_t, double)> cb) { std::lock_guard<std::mutex> lk(p_->mu); p_->audioTapSet = std::move(cb); p_->tapReq = true; }

ModeTuning vorTuning() {
    ModeTuning t;
    t.stdMode = 32; t.id = "vorils"; t.name = "VOR / ILS";
    t.minMhz = 74; t.maxMhz = 336; t.defMhz = 113.000;
    t.sampleRate = 2000000;
    t.basebandHz = 1750000;
    t.bandwidthMhz = 0.05;                   // one 50 kHz channel
    t.minSampleRate = 100000;
    t.tuneOffsetHz = 250000;                 // the channel stays off the radio's DC spike
    return t;
}

} // namespace dect2
