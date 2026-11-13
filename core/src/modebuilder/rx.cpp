// Mode Builder receiver: the pipeline a definition describes, built at configure(). Mixer and decimating channel filter, demodulator
// (envelope, frequency discriminator or carrier loop), matched filter, Gardner symbol clock with cubic interpolation, adaptive level
// slicer (FSK / ASK) or decision-directed PSK carrier loop, then the frame engine: every polarity / PSK rotation and line-code phase is
// searched for the sync word, and the frame after it is decoded (frames.cpp). Streams: feed() any number of samples, frames come out
// of take(); flush() at the end of a recording.
#include "dect2/modebuilder/modebuilder.h"
#include <algorithm>
#include <cmath>
#include <deque>
#include <limits>
#include <mutex>

namespace dect2::modebuilder {

namespace {
constexpr double kPi = 3.14159265358979323846;
double wrap(double a) { while (a > kPi) a -= 2 * kPi; while (a < -kPi) a += 2 * kPi; return a; }
}   // namespace

struct Receiver::Impl {
    Definition d;
    double fsIn = 0, extraOff = 0;
    int M = 2, bps = 1;
    bool fsk = false, psk = false, envelope = false;
    double Rs = 1, dev = 1, bw = 1;
    // mixer
    cf rot{1, 0}, rotStep{1, 0};
    int rotCount = 0;
    // decimator
    int D = 1;
    std::vector<float> taps;
    std::vector<cf> hist;
    size_t hpos = 0;
    int decCount = 0;
    double fs1 = 0, sps = 1;
    // demodulator and matched filter
    cf prevZ{0, 0};
    int mfLen = 1;
    std::vector<cf> mfRing;
    size_t mfPos = 0;
    cf mfSum{0, 0};
    std::vector<float> rrc;          // PSK / ASK with rrc pulses: the matched filter
    std::vector<cf> rrcHist;
    size_t rrcPos = 0;
    double pw = 0, apw = 0.1, pwS = 0, apwS = 0.1;
    double noise = -1, riseS = 1, thrLin = 4, hangS = 0, hangLeft = 0;
    bool open = false, wasOpen = false, acquiring = false;
    long long acqStart = 0, mfDelay = 0;
    double acqLen = 64, acqPw = 1e300, warmup = 0, peakDecay = 1;
    // timing
    std::deque<cf> buf;
    long long bufBase = 0, n1 = 0;   // absolute index of buf[0]; samples at fs1 so far
    double tNext = 4, loopInt = 0;
    cf yPrev{0, 0};
    int sinceOpen = 0;
    // slicer
    double lo = -1, hi = 1;
    // PSK
    double cph = 0, cfreq = 0, agc = 1;
    int prevIdx = 0;
    // symbols
    std::vector<uint8_t> S, Sopen;
    std::vector<float> Ssnr;
    std::vector<double> Tpos;
    long long symBase = 0, searchFrom = 0;
    long long inCount = 0;
    // frame engine
    int needSyms = 0, syncSyms = 0, chunk = 256;
    std::vector<int> valueOf;        // level -> bit value
    std::mutex mu;
    std::vector<Frame> out;
    RxStats st;
    // the Signal view: the symbol-timed values of the last kSigCap symbols
    static constexpr size_t kSigCap = 2048;
    mutable std::mutex sigMu;
    std::vector<float> sigR;         // value at the strobe (FSK: in deviations; envelope: amplitude)
    std::vector<uint8_t> sigLvl;
    std::vector<cf> sigZ;            // PSK point after carrier recovery
    size_t sigPos = 0, sigCnt = 0;
    double sigLo = -1, sigHi = 1, sigCfreq = 0;

    explicit Impl(const Definition& def) : d(def) {}

