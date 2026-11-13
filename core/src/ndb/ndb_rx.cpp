// NDB receiver (see ndb_rx.h).
#include "dect2/ndb/ndb_rx.h"
#include "dect2/cw/cw_morse.h"
#include "dect2/ndb/ndb_tel.h"
#include "dect2/ndb/ndb_census.h"
#include "../audio/ham_listen.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <complex>
#include <deque>
#include <mutex>
#include <vector>
#include "dect2/common/mathconst.h"

namespace dect2 {

namespace {
using dect2::math::kPi;
constexpr double kFs = 8000;          // working rate: the 400 Hz and 1020 Hz tones, and the audio
constexpr int kTones = 2;
constexpr double kToneHz[kTones] = {400, 1020};

using cd = std::complex<double>;

struct Biquad {
    double b0 = 0, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
    void bandpass(double f, double fs, double q) {
        const double w = 2 * kPi * f / fs;
        const double alpha = std::sin(w) / (2 * q);
        const double a0 = 1 + alpha;
        b0 = alpha / a0; b1 = 0; b2 = -alpha / a0;
        a1 = -2 * std::cos(w) / a0; a2 = (1 - alpha) / a0;
        z1 = z2 = 0;
    }
    void lowpass(double f, double fs, double q) {
        const double w = 2 * kPi * f / fs;
        const double alpha = std::sin(w) / (2 * q), c = std::cos(w);
        const double a0 = 1 + alpha;
        b0 = (1 - c) / 2 / a0; b1 = (1 - c) / a0; b2 = b0;
        a1 = -2 * c / a0; a2 = (1 - alpha) / a0;
        z1 = z2 = 0;
    }
    double run(double x) {
        const double y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
};

struct Smooth {
    double a = 0, y = 0;
    void set(double fc, double fs) { a = 1 - std::exp(-2 * kPi * fc / fs); y = 0; }
    double run(double x) { y += a * (x - y); return y; }
};

bool lettersOnly(const std::string& w) {
    if (w.empty() || w.size() > 4) return false;
    for (char c : w) if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) return false;
    return true;
}
} // namespace

struct NdbReceiver::Impl {
    std::mutex mu;
    std::function<void(const std::string&)> log;
    std::unique_ptr<NdbCensus> census;
    bool censusOff = false;
    double rateSet = 0, offsetSet = 0;
    std::atomic<bool> resetReq{true};
    std::atomic<int> toneMode{0};          // 0 auto, 1 400 Hz, 2 1020 Hz, 3 keyed carrier
    hfdig::Listen listen;

    double rate = 0, offset = 0;
    cd lo{1, 0}, loStep{1, 0};
    int64_t nIn = 0, loCount = 0;
    double accN = 0, need = 1;
    cd acc{0, 0};

    Biquad bp[kTones];
    Biquad preI[2], preQ[2];               // 4.0: the 8 kHz stream is limited to +-1.5 kHz before the envelope (the tones are at 400 and 1020 Hz)
    std::atomic<bool> weak{true};          // false: the 3.9 receiver (no pre-filter)
    Smooth envLp[kTones];
    double peak = 0;
    cd carrierLp{0, 0}, fLp{0, 0}, fPrev{0, 0};
    double fPh = 0, fEst = 0;
    double cmag = 0, carrPwr = 0;
    bool carrierOk() const { return !weak || carrPwr > kCarrierRatio * 0.0033 * pwr; }   // the carrier line must stand out of the noise in the 3 Hz estimate
    static constexpr double kCarrierRatio = 5.0;
    static constexpr float kMinKeySnrDb = 5.0f;
    Smooth cSlow;
    cd cLp{0, 0}, cPrev{0, 0};
    double cfo = 0;
    double pwr = 0;

    int path = -1;                         // 0 400 Hz, 1 1020 Hz, 2 keyed carrier; -1 not chosen
    bool key = false;
    int downMs = 0, upMs = 0;
    bool swallow = false;                  // a long dash is in progress: do not hand it to the Morse decoder
    bool longDash = false;
    int longDashHold = 0;
    int sub = 0;                           // 8 kHz samples toward the next millisecond
    cw::Timing timing;
    cw::Keyer keyer;
    std::string text, partial;
    std::string lastWord;
    int streak = 0;
    std::string ident;
    int repeats = 0;
    std::deque<std::string> hist;
    uint64_t okCount = 0, badCount = 0;