    void configure(double rate, double extra) {
        fsIn = rate > 0 ? rate : d.sampleRate;
        extraOff = extra;
        M = d.levels();
        bps = d.bitsPerSymbol();
        fsk = d.mod == Mod::FSK || d.mod == Mod::GFSK || d.mod == Mod::GMSK;
        psk = d.mod == Mod::PSK;
        envelope = d.mod == Mod::OOK || d.mod == Mod::ASK;
        Rs = std::max(1.0, d.symbolRate);
        dev = std::max(1.0, d.deviation());
        bw = std::min(d.bandwidth(), fsIn * 0.98);
        const double off = d.offsetHz + extraOff;
        const double w = -2 * kPi * off / fsIn;
        rotStep = cf((float)std::cos(w), (float)std::sin(w));
        rot = cf(1, 0);
        rotCount = 0;
        const double target = std::max(8 * Rs, 1.6 * bw);
        D = std::max(1, (int)std::floor(fsIn / target));
        fs1 = fsIn / D;
        sps = fs1 / Rs;
        // channel filter: windowed sinc, flat to bw/2
        const double tw = std::max(bw * 0.25, fsIn * 0.002);
        const double fc = std::min(0.5, (bw / 2 + tw / 2) / fsIn);
        int N = (int)std::ceil(3.3 * fsIn / tw) | 1;
        N = std::clamp(N, 1, 4095);
        if (fc >= 0.49 && D == 1) N = 1;
        taps.assign((size_t)N, 0.f);
        double sum = 0;
        for (int i = 0; i < N; i++) {
            const double x = i - (N - 1) / 2.0;
            const double s = std::fabs(x) < 1e-9 ? 2 * fc : std::sin(2 * kPi * fc * x) / (kPi * x);
            const double win = N > 1 ? 0.54 - 0.46 * std::cos(2 * kPi * i / (N - 1)) : 1;
            taps[(size_t)i] = (float)(s * win);
            sum += s * win;
        }
        for (float& t : taps) t = (float)(t / sum);
        hist.assign((size_t)N * 2, cf(0, 0));
        hpos = 0;
        decCount = 0;
        // matched filter
        const Pulse pulse = d.pulseShape();
        rrc.clear();
        if ((psk || envelope) && pulse == Pulse::Rrc) {
            const int span = 6;
            const int L = std::max(3, (int)std::lround(span * 2 * sps)) | 1;
            double s2 = 0;
            for (int i = 0; i < L; i++) {
                const double t = (i - (L - 1) / 2.0) / sps;
                double h;
                const double b = d.rolloff;
                if (std::fabs(t) < 1e-9) h = 1 - b + 4 * b / kPi;
                else if (std::fabs(std::fabs(t) - 1 / (4 * b)) < 1e-6) h = b / std::sqrt(2.0) * ((1 + 2 / kPi) * std::sin(kPi / (4 * b)) + (1 - 2 / kPi) * std::cos(kPi / (4 * b)));
                else h = (std::sin(kPi * t * (1 - b)) + 4 * b * t * std::cos(kPi * t * (1 + b))) / (kPi * t * (1 - 16 * b * b * t * t));
                rrc.push_back((float)h);
                s2 += h;
            }
            for (float& h : rrc) h = (float)(h / s2);
            rrcHist.assign(rrc.size() * 2, cf(0, 0));
            rrcPos = 0;
            mfLen = 1;
        } else if (pulse == Pulse::Rc) mfLen = 1;
        else mfLen = std::max(1, (int)std::lround(sps * (fsk && pulse == Pulse::Gaussian ? 0.8 : 1.0)));
        mfRing.assign((size_t)mfLen, cf(0, 0));
        mfPos = 0;
        mfSum = cf(0, 0);
        prevZ = cf(0, 0);
        pw = 0;
        apw = std::min(1.0, 1.0 / sps);
        apwS = 1.0 / std::max(16.0, 2 * sps);
        pwS = 0;
        noise = -1;
        riseS = std::pow(1.0001, 1.0 / sps);
        thrLin = std::pow(10.0, d.squelchDb / 10);
        hangS = 48 * sps;
        hangLeft = 0;
        acquiring = false;
        acqStart = 0;
        acqLen = std::max(24.0, 12 * sps);
        mfDelay = rrc.empty() ? (long long)(mfLen - 1) / 2 : (long long)(rrc.size() - 1) / 2;   // the burst reaches the strobes this much later
        acqPw = 1e300;
        peakDecay = 1.0 / (16 * sps);
        warmup = 4 * sps + (double)(rrc.size() + (size_t)mfLen);
        open = wasOpen = false;
        buf.clear();
        bufBase = 0;
        n1 = 0;
        tNext = 4;
        loopInt = 0;
        yPrev = cf(0, 0);
        sinceOpen = 0;
        lo = -1; hi = 1;
        cph = 0; cfreq = 0; agc = 1; prevIdx = 0;
        S.clear(); Sopen.clear(); Ssnr.clear(); Tpos.clear();
        {
            std::lock_guard<std::mutex> sl(sigMu);
            sigR.assign(kSigCap, 0.f); sigLvl.assign(kSigCap, 0); sigZ.assign(kSigCap, cf(0, 0));
            sigPos = sigCnt = 0; sigLo = -1; sigHi = 1; sigCfreq = 0;
        }
        symBase = 0; searchFrom = 0; inCount = 0;
        valueOf.assign((size_t)M, 0);
        for (int L = 0; L < M; L++) valueOf[(size_t)L] = !d.symbolMap.empty() ? d.symbolMap[(size_t)L] : (d.gray && M > 2) ? (L ^ (L >> 1)) : L;
        // what the frame engine must see after a sync word before it can decode the longest frame
        size_t needBits;
        if (d.lenMode == LenMode::EndFlag) needBits = (size_t)d.maxBytes * 8 + (d.bitStuffing ? (size_t)d.maxBytes * 8 / 5 + 1 : 0) + (size_t)d.flag.bits * 9;
        else needBits = stage::codedBits(d, (size_t)d.longestFrameBytes() * 8);
        const double exp = stage::lineExpansion(d.line);
        needSyms = (int)std::ceil(needBits * exp / bps) + 12;
        syncSyms = (int)std::ceil(d.sync.bits * exp / bps) + 2;
        chunk = std::max(256, needSyms);
        std::lock_guard<std::mutex> lk(mu);
        out.clear();
        st = RxStats{};
        st.fs1 = fs1;
        st.symbolRateHz = Rs;
    }


    SignalView view() const {
        SignalView v;
        v.mod = d.mod;
        v.levels = M;
        std::lock_guard<std::mutex> sl(sigMu);
        v.samples = sigCnt;
        if (sigCnt < 64) return v;
        v.have = true;
        const size_t n = sigCnt, first = (sigPos + kSigCap - n) % kSigCap;
        if (psk) {
            // least-squares gain onto the ideal points: the displayed constellation and the EVM do not depend on the AGC's level
            double num = 0;
            for (size_t i = 0; i < n; i++) {
                const size_t k = (first + i) % kSigCap;
                const double ang = 2 * kPi * sigLvl[k] / M;
                num += sigZ[k].real() * std::cos(ang) + sigZ[k].imag() * std::sin(ang);
            }
            const double gain = num > 1e-9 ? (double)n / num : 1.0;
            double e2 = 0, p2 = 0;
            for (size_t i = 0; i < n; i++) {
                const size_t k = (first + i) % kSigCap;
                const double ang = 2 * kPi * sigLvl[k] / M;
                const double pe = wrap(std::atan2((double)sigZ[k].imag(), (double)sigZ[k].real()) - ang);
                p2 += pe * pe;
                const double dx = gain * sigZ[k].real() - std::cos(ang), dy = gain * sigZ[k].imag() - std::sin(ang);
                e2 += dx * dx + dy * dy;
            }
            const double evm = std::sqrt(e2 / (double)n);
            v.evmPct = 100 * evm;
            // the decision is made on the phase: separation = angle between neighbouring points / rms phase error (the amplitude scatter
            // is in the EVM but does not move a PSK decision)
            const double sa = std::max(1e-6, std::sqrt(p2 / (double)n));
            v.separation = std::min(99.0, (2 * kPi / M) / sa);
            v.snrDb = -20 * std::log10(std::max(evm, 1e-6));
            v.hasEbn0 = true;
            v.ebn0Db = v.snrDb - 10 * std::log10((double)bps);
            v.offsetHz = sigCfreq * Rs / (2 * kPi);
            v.resolved = v.separation >= 4;
            v.clusters = v.resolved ? M : 0;
            const size_t keep = std::min<size_t>(n, 512);
            for (size_t i = n - keep; i < n; i++) v.points.push_back(sigZ[(first + i) % kSigCap] * (float)gain);
            return v;
        }
        const double sc = fsk ? dev : 1.0;
        v.unit = fsk ? "Hz" : "";
        std::vector<double> x(n);
        std::vector<double> sum((size_t)M, 0.0), sum2((size_t)M, 0.0);
        std::vector<size_t> cnt((size_t)M, 0);
        for (size_t i = 0; i < n; i++) {
            const size_t k = (first + i) % kSigCap;
            x[i] = sigR[k] * sc;
            const size_t L = std::min<size_t>(sigLvl[k], (size_t)M - 1);
            sum[L] += x[i]; sum2[L] += x[i] * x[i]; cnt[L]++;
        }
        std::vector<double> srt = x;
        std::sort(srt.begin(), srt.end());
        double lo = srt[(size_t)((double)(n - 1) * 0.005)], hi = srt[(size_t)((double)(n - 1) * 0.995)];
        for (int k = 0; k < M; k++) if (cnt[(size_t)k]) { const double c = sum[(size_t)k] / (double)cnt[(size_t)k]; lo = std::min(lo, c); hi = std::max(hi, c); }
        const double pad = std::max((hi - lo) * 0.1, 1e-9);
        v.histMin = lo - pad; v.histMax = hi + pad;
        v.hist.assign(64, 0.f);
        for (double xv : x) {
            const int b = (int)std::floor((xv - v.histMin) / (v.histMax - v.histMin) * 64);
            if (b >= 0 && b < 64) v.hist[(size_t)b] += 1.f;
        }
        // the slicer's thresholds now, and the midpoints of the measured level centres
        for (int k = 0; k + 1 < M; k++) v.thresholds.push_back((sigLo + (k + 0.5) * (sigHi - sigLo) / (M - 1)) * sc);
        bool all = true;
        double var = 0;
        size_t tot = 0;
        std::vector<double> cen((size_t)M, 0.0);
        for (int k = 0; k < M; k++) {
            const size_t c = cnt[(size_t)k];
            if (c < 3) { all = false; continue; }
            cen[(size_t)k] = sum[(size_t)k] / (double)c;
            var += sum2[(size_t)k] - (double)c * cen[(size_t)k] * cen[(size_t)k];
            tot += c;
        }
        if (!all || tot == 0) return v;
        const double sigma = std::max(std::sqrt(std::max(var, 0.0) / (double)tot), 1e-12 * std::max(1.0, std::fabs(hi - lo)));
        double gap = 1e300;
        for (int k = 0; k + 1 < M; k++) gap = std::min(gap, cen[(size_t)k + 1] - cen[(size_t)k]);
        v.separation = std::min(99.0, gap / sigma);
        v.snrDb = 20 * std::log10(std::max(v.separation, 1e-6) / 2);
        for (int k = 0; k + 1 < M; k++) v.measuredThresholds.push_back((cen[(size_t)k] + cen[(size_t)k + 1]) / 2);
        v.resolved = gap > 0 && v.separation >= 4;
        if (v.resolved) {
            v.clusters = M;
            v.centres = cen;
            if (fsk) { v.deviationHz = (cen.back() - cen.front()) / 2; v.offsetHz = (cen.back() + cen.front()) / 2; }
        }
        return v;
    }