    std::vector<float> depthHist[3];       // recent depth of each path, 50 Hz
    // 4.0 ident folding: depth of each path at 50 Hz for up to 3 minutes, evaluated every 5 s
    std::atomic<bool> foldOn{true};
    static constexpr size_t kFoldMax = 9000;
    std::deque<float> foldHist[3];
    int foldTick = 0;
    std::string foldLast;                  // the word the previous evaluation read (two evaluations in a row must agree)
    uint64_t foldReads = 0;
    int histTick = 0;
    float depth = 0, snr = 0;
    double nextReport = 0, tNow = 0;
    std::vector<float> aud;
    NdbTelemetry pub;

    void armFilters() {
        // Q = 4 at 400 Hz is about 100 Hz wide (400 ± 25, plus a radio that is a little off).
        // Q = 6 at 1020 Hz is about 170 Hz wide (1020 ± 50).
        bp[0].bandpass(400, kFs, 4);
        bp[1].bandpass(1020, kFs, 6);
        for (int i = 0; i < kTones; i++) envLp[i].set(20, kFs);
        cSlow.set(20, kFs);
        for (int i = 0; i < 2; i++) { preI[i].lowpass(1500, kFs, 0.7071); preQ[i].lowpass(1500, kFs, 0.7071); }
        peak = 0; carrierLp = 0; cmag = 0; carrPwr = 0; fLp = 0; fPrev = 0; fPh = 0; fEst = 0;
        for (auto& h : depthHist) h.clear();
        for (auto& h : foldHist) h.clear();
        foldTick = 0; foldLast.clear();
    }

    void resetDemod() {
        lo = 1; loCount = 0;
        loStep = rate > 0 ? std::polar(1.0, -2 * kPi * offset / rate) : cd(1, 0);
        accN = 0; acc = 0; need = rate > 0 ? std::max(1.0, rate / kFs) : 1;
        armFilters();
        pwr = 0; cfo = 0; cLp = 0; cPrev = 0;
        path = -1; key = false; downMs = upMs = 0; swallow = false; longDash = false; longDashHold = 0; sub = 0;
        timing.reset(); keyer.reset();
        text.clear(); partial.clear(); lastWord.clear(); streak = 0;
        depth = 0; snr = 0; tNow = 0; nextReport = 0.25;
        aud.clear();
        // the confirmed ident and the history stay: a retune does not wipe what was already read
    }