    cf interp(double t) const {
        const long long i = (long long)std::floor(t);
        const double m = t - (double)i;
        auto at = [&](long long k) -> cf {
            const long long j = k - bufBase;
            return j >= 0 && j < (long long)buf.size() ? buf[(size_t)j] : cf(0, 0);
        };
        const float h0 = (float)(-m * (m - 1) * (m - 2) / 6), h1 = (float)((m + 1) * (m - 1) * (m - 2) / 2);
        const float h2 = (float)(-(m + 1) * m * (m - 2) / 2), h3 = (float)((m + 1) * m * (m - 1) / 6);
        return at(i - 1) * h0 + at(i) * h1 + at(i + 1) * h2 + at(i + 2) * h3;
    }

    void sample1(cf z) {
        // power and squelch (per sample, smoothed over one symbol); the noise floor follows the quietest level
        pw += (std::norm(z) - pw) * apw;
        pwS += (std::norm(z) - pwS) * apwS;
        if ((double)n1 < warmup) noise = std::max(pwS, 1e-12);   // the filters fill first
        else {
            // the noise floor: falls quickly, follows the level slowly while nothing is there, creeps up under a signal that never stops
            if (pwS < noise) noise += (pwS - noise) * 4 * apwS;
            else if (pwS < noise * thrLin) noise += (pwS - noise) * apwS / 8;
            else noise *= riseS;
            noise = std::max(noise, 1e-12);
            if (d.squelchDb <= 0) open = true;
            else if (pwS > noise * thrLin) { open = true; hangLeft = hangS; }
            else if (hangLeft > 0) hangLeft--;
            else open = false;
            // a new burst: the squelch opens, or the level jumps while it is open (a stronger signal, or no squelch)
            const bool jump = open && wasOpen && pw > acqPw * 8;   // well above the level of the last 16 symbols
            acqPw += (pw - acqPw) * peakDecay;
            if (!acquiring && ((open && !wasOpen) || jump)) { acquiring = true; acqStart = n1 + mfDelay - (long long)sps; acqPw = pw; }
            wasOpen = open;
        }
        cf v;
        if (fsk) {
            const cf p = z * std::conj(prevZ);
            prevZ = z;
            const double f = (p.real() == 0 && p.imag() == 0) ? 0.0 : std::atan2(p.imag(), p.real()) * fs1 / (2 * kPi);
            v = cf((float)(f / dev), 0);
        } else if (envelope) v = cf(std::abs(z), 0);
        else v = z;
        // matched filter
        cf y;
        if (!rrc.empty()) {
            const size_t L = rrc.size();
            rrcHist[rrcPos] = v;
            rrcHist[rrcPos + L] = v;
            rrcPos = (rrcPos + 1) % L;
            cf acc(0, 0);
            for (size_t i = 0; i < L; i++) acc += rrcHist[rrcPos + i] * rrc[i];
            y = acc;
        } else {
            mfSum += v - mfRing[mfPos];
            mfRing[mfPos] = v;
            mfPos = (mfPos + 1) % (size_t)mfLen;
            y = mfSum / (float)mfLen;
        }
        buf.push_back(y);
        n1++;
        if (acquiring) {
            if ((double)(n1 - acqStart) < acqLen) return;   // collect the start of the burst, then strobe through it
            acquire();
            acquiring = false;
        }
        while (std::floor(tNext) + 2 < (double)n1) strobe();
        // keep what the next strobe can reach
        const long long keep = (long long)std::floor(tNext - sps) - 4;
        while (bufBase < keep && !buf.empty()) { buf.pop_front(); bufBase++; }
    }

    // the start of a burst: symbol timing from the energy at the symbol rate (Oerder & Meyr), the levels, the PSK frequency
    void acquire() {
        const long long a = std::max(acqStart, bufBase), b = n1;
        if (b - a < 8) return;
        auto at = [&](long long k) { return buf[(size_t)(k - bufBase)]; };
        double c = 0;
        if (!psk) {   // the middle between the levels seen (the mean would sit on the common level when one tone dominates)
            double mn = 1e300, mx = -1e300;
            for (long long k = a + (b - a) / 4; k < b; k++) { const double r = at(k).real(); if (fsk && std::fabs(r) > 1.4) continue; mn = std::min(mn, r); mx = std::max(mx, r); }
            if (mn > mx) mn = mx = 0;
            c = (mn + mx) / 2;
        }
        std::complex<double> X(0, 0);
        for (long long k = a; k < b; k++) {
            const cf y = at(k);
            if (fsk && std::fabs(y.real()) > 1.4) continue;   // the click of the burst edge would decide the timing
            const double w = psk ? std::norm(y) : std::pow(std::clamp((double)y.real(), -1.6, 1.6) - c, 2);
            const double ph = -2 * kPi * std::fmod((double)k, sps) / sps;
            X += w * std::complex<double>(std::cos(ph), std::sin(ph));
        }
        if (std::abs(X) > 0) {
            double phase = -std::arg(X) / (2 * kPi) * sps;   // the energy peaks at k = phase (mod sps)
            if (phase < 0) phase += sps;
            const double lowest = std::max<double>((double)a + 1, (double)bufBase + sps / 2 + 2);
            const double from = std::max(lowest, tNext - sps);
            double t = std::floor(from / sps) * sps + phase;
            while (t < from) t += sps;
            tNext = t;
        }
        loopInt = 0;
        sinceOpen = 0;
        if (!psk) {
            std::vector<float> v;   // the values at the symbol times of the block
            for (double t = tNext; t + 2 < (double)b; t += sps) { const float r = interp(t).real(); if (!fsk || std::fabs(r) < 1.4) v.push_back(r); }
            if (v.size() < 4) v.assign({fsk ? -1.f : 0.f, 1.f});
            // the burst may start inside the block: the first symbols can still be rising
            if (v.size() > 7) v.erase(v.begin(), v.begin() + 2);
            std::sort(v.begin(), v.end());
            const double p5 = v.front(), p95 = v.back();
            if (fsk && M == 2) {
                lo = std::clamp(p5, -1.6, 1.6);
                hi = std::clamp(p95, -1.6, 1.6);
                if (hi - lo < 0.35) {
                    const double mid = (hi + lo) / 2;
                    // A narrow cluster very close to zero does not identify either nominal
                    // FSK tone (-1 and +1). Start centred until there is modulation to fit;
                    // the one-tone fallback would put the threshold near a real tone.
                    // Limit this guard to a quarter deviation to retain offset one-tone starts.
                    if (std::fabs(mid) < 0.25) { lo = -1; hi = 1; }
                    // Only one real tone so far: the other is two nominal deviations away.
                    else if (mid > 0) { hi = mid; lo = mid - 2; } else { lo = mid; hi = mid + 2; }
                }
            } else if (fsk) {
                // more levels: the nominal deviation, shifted to fit the symbols of the burst start (a frequency offset)
                // sampled at the widest eye near the timing estimate: a timing error pulls neighbouring tones towards each other, and a
                // pair of tones squeezed together fits the level grid shifted by half a step as well as unshifted (8FSK lost bursts)
                std::vector<double> sv;
                double bestVar = -1;
                for (int k = -8; k <= 8; k++) {
                    std::vector<double> cand;
                    for (double t = tNext + k * sps / 32; t + 2 < (double)b; t += sps) {
                        if (t < (double)bufBase + 2) continue;
                        const double r = interp(t).real();
                        if (std::fabs(r) < 1.4) cand.push_back(r);
                    }
                    if (cand.size() > 7) cand.erase(cand.begin(), cand.begin() + 2);
                    if (cand.size() < 2) continue;
                    double m = 0, var = 0;
                    for (double r : cand) m += r;
                    m /= (double)cand.size();
                    for (double r : cand) var += (r - m) * (r - m);
                    var /= (double)cand.size();
                    if (var > bestVar * (1 + 1e-9) || (k == 0 && var >= bestVar)) { bestVar = var; sv = std::move(cand); }
                }
                const double sp = 2.0 / (M - 1);
                double bestC = 0, bestCost = 1e300;
                const auto mm = std::minmax_element(sv.begin(), sv.end());
                const bool outer = !sv.empty() && *mm.second - *mm.first > 2 - sp && *mm.second - *mm.first < 2 + sp;   // the preamble uses the outer tones
                for (int i = -20; i <= 20; i++) {
                    const double c = sp / 2 * i / 20.0;
                    double cost = 0;
                    for (double r : sv) {
                        const double q = std::clamp(std::round((r - c + 1) / sp), 0.0, (double)(M - 1));
                        const double dd = r - c - (-1 + q * sp);
                        cost += std::min(dd * dd, sp * sp / 4);
                    }
                    if (cost < bestCost) { bestCost = cost; bestC = c; }
                }
                lo = outer ? *mm.first : -1 + bestC;
                hi = outer ? *mm.second : 1 + bestC;
            } else {
                lo = d.mod == Mod::OOK ? std::min(p5, std::sqrt(noise)) : p5;   // ASK: the lowest level is not off
                hi = std::max(p95, lo * 2 + 1e-9);
            }
        } else {
            // the level, and the frequency from the M-th power of the symbols of the block (the modulation removed)
            double s = 0, wsum = 0;
            int n = 0;
            std::complex<double> F(0, 0), prev(0, 0);
            for (double t = tNext; t + 2 < (double)b; t += sps, n++) {
                const cf c = interp(t);
                const std::complex<double> y(c.real(), c.imag());
                s += std::abs(y);
                std::complex<double> p = y;
                for (int m = 1; m < M; m++) p *= y;
                if (n > 0) { F += p * std::conj(prev); wsum += std::abs(p) * std::abs(prev); }
                prev = p;
            }
            agc = std::max(1e-9, n ? s / n : 1.0);
            // only a consistent estimate is used (noise in the M-th power is large for 8PSK)
            cfreq = n > 3 && std::abs(F) > 0.5 * wsum ? std::arg(F) / M : 0;   // radians per symbol
            cph = 0;
        }
    }