    // Reads the ident from the average of the last ident periods of one path (50 Hz samples). Returns the word or "".
    struct FoldInfo { double period = 0, z = 0, qual = 0; std::string text; };
    FoldInfo lastFold;
    static std::string foldRead(const std::deque<float>& h, FoldInfo* info) {
        FoldInfo dummy;
        FoldInfo& fi = info ? *info : dummy;
        constexpr double kRate = 50.0;
        const int n = (int)h.size();
        const int lMin = (int)(2.0 * kRate), lMax = std::min((int)(30.0 * kRate), n / 3);
        if (lMax <= lMin + 2) return "";
        std::vector<double> x(h.begin(), h.end());
        double mean = 0;
        for (double v : x) mean += v;
        mean /= n;
        for (double& v : x) v -= mean;
        double r0 = 0;
        for (double v : x) r0 += v * v;
        r0 /= n;
        if (r0 <= 1e-20) return "";
        std::vector<double> r((size_t)lMax + 2, 0.0);
        for (int L = lMin - 1; L <= lMax + 1; L++) {
            double acc = 0;
            for (int i = 0; i + L < n; i++) acc += x[(size_t)i] * x[(size_t)(i + L)];
            r[(size_t)L] = acc / (n - L) / r0;
        }
        int top = lMin;
        for (int L = lMin; L <= lMax; L++) if (r[(size_t)L] > r[(size_t)top]) top = L;
        // the ident period, not a multiple of it: the shortest lag that is a local peak within 80 % of the highest (2 and 3 periods correlate as well)
        int best = top;
        for (int L = lMin; L < top; L++)
            if (r[(size_t)L] >= 0.8 * r[(size_t)top] && r[(size_t)L] >= r[(size_t)L - 1] && r[(size_t)L] >= r[(size_t)L + 1]) { best = L; break; }
        // the period must stand out of the autocorrelation at the other lags (robust z-score: median and median absolute deviation)
        std::vector<double> rv(r.begin() + lMin, r.begin() + lMax + 1);
        std::vector<double> srt = rv;
        std::nth_element(srt.begin(), srt.begin() + (long)(srt.size() / 2), srt.end());
        const double med = srt[srt.size() / 2];
        for (double& v : srt) v = std::fabs(v - med);
        std::nth_element(srt.begin(), srt.begin() + (long)(srt.size() / 2), srt.end());
        const double mad = std::max(srt[srt.size() / 2], 1e-6);
        const double z = (r[(size_t)best] - med) / (1.4826 * mad);
        fi.z = z; fi.period = best / kRate;
        // a periodic keying correlates at many lags, which widens the spread the z-score is taken against (measured 3.4 to 5.8 for a real ident at 4 to 7 dB):
        // the period only has to stand out moderately; the average's contrast, the Morse reading of two copies and two evaluations in a row do the rest
        if (z < 3.0 || r[(size_t)best] < 0.1) return "";
        // fractional period (parabola through the peak); the fold must not drift by a dot over 20 periods
        const double ra = r[(size_t)best - 1], rb = r[(size_t)best], rc = r[(size_t)best + 1];
        const double den = ra - 2 * rb + rc;
        const double P = best + (std::fabs(den) > 1e-12 ? std::max(-0.5, std::min(0.5, 0.5 * (ra - rc) / den)) : 0.0);
        const int K = (int)std::floor((double)n / P);
        if (K < 3) return "";
        const int len = (int)std::floor(P);
        std::vector<double> f((size_t)len, 0.0), f2((size_t)len, 0.0);
        for (int k = 0; k < K; k++) {
            const double base = (double)n - (double)(K - k) * P;   // the K most recent periods
            for (int j = 0; j < len; j++) {
                const double pos = base + j;
                const int i0 = (int)std::floor(pos);
                const double fr = pos - i0;
                const double v = i0 + 1 < n && i0 >= 0 ? x[(size_t)i0] * (1 - fr) + x[(size_t)(i0 + 1)] * fr : 0.0;
                f[(size_t)j] += v; f2[(size_t)j] += v * v;
            }
        }
        double resid = 0;
        for (int j = 0; j < len; j++) { f[(size_t)j] /= K; resid += f2[(size_t)j] / K - f[(size_t)j] * f[(size_t)j]; }
        const double sigmaAvg = std::sqrt(std::max(resid / len, 1e-30) / std::max(1, K - 1));   // noise of the average
        std::vector<double> sf = f;
        std::sort(sf.begin(), sf.end());
        const double p10 = sf[sf.size() / 10], p90 = sf[sf.size() * 9 / 10];
        const double qual = (p90 - p10) / sigmaAvg;
        fi.qual = qual; fi.period = P / kRate;
        if (qual < 6.0) return "";
        // key up / down runs of the average, starting after the longest key-up (the gap between idents), read three times over
        const double thr = 0.5 * (p10 + p90);
        std::vector<char> on((size_t)len);
        for (int j = 0; j < len; j++) on[(size_t)j] = f[(size_t)j] > thr;
        int startJ = 0, bestRun = 0;
        for (int j = 0; j < len; j++) {
            if (on[(size_t)j]) continue;
            int run = 0;
            while (run < len && !on[(size_t)((j + run) % len)]) run++;
            if (run > bestRun) { bestRun = run; startJ = (j + run) % len; }
        }
        if (bestRun == 0 || bestRun >= len) return "";
        cw::Timing tm;
        tm.reset();
        for (int rep = 0; rep < 3; rep++) {
            int j = 0;
            while (j < len) {
                const bool v = on[(size_t)((startJ + j) % len)] != 0;
                int run = 0;
                while (j + run < len && (on[(size_t)((startJ + j + run) % len)] != 0) == v) run++;
                const double ms = run * 1000.0 / kRate;
                if (v) tm.mark(ms); else { tm.idle(ms); tm.space(ms); }
                j += run;
            }
        }
        tm.idle(1e6);
        const std::string txt = tm.take();
        fi.text = txt;
        // the ident is the word read in each of the later copies (the first copy can be lost while the dot length settles)
        std::vector<std::string> words;
        std::string w;
        for (char c : txt) { if (c == ' ') { if (!w.empty()) words.push_back(w); w.clear(); } else w.push_back(c); }
        if (!w.empty()) words.push_back(w);
        if (words.size() < 2) return "";
        const std::string& last = words.back();
        if (!lettersOnly(last) || words[words.size() - 2] != last) return "";
        return last;
    }

    void foldEval() {
        if (!weak || !foldOn || !carrierOk()) { foldLast.clear(); return; }
        const int force = forcedPath();
        std::string best;
        double bestQ = 0;
        FoldInfo shown;
        for (int i = 0; i < 3; i++) {
            if (force >= 0 && i != force) continue;
            FoldInfo fi;
            const std::string w = foldRead(foldHist[i], &fi);
            if (!w.empty() && fi.qual > bestQ) { bestQ = fi.qual; best = w; }
            if (fi.z > shown.z) shown = fi;
        }
        lastFold = shown;
        if (!best.empty() && best == foldLast) {
            foldReads++;
            if (best == ident) { repeats++; okCount++; }
            else {
                ident = best;
                repeats = 2;
                okCount++;
                hist.push_back(best);
                while (hist.size() > 24) hist.pop_front();
                if (log) log("NDB ident " + best + " (from the average of repeated idents)");
            }
        }
        foldLast = best;
    }

    void confirm(const std::string& w) {
        if (!lettersOnly(w)) return;
        if (weak && snr < kMinKeySnrDb) return;   // 4.0: the weak path reads keying down to a few dB, where noise makes 'E' and 'T' words: a word counts only with a clear on / off contrast
        if (w == ident) { repeats++; okCount++; return; }
        ident = w;
        repeats = w.size() == 1 ? 3 : 2;
        okCount++;
        hist.push_back(w);
        while (hist.size() > 24) hist.pop_front();
        if (log) log("NDB ident " + w);
    }

    void onWord(const std::string& w) {
        if (!lettersOnly(w)) { lastWord.clear(); streak = 0; return; }
        if (w == lastWord) {
            streak++;
            const int needN = w.size() == 1 ? (weak ? 5 : 3) : (weak && w.size() == 2) ? 3 : 2;
            if (streak >= needN) confirm(w);
        } else {
            lastWord = w;
            streak = 1;
        }
    }

    void takeText(const std::string& add) {
        if (add.empty()) return;
        text += add;
        if (text.size() > 180) text.erase(0, text.size() - 180);
        partial += add;
        std::string word;
        for (char c : partial) {
            if (c == ' ') {
                if (!word.empty()) onWord(word);
                word.clear();
            } else if (c != '*') word.push_back(c);
        }
        partial = word;
        if (partial.size() > 8) partial.clear();
        // no word space yet (the gap has not closed): a doubled ident "NDBNDB" still counts
        std::string letters;
        for (char c : text) if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) letters.push_back(c);
        if (letters.size() > 24) letters.erase(0, letters.size() - 24);
        for (int n = 2; n <= 4; n++) {
            if ((int)letters.size() < 2 * n) continue;
            const std::string a = letters.substr(letters.size() - 2 * (size_t)n, (size_t)n);
            const std::string b = letters.substr(letters.size() - (size_t)n);
            if (a == b) confirm(a);
        }
        if (letters.size() >= 3) {
            const char c = letters.back();
            if (letters[letters.size() - 2] == c && letters[letters.size() - 3] == c) confirm(std::string(1, c));
        }
    }

    int forcedPath() const {
        const int m = toneMode.load();
        if (m == 1) return 0;
        if (m == 2) return 1;
        if (m == 3) return 2;
        return -1;
    }

    void choosePath() {
        const int force = forcedPath();
        if (force >= 0) { path = force; return; }
        float best = 0, cur = 0;
        int bi = -1;
        for (int i = 0; i < 3; i++) {
            const auto& h = depthHist[i];
            if (h.size() < 20) continue;
            std::vector<float> s(h.begin(), h.end());   // 2 s: a 0.4 s window inside a dash shows no contrast
            std::sort(s.begin(), s.end());
            const float p10 = s[s.size() / 10], p90 = s[s.size() * 9 / 10];
            const float contrast = p90 - p10;
            if (i == path) cur = contrast;
            if (contrast > best) { best = contrast; bi = i; }
        }
        if (bi < 0 || best < 0.06f) return;
        // 4.0: the synchronous tone depths of a keyed-carrier station flicker; leaving the keyed carrier for a tone needs a clear margin
        if (path < 0 || best > cur * (weak && path == 2 && bi != 2 ? 2.5f : 1.3f)) path = bi;
    }