    // the Signal view takes a symbol only while the signal is really there: not in the squelch hang time after a burst, where the
    // strobes sit on noise (it would smear every cluster), and not while the levels / the carrier loop settle at the start of a burst (24 symbols, PSK 48)
    bool present() const { return open && sinceOpen > (psk ? 48 : 24) && pwS > noise * std::max(thrLin, 4.0); }

    void strobe() {
        const cf y = interp(tNext), ym = interp(tNext - sps / 2);
        sinceOpen++;
        int level = 0;
        double e = 0;
        if (!psk) {
            const double r = y.real();
            const double c = (hi + lo) / 2, sc = std::max(1e-9, (hi - lo) / 2);
            const double q = (r - lo) / std::max(1e-9, hi - lo) * (M - 1);
            level = std::clamp((int)std::lround(q), 0, M - 1);
            if (open && (!fsk || std::fabs(r) < 1.4)) {
                // least-squares fit of the levels: a decision on level L moves both ends by its share
                const double a = sinceOpen < 24 ? 0.1 : 0.03;
                const double x = (double)level / (M - 1), err = r - (lo + x * (hi - lo));
                lo += a * err * (1 - x);
                hi += a * err * x;
                if (fsk && hi - lo < 0.3) { const double mid = (hi + lo) / 2; hi = mid + 0.15; lo = mid - 0.15; }
                if (!fsk && hi - lo < 1e-9) hi = lo + 1e-9;
            }
            const double A = (yPrev.real() - c) / sc, B = (r - c) / sc, Mi = (ym.real() - c) / sc;
            e = (B - A) * Mi * 0.5;
            if (fsk && (std::fabs(r) > 1.4 || std::fabs(ym.real()) > 1.4 || std::fabs(yPrev.real()) > 1.4)) e = 0;   // a click (burst edge, impulse)
            yPrev = y;
            if (present() && (!fsk || std::fabs(r) < 1.4)) {
                std::lock_guard<std::mutex> sl(sigMu);
                sigR[sigPos] = (float)r; sigLvl[sigPos] = (uint8_t)level;
                sigPos = (sigPos + 1) % kSigCap; sigCnt = std::min(sigCnt + 1, kSigCap);
                sigLo = lo; sigHi = hi;
            }
        } else {
            const double a = std::abs(y);
            if (open) agc += 0.05 * (a - agc);
            const float g = (float)(1.0 / std::max(agc, 1e-12));
            const cf rotc((float)std::cos(-cph), (float)std::sin(-cph));
            const cf z = y * g * rotc, zm = ym * g * rotc, zp = yPrev * g * rotc;
            const double ang = std::atan2(z.imag(), z.real());
            int idx = (int)std::lround(ang / (2 * kPi / M));
            idx = ((idx % M) + M) % M;
            const double pe = wrap(ang - 2 * kPi * idx / M);
            if (open) {
                const double kp = sinceOpen < 32 ? 0.25 : 0.1, kf = sinceOpen < 32 ? 0.02 : 0.004;
                cfreq += kf * pe;
                cph = wrap(cph + cfreq + kp * pe);
            }
            const cf dz = z - zp;
            e = (dz.real() * zm.real() + dz.imag() * zm.imag()) * 0.5;
            yPrev = y;
            if (present()) {
                std::lock_guard<std::mutex> sl(sigMu);
                sigZ[sigPos] = z; sigLvl[sigPos] = (uint8_t)idx;
                sigPos = (sigPos + 1) % kSigCap; sigCnt = std::min(sigCnt + 1, kSigCap);
                sigCfreq = cfreq;
            }
            if (d.differential) { const int dv = ((idx - prevIdx) % M + M) % M; prevIdx = idx; level = dv; }
            else level = idx;
        }
        e = std::clamp(e, -1.0, 1.0);
        if (open) {
            // proportional and a small integral part (the transmitter's clock error, up to 0.3 %)
            const double kp = sinceOpen < 16 ? 0.12 : 0.06, ki = sinceOpen < 16 ? 0.0004 : 0.0001;
            loopInt = std::clamp(loopInt + ki * e * sps, -0.003 * sps, 0.003 * sps);
            tNext += sps - kp * e * sps - loopInt;
        } else tNext += sps - loopInt;
        S.push_back((uint8_t)level);
        Sopen.push_back(open ? 1 : 0);
        Ssnr.push_back((float)(10 * std::log10(std::max(1e-12, pw / noise))));
        Tpos.push_back((double)(tNext - sps) * D);   // input sample of this symbol (approximately)
        if ((long long)S.size() - (searchFrom - symBase) >= (long long)(needSyms + syncSyms + chunk)) process(false);
    }