    void atMilli(bool raw) {
        // A mark still down after 0.9 s is the long dash some beacons send between idents (a Morse dash at 7 WPM is about
        // half a second). Drop it: feeding it through would read as T and would pull the dot length.
        if (swallow) {
            if (!raw) swallow = false;
            keyer.sample(false, timing);
        } else if (raw && downMs >= 900) {
            keyer.reset();
            swallow = true;
            longDash = true;
            longDashHold = 2500;
            keyer.sample(false, timing);
        } else keyer.sample(raw, timing);
        if (raw) { downMs++; upMs = 0; }
        else {
            downMs = 0; upMs++;
            if (longDashHold > 0 && --longDashHold == 0) longDash = false;
        }
        takeText(timing.take());
        badCount = timing.unknown();
    }

    void at8k(cd z) {
        if (weak) z = cd(preI[1].run(preI[0].run(z.real())), preQ[1].run(preQ[0].run(z.imag())));
        tNow += 1.0 / kFs;
        const double env = std::abs(z);
        const double decay = std::exp(-1.0 / (kFs * 5.0));
        if (env > peak) peak += (env - peak) * 0.3;
        else peak *= decay;
        const double ref = std::max(peak, 1e-9);
        pwr += (std::norm(z) - pwr) * 0.002;

        double toneAud[kTones];
        double depths[3];
        // 4.0: the tones are read from the carrier-synchronous component (the in-phase part against a 3 Hz carrier estimate), which has no squaring loss
        // against noise, and normalised by the carrier amplitude instead of the noise-driven peak of the envelope
        double toneIn = env, tref = ref;
        if (weak) {
            // a frequency loop keeps the carrier at 0 Hz (a radio error of tens of Hz would walk out of the 3 Hz estimate): a 30 Hz average of the
            // de-rotated stream gives the residual frequency from its phase steps
            fPh = std::remainder(fPh + 2 * kPi * fEst / kFs, 2 * kPi);
            const cd zr = z * cd(std::cos(-fPh), std::sin(-fPh));
            fLp += (zr - fLp) * (1 - std::exp(-2 * kPi * 30.0 / kFs));
            if (std::abs(fLp) > 1e-12 && std::abs(fPrev) > 1e-12) {
                const double inst = std::arg(fLp * std::conj(fPrev)) * kFs / (2 * kPi);
                fEst += std::max(-5.0, std::min(5.0, inst)) * 0.002;
                fEst = std::max(-200.0, std::min(200.0, fEst));
            }
            fPrev = fLp;
            carrierLp += (zr - carrierLp) * (1 - std::exp(-2 * kPi * 3.0 / kFs));
            const double cm = std::abs(carrierLp);
            cmag += (cm - cmag) * 0.001;   // slow, so that a keyed carrier's off periods do not turn the noise into a unit-size signal
            if (cmag > 1e-12) toneIn = (zr * std::conj(carrierLp)).real() / std::max(std::max(cm, cmag), 1e-12);
            carrPwr += (cm * cm - carrPwr) * 0.0005;
            tref = std::max(cmag, 1e-9);
        }
        for (int i = 0; i < kTones; i++) {
            toneAud[i] = bp[i].run(toneIn);
            depths[i] = envLp[i].run(std::fabs(toneAud[i])) / (weak ? tref : ref);
        }
        const double fast = cSlow.run(env);     // 20 Hz: follows the carrier keying, not the 400 Hz ripple
        depths[2] = fast / ref;

        if (++histTick >= 160) {               // 50 Hz
            histTick = 0;
            for (int i = 0; i < 3; i++) {
                depthHist[i].push_back((float)depths[i]);
                if (depthHist[i].size() > 100) depthHist[i].erase(depthHist[i].begin());
            }
            choosePath();
            if (weak && foldOn) {
                for (int i = 0; i < 3; i++) {
                    foldHist[i].push_back((float)depths[i]);
                    if (foldHist[i].size() > kFoldMax) foldHist[i].pop_front();
                }
                if (++foldTick >= 250) { foldTick = 0; foldEval(); }
            }
        }

        const int use = path;
        double metric = use >= 0 && use < 3 ? depths[use] : 0;
        bool present = false;
        float p10 = 0, p90 = 0;
        if (use >= 0 && depthHist[use].size() >= 20) {
            // the whole 2 s history: a window of 0.4 s holds only key-down inside a dash (about 0.5 s at 7 WPM), p10 meets p90 and the
            // dash is cut short, so it reads as a dot
            std::vector<float> s(depthHist[use].begin(), depthHist[use].end());
            std::sort(s.begin(), s.end());
            p10 = s[s.size() / 10]; p90 = s[s.size() * 9 / 10];
            present = p90 > 0.05f && p90 > 1.8f * std::max(p10, 1e-4f) && carrierOk();
            const float thr = 0.5f * (p10 + p90);
            if (!present) key = false;
            else if (key) key = metric > thr * 0.85;
            else key = metric > thr * 1.15;
            depth = p90;
            const float lo = std::max(p10, 1e-4f);
            snr = (float)(10.0 * std::log10(p90 / lo));
        } else key = false;

        const double cA = 1 - std::exp(-2 * kPi * 30 / kFs);
        cLp += (z - cLp) * cA;
        if (std::abs(cLp) > 0.25 * ref) {
            const double inst = std::arg(cLp * std::conj(cPrev)) * kFs / (2 * kPi);
            cPrev = cLp;
            if (std::fabs(inst) < 2000) cfo += (inst - cfo) * 0.02;
        }

        if (++sub >= 8) { sub = 0; atMilli(present && key); }

        int play = use;
        if (play < 0) play = depths[0] >= depths[1] ? 0 : 1;
        float sample = 0;
        if (play == 2 || (play < 0 && depths[2] > depths[0] && depths[2] > depths[1])) {
            tonePh_ = std::remainder(tonePh_ + 2 * kPi * 400 / kFs, 2 * kPi);
            sample = (present && key) ? (float)(0.35 * std::sin(tonePh_)) : 0;
        } else if (play >= 0 && play < kTones) {
            sample = (float)(2.5 * toneAud[play] / ref);
            if (sample > 0.9f) sample = 0.9f;
            if (sample < -0.9f) sample = -0.9f;
        }
        aud.push_back(sample);
    }