    // the bits of the symbols [from, end) with variant v: bit 0 inverted / conjugated, bits 1..: PSK rotation
    void variantBits(long long from, int v, std::vector<uint8_t>& bits) const {
        bits.clear();
        const bool inv = v & 1;
        const int rotn = v >> 1;
        for (size_t i = (size_t)(from - symBase); i < S.size(); i++) {
            int L = S[i];
            if (psk) {
                if (inv) L = (M - L) % M;
                if (!d.differential) L = (L + rotn) % M;
            } else if (inv) L = M - 1 - L;
            const int val = valueOf[(size_t)L];
            for (int k = bps - 1; k >= 0; k--) bits.push_back((uint8_t)((val >> k) & 1));
        }
    }

    struct Hit { long long sym; int errs; int var, phase; size_t decStart; };

    void process(bool final) {
        const long long endSym = symBase + (long long)S.size();
        if (endSym <= searchFrom) return;
        const long long from = std::max(symBase, searchFrom - 4);
        std::vector<int> variants;
        const bool tryInv = d.polarity == Polarity::Auto, onlyInv = d.polarity == Polarity::Inverted;
        const int rots = psk && !d.differential ? M : 1;
        for (int r = 0; r < rots; r++) {
            if (!onlyInv) variants.push_back(r << 1);
            if (tryInv || onlyInv) variants.push_back((r << 1) | 1);
        }
        const int phases = stage::linePhases(d.line);
        const std::vector<uint8_t> sw = d.sync.toBits();
        const int sl = (int)sw.size();
        const uint64_t mask = sl >= 64 ? ~0ull : (1ull << sl) - 1;
        const uint64_t want = d.sync.value & mask;
        struct Dec { std::vector<uint8_t> bits; std::vector<int> pos; };
        std::vector<Dec> decs;
        std::vector<Hit> hits;
        std::vector<uint8_t> chips;
        const bool gate = d.squelchDb > 0;
        for (int v : variants) {
            variantBits(from, v, chips);
            for (int ph = 0; ph < phases; ph++) {
                Dec dc;
                dc.bits = stage::lineDecode(chips, d.line, d.nrziToggleOn, ph, &dc.pos, nullptr);
                uint64_t wnd = 0;
                for (size_t i = 0; i < dc.bits.size(); i++) {
                    wnd = ((wnd << 1) | dc.bits[i]) & mask;
                    if ((int)i + 1 < sl) continue;
                    uint64_t x = wnd ^ want;
                    int errs = 0;
                    while (x && errs <= d.syncMaxErrors) { x &= x - 1; errs++; }
                    if (errs > d.syncMaxErrors) continue;
                    const long long chipStart = dc.pos[i + 1 - (size_t)sl];
                    const long long sym = from + chipStart / bps;
                    if (sym < searchFrom) continue;
                    if (gate && !Sopen[(size_t)(sym - symBase)]) continue;
                    hits.push_back({sym, errs, (int)decs.size(), ph, i + 1});
                }
                decs.push_back(std::move(dc));
            }
        }
        std::stable_sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) { return a.sym != b.sym ? a.sym < b.sym : a.errs < b.errs; });
        long long cursor = searchFrom;
        long long pending = -1;
        std::vector<Frame> got;
        long long lastBad = -1000000;
        for (size_t h = 0; h < hits.size(); h++) {
            const Hit& hit = hits[h];
            if (hit.sym < cursor) continue;
            const Dec& dc = decs[(size_t)hit.var];
            // enough symbols after the sync word for the longest frame?
            const long long needEnd = hit.sym + syncSyms + needSyms;
            if (!final && needEnd > endSym) { pending = hit.sym; break; }
            Frame f;
            size_t used = 0;
            if (!decodeFrameBits(d, dc.bits, hit.decStart, true, f, used)) { pending = hit.sym; break; }
            {
                std::lock_guard<std::mutex> lk(mu);
                st.syncHits++;
            }
            const bool good = !f.bytes.empty() && f.good();
            if (good) {
                // the same frame found by another variant at the same place: the first good one wins
                f.syncErrors = hit.errs;
                const size_t s0 = (size_t)(hit.sym - symBase);
                double snr = 0;
                int n = 0;
                for (size_t k = s0; k < S.size() && k < s0 + (size_t)syncSyms; k++, n++) snr += Ssnr[k];
                f.snrDb = n ? snr / n : 0;
                f.timeSec = Tpos[s0] / fsIn;
                const size_t lastDec = std::min(dc.pos.size() - 1, hit.decStart + (used ? used - 1 : 0));
                const long long endS = from + (dc.pos[lastDec] + 1) / bps;
                got.push_back(std::move(f));
                cursor = std::max(hit.sym + 1, endS);
                lastBad = -1000000;
            } else {
                // a bad frame: shown only when asked, once per place, and only if no good frame starts at the same symbol
                bool goodSame = false;
                for (size_t k = h + 1; k < hits.size() && hits[k].sym <= hit.sym + 1; k++) goodSame = true;
                if (d.showBadFrames && !goodSame && hit.sym > lastBad + syncSyms && !f.bytes.empty()) {
                    f.syncErrors = hit.errs;
                    const size_t s0 = (size_t)(hit.sym - symBase);
                    f.timeSec = Tpos[s0] / fsIn;
                    f.snrDb = Ssnr[s0];
                    got.push_back(std::move(f));
                    lastBad = hit.sym;
                    std::lock_guard<std::mutex> lk(mu);
                    st.badFrames++;
                }
                if (!goodSame) cursor = std::max(cursor, hit.sym + 1);
            }
        }
        if (pending >= 0) searchFrom = std::max(cursor, pending);
        else searchFrom = std::max(cursor, final ? endSym : endSym - syncSyms - 8);
        // drop the symbols nothing can use any more
        const long long keepFrom = std::max(symBase, searchFrom - 16);
        const size_t drop = (size_t)(keepFrom - symBase);
        if (drop > 0) {
            S.erase(S.begin(), S.begin() + (std::ptrdiff_t)drop);
            Sopen.erase(Sopen.begin(), Sopen.begin() + (std::ptrdiff_t)drop);
            Ssnr.erase(Ssnr.begin(), Ssnr.begin() + (std::ptrdiff_t)drop);
            Tpos.erase(Tpos.begin(), Tpos.begin() + (std::ptrdiff_t)drop);
            symBase = keepFrom;
        }
        std::lock_guard<std::mutex> lk(mu);
        for (Frame& f : got) { st.frames++; st.snrDb = f.snrDb; out.push_back(std::move(f)); }
        if (out.size() > 20000) out.erase(out.begin(), out.begin() + (std::ptrdiff_t)(out.size() - 20000));
    }

    void feed(const cf* x, size_t n) {
        const size_t N = taps.size();
        for (size_t i = 0; i < n; i++) {
            const cf y = x[i] * rot;
            rot *= rotStep;
            if (++rotCount >= 1024) { rot /= std::abs(rot); rotCount = 0; }
            if (N == 1) { if (++decCount >= D) { decCount = 0; sample1(y * taps[0]); } continue; }
            hist[hpos] = y;
            hist[hpos + N] = y;
            hpos = (hpos + 1) % N;
            if (++decCount >= D) {
                decCount = 0;
                cf acc(0, 0);
                const cf* h = &hist[hpos];
                for (size_t k = 0; k < N; k++) acc += h[k] * taps[k];
                sample1(acc);
            }
        }
        inCount += (long long)n;
        std::lock_guard<std::mutex> lk(mu);
        st.symbols = (uint64_t)(symBase + (long long)S.size());
        st.open = open;
    }
};