    double tonePh_ = 0;

    void publish() {
        NdbTelemetry t;
        t.seq = pub.seq + 1;
        if (census) census->snapshot(t.census);
        const double level = 10.0 * std::log10(pwr + 1e-20);
        t.levelDb = (float)level;
        t.cfoHz = cfo;
        t.snrDb = snr;
        t.depth = depth;
        t.wpm = (float)timing.wpm();
        t.key = key;
        t.longDash = longDash;
        t.ident = ident;
        t.text = text;
        t.partial = partial;
        t.history.assign(hist.begin(), hist.end());
        t.blocksOk = okCount;
        t.blocksBad = badCount;
        t.foldPeriodSec = (float)lastFold.period; t.foldZ = (float)lastFold.z; t.foldQuality = (float)lastFold.qual; t.foldText = lastFold.text;
        t.dataValid = !ident.empty();
        if (path == 0) t.toneHz = 400;
        else if (path == 1) t.toneHz = 1020;
        else if (path == 2) t.toneHz = -1;
        const bool carrier = level > -80.0 && peak > 1e-5;
        t.state = !carrier ? 0 : ident.empty() ? 1 : 2;
        pub = std::move(t);
    }
};

NdbReceiver::NdbReceiver() : p_(std::make_unique<Impl>()) {}
NdbReceiver::~NdbReceiver() = default;

void NdbReceiver::configure(double inputRateHz) {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->rateSet = inputRateHz;
    p_->resetReq = true;
}
void NdbReceiver::setSignalOffset(double hz) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (hz != p_->offsetSet) { p_->offsetSet = hz; p_->resetReq = true; }
}
bool NdbReceiver::ready() const {
    std::lock_guard<std::mutex> lk(p_->mu);
    return p_->rateSet >= ndbTuning().minSampleRate - 1;
}
void NdbReceiver::reset() {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->resetReq = true;
}
void NdbReceiver::feed(const cf32* x, size_t n) {
    std::lock_guard<std::mutex> lk(p_->mu);
    Impl& m = *p_;
    if (m.resetReq.exchange(false)) {
        m.rate = m.rateSet;
        m.offset = m.offsetSet;
        m.nIn = 0;
        m.resetDemod();
        m.census.reset();
        m.censusOff = m.listen.silent;    // the channels of the census run silent: they must not start their own census
    }
    if (m.rate < ndbTuning().minSampleRate - 1 || n == 0) return;
    if (!m.censusOff) {
        if (!m.census) { m.census = std::make_unique<NdbCensus>(); m.census->configure(m.rate, m.offset); }
        m.census->feed(x, n);
    }
    m.aud.clear();
    for (size_t i = 0; i < n; i++) {
        m.lo *= m.loStep;
        if ((++m.loCount & 1023) == 0) m.lo /= std::abs(m.lo);
        const cd z = cd((double)x[i].real(), (double)x[i].imag()) * m.lo;
        m.acc += z;
        m.accN += 1;
        if (m.accN + 1e-6 >= m.need) {
            m.at8k(m.acc / m.accN);
            m.acc = 0;
            m.accN = 0;
        }
        m.nIn++;
    }
    if (m.tNow >= m.nextReport) { m.nextReport += 0.25; m.publish(); }
    if (!m.aud.empty()) m.listen.play(m.aud.data(), m.aud.size());
}
bool NdbReceiver::telemetry(NdbTelemetry& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (p_->pub.seq <= lastSeq) return false;
    out = p_->pub;
    return true;
}
void NdbReceiver::clearResults() {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->ident.clear();
    p_->hist.clear();
    p_->text.clear();
    p_->partial.clear();
    p_->lastWord.clear();
    p_->streak = 0;
    p_->repeats = 0;
    p_->okCount = 0;
    if (p_->census) p_->census->clear();
    p_->pub.ident.clear();
    p_->pub.history.clear();
    p_->pub.text.clear();
    p_->pub.dataValid = false;
    p_->pub.state = p_->pub.state == 2 ? 1 : p_->pub.state;
}
std::string NdbReceiver::saveResults() const {
    std::lock_guard<std::mutex> lk(p_->mu);
    std::string s = "ndb1\n";
    for (const auto& h : p_->hist) { s += h; s += '\n'; }
    return s;
}
bool NdbReceiver::loadResults(const std::string& blob) {
    if (blob.compare(0, 5, "ndb1\n") != 0) return false;
    std::deque<std::string> h;
    size_t i = 5;
    while (i < blob.size() && h.size() < 24) {
        size_t e = blob.find('\n', i);
        if (e == std::string::npos) e = blob.size();
        std::string w = blob.substr(i, e - i);
        if (lettersOnly(w)) h.push_back(w);
        i = e + 1;
    }
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->hist = std::move(h);
    if (!p_->hist.empty()) p_->ident = p_->hist.back();
    return true;
}
void NdbReceiver::setLogCallback(std::function<void(const std::string&)> cb) {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->log = std::move(cb);
}
void NdbReceiver::setWeakSignalPath(bool on) { p_->weak = on; }
void NdbReceiver::setIdentFolding(bool on) { p_->foldOn = on; }
void NdbReceiver::setToneMode(int mode) { p_->toneMode = mode < 0 || mode > 3 ? 0 : mode; }
void NdbReceiver::setListen(bool on) { p_->listen.on = on; }
bool NdbReceiver::listening() const { return p_->listen.on.load(); }
void NdbReceiver::setVolume(float v) { p_->listen.volume = std::max(0.f, std::min(1.f, v)); }
void NdbReceiver::setMuted(bool m) { p_->listen.muted = m; }
void NdbReceiver::setSilent(bool s) { p_->listen.silent = s; }
void NdbReceiver::setListenTap(std::function<void(const float* x, size_t n)> cb) { p_->listen.setTap(std::move(cb)); }

ModeTuning ndbTuning() {
    ModeTuning t;
    t.stdMode = 31; t.id = "ndb"; t.name = "NDB";
    t.minMhz = 0.190; t.maxMhz = 0.535; t.defMhz = 0.350;   // middle of the long-wave beacon band; modeTuning() opens the range
    t.sampleRate = 256000;                   // a rate LF radios offer (RTL-SDR direct sampling, Airspy HF+, SDRplay), as the time-signal mode
    t.basebandHz = 0;
    t.bandwidthMhz = 0.01;
    t.minSampleRate = 48000;
    t.tuneOffsetHz = 12000;                  // the radio sits 12 kHz above the carrier, away from its DC spike
    return t;
}

} // namespace dect2