Receiver::Receiver(const Definition& d) : p_(std::make_unique<Impl>(d)) { p_->configure(d.sampleRate, 0); }
Receiver::~Receiver() = default;
void Receiver::configure(double rate, double extra) { p_->configure(rate, extra); }
void Receiver::feed(const cf* iq, size_t n) { p_->feed(iq, n); }
void Receiver::flush() {
    // push the last symbols through the filters, then decode what is left
    std::vector<cf> z((size_t)std::min<double>(p_->fsIn, p_->taps.size() + (p_->sps * 8 + 16) * p_->D), cf(0, 0));
    p_->feed(z.data(), z.size());
    p_->process(true);
}
std::vector<Frame> Receiver::take() {
    std::lock_guard<std::mutex> lk(p_->mu);
    std::vector<Frame> v;
    v.swap(p_->out);
    return v;
}
SignalView Receiver::signalView() const { return p_->view(); }
RxStats Receiver::stats() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->st; }
const Definition& Receiver::definition() const { return p_->d; }

std::vector<Frame> decodeIq(const Definition& d, const std::vector<cf>& iq, double fs, double extra, RxStats* stats) {
    Receiver r(d);
    r.configure(fs, extra);
    const size_t step = 65536;
    std::vector<Frame> all;
    for (size_t i = 0; i < iq.size(); i += step) {
        r.feed(iq.data() + i, std::min(step, iq.size() - i));
        for (Frame& f : r.take()) all.push_back(std::move(f));
    }
    r.flush();
    for (Frame& f : r.take()) all.push_back(std::move(f));
    if (stats) *stats = r.stats();
    return all;
}

}   // namespace dect2::modebuilder
