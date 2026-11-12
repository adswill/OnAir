// Fleet Broadcast monitor: see fbr_rx.h for what is public and what is not.
//
// Chain (everything at the radio's rate R is a mix and a box filter):
//   mix by the nominal offset and the tracked carrier offset, integrate-and-dump by D = round(R / 19200) to fs ~ 19.2 kHz (16 samples per symbol),
//   acquisition: square the baseband (BPSK loses its phase, the carrier shows as a line at twice its offset), a Hann FFT of 8192 samples finds the
//     line; the offset is half the line frequency, resolution about 0.6 Hz, range +-4.5 kHz,
//   symbol clock: the energy |b|^2 of a BPSK signal with transitions has a line at the symbol rate (Oerder & Meyr); its phase gives the symbol
//     centre, a first-order loop pulls the sampling instants to it and the average correction is the clock error,
//   bits: a box integral over one symbol sampled at the symbol end, differential detection, a bit 1 is a phase change (the polarity is not
//     published: setInvert flips it), a frequency loop on the phase of the decisions keeps the carrier within a fraction of a Hz,
//   frames: 16 bits per frame, 16 columns. Every ~0.85 s the last 4096 bits are cut into the 16 columns and each is classified.
//     The one column that repeats (period 3 .. 64 frames, 80% agreement) is taken for channel 0 (the sync channel).
// Written to the engine's contract: feed() from the analysis thread, telemetry() and the setters from the interface thread.
#include "dect2/fbr/fbr_rx.h"
#include "dect2/fbr/fbr_tel.h"
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <deque>
#include <mutex>
#include <vector>

namespace dect2 {

namespace {
constexpr double kPi = 3.14159265358979323846;
using cd = std::complex<double>;
constexpr double kFsTarget = 19200.0;
constexpr int kAcqN = 8192;
constexpr int kBitWindow = 4096, kBitMin = 2048, kAnalyseEvery = 1024;
constexpr double kTimingThresh = 0.015;

const fbr::Carrier kCarriers[] = {{"250.450 MHz", 250.450}, {"250.550 MHz", 250.550}, {"250.650 MHz", 250.650}};

void fft(std::vector<cd>& a) {
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; i++) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        const double ang = -2 * kPi / (double)len;
        const cd wl(std::cos(ang), std::sin(ang));
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

std::string hex64(const std::vector<uint8_t>& col) {   // the last 64 bits, oldest first
    const size_t n = col.size();
    const size_t k = std::min<size_t>(64, n);
    std::string s;
    unsigned v = 0;
    int nb = 0;
    // pad on the left with zeros to a multiple of 4
    const size_t pad = (4 - k % 4) % 4;
    for (size_t i = 0; i < pad; i++) { v = v << 1; nb++; }
    for (size_t i = n - k; i < n; i++) {
        v = (v << 1) | col[i]; nb++;
        if (nb == 4) { s += "0123456789abcdef"[v & 15]; v = 0; nb = 0; }
    }
    return s;
}
} // namespace

struct FbrReceiver::Impl {
    std::mutex mu;
    // settings
    double rate = 0, offsetSet = 0, centerHz = 0;
    bool invert = false, invertApplied = false;
    std::function<void(const std::string&)> logCb;
    // front end
    int D = 1;
    double fs = 0, sps = 16;
    int boxCount = 0;
    cd boxAcc{0, 0};
    cd mixPh{1, 0}, mixRot{1, 0};
    double mixFreq = 0;            // total mixing frequency, Hz (nominal offset + carrier offset)
    double cfo = 0;                // tracked carrier offset from the nominal place, Hz
    uint64_t sampleCount = 0;      // input samples
    uint64_t sincePub = 0;
    double level = 0;              // mean power estimate of the input
    // acquisition
    std::vector<cd> acq;
    // baseband samples and timing
    std::vector<cd> bring;         // ring of the last 64 baseband samples
    std::vector<cd> yring;         // ring of the box integral y
    double nCur = 0;               // index of the newest baseband sample
    cd xAcc{0, 0}, xPh{1, 0}, xRot{1, 0};
    double s0 = 0;                 // smoothed energy
    double xDecay = 0;
    double tStrobe = -1;
    bool strobeOn = false;
    double spsEff = 0;
    double ppmAcc = 0;
    cd prevS{0, 0};
    bool havePrev = false;
    double absRe = 0, imSq = 0;
    uint64_t sinceAcq = 0;
    uint64_t badTiming = 0;        // samples with no timing line while locked
    // bits and frames
    std::deque<uint8_t> bits;
    uint64_t bitCount = 0, sinceAnalysis = 0;
    int syncCol = -1, lostCount = 0;
    // results
    FbrTelemetry tel;
    uint64_t seq = 0;
    bool dirty = true;
    double timeSec = 0;
    std::vector<std::string> log;

    void say(const std::string& s) {
        char b[64];
        snprintf(b, sizeof b, "%.1f s  ", timeSec);
        log.push_back(std::string(b) + s);
        if (log.size() > kFbrLogMax) log.erase(log.begin());
        if (logCb) logCb("Fleet Broadcast: " + s);
        dirty = true;
    }

    void initFront() {
        D = std::max(1, (int)std::lround(rate / kFsTarget));
        fs = rate / D;
        sps = fs / fbr::kBaud;
        spsEff = sps;
        boxCount = 0; boxAcc = 0;
        mixFreq = offsetSet + cfo;
        mixPh = cd(1, 0);
        mixRot = std::polar(1.0, -2 * kPi * mixFreq / rate);
        bring.assign(64, cd(0, 0));
        yring.assign(64, cd(0, 0));
        xRot = std::polar(1.0, -2 * kPi / sps);
        xDecay = 1.0 - 1.0 / (0.5 * fs);
    }

    void resetState(bool keepLog) {
        tel = FbrTelemetry();
        cfo = 0;
        acq.clear();
        nCur = 0; xAcc = 0; xPh = cd(1, 0); s0 = 0; tStrobe = -1; strobeOn = false;
        ppmAcc = 0; prevS = 0; havePrev = false; absRe = 0; imSq = 0; sinceAcq = 0; badTiming = 0;
        bits.clear(); bitCount = 0; sinceAnalysis = 0; syncCol = -1; lostCount = 0;
        if (!keepLog) log.clear();
        if (rate > 0) initFront();
        tel.state = 0;
        dirty = true;
    }

    void setMix(double f) {
        mixFreq = f;
        mixRot = std::polar(1.0, -2 * kPi * f / rate);
    }

    // back to acquisition (carrier lost)
    void dropToAcquire(const char* why) {
        if (tel.state > 0) say(why);
        if (tel.framed) tel.blocksBad++;
        tel.state = 0; tel.framed = false; tel.dataValid = false; tel.ambiguous = false; tel.syncPeriod = 0; tel.syncQuality = 0;
        tel.status.clear();
        for (auto& c : tel.ch) c = FbrChannel();
        cfo = 0; setMix(offsetSet);
        acq.clear();
        strobeOn = false; tStrobe = -1; xAcc = 0; s0 = 0; havePrev = false; ppmAcc = 0;
        sinceAcq = 0; badTiming = 0;
        bits.clear(); bitCount = 0; sinceAnalysis = 0; syncCol = -1; lostCount = 0;
        dirty = true;
    }

    void acquire() {
        // squared baseband, Hann window
        std::vector<cd> a(kAcqN);
        for (int i = 0; i < kAcqN; i++) {
            const double w = 0.5 - 0.5 * std::cos(2 * kPi * (i + 0.5) / kAcqN);
            a[i] = acq[(size_t)i] * w;
        }
        fft(a);
        std::vector<double> pw(kAcqN);
        for (int i = 0; i < kAcqN; i++) pw[(size_t)i] = std::norm(a[(size_t)i]);
        // bins of the squared signal: line at 2f, |f| <= max carrier offset
        const double binHz = fs / kAcqN;
        int best = -1;
        double bestP = 0;
        for (int i = 0; i < kAcqN; i++) {
            const int k = i < kAcqN / 2 ? i : i - kAcqN;
            const double f2 = k * binHz;
            if (std::fabs(f2 / 2) > fbr::kMaxCarrierHz) continue;
            if (pw[(size_t)i] > bestP) { bestP = pw[(size_t)i]; best = i; }
        }
        if (best < 0) return;
        std::vector<double> sorted = pw;
        const size_t mid = sorted.size() / 2;
        std::nth_element(sorted.begin(), sorted.begin() + (long)mid, sorted.end());
        const double med = std::max(1e-30, sorted[mid]);
        const double ratio = 10 * std::log10(bestP / (med * 1.443));   // the median of an exponential is ln2 of its mean
        if (ratio < 17.0) return;
        // a parabola through the peak and its neighbours for a sub-bin frequency
        const double pm = pw[(size_t)((best + kAcqN - 1) % kAcqN)], p0 = pw[(size_t)best], pp = pw[(size_t)((best + 1) % kAcqN)];
        double delta = 0;
        const double den = pm - 2 * p0 + pp;
        if (den < 0) delta = 0.5 * (pm - pp) / den;
        const int k = best < kAcqN / 2 ? best : best - kAcqN;
        const double f2 = (k + delta) * binHz;
        cfo = f2 / 2;
        setMix(offsetSet + cfo);
        tel.toneDb = (float)ratio;
        tel.state = 1;
        tel.offsetHz = offsetSet + cfo;
        char b[120];
        snprintf(b, sizeof b, "BPSK carrier at %+.1f Hz from the tuned frequency, %.0f dB over the floor", offsetSet + cfo, ratio);
        say(b);
        // start the symbol clock search afresh
        acq.clear();
        sinceAcq = 0; badTiming = 0; xAcc = 0; s0 = 0; strobeOn = false; tStrobe = -1; havePrev = false; ppmAcc = 0;
        bits.clear(); bitCount = 0; sinceAnalysis = 0;
        spsEff = sps;
    }

    // one baseband sample at fs
    void onBase(cd b) {
        step(b);
        nCur += 1;
        xPh *= xRot;
        xPh *= 1.5 - 0.5 * std::norm(xPh);
    }
    void step(cd b) {
        const size_t RS = bring.size();
        const size_t idx = (size_t)nCur % RS;
        bring[idx] = b;
        // box integral over one symbol (fractional): y = sum of L samples + frac of the one before
        const int L = (int)std::floor(sps);
        const double frac = sps - L;
        cd y(0, 0);
        for (int k = 0; k < L; k++) y += bring[((size_t)nCur + RS - (size_t)k) % RS];
        y += frac * bring[((size_t)nCur + RS - (size_t)L) % RS];
        yring[idx] = y / sps;

        if (tel.state == 0) {
            acq.push_back(b * b);
            if ((int)acq.size() >= kAcqN) { acquire(); if (tel.state == 0) acq.clear(); }
            return;
        }
        sinceAcq++;
        // timing line of the energy
        const double e2 = std::norm(b);
        xAcc = xAcc * xDecay + e2 * xPh;
        s0 = s0 * xDecay + e2;
        const double ratio = s0 > 0 ? std::abs(xAcc) / s0 : 0;
        const double nmax = -std::arg(xAcc) * sps / (2 * kPi);   // where the energy peaks, modulo sps
        const auto wrap = [&](double v) { v = std::fmod(v, sps); if (v > sps / 2) v -= sps; if (v < -sps / 2) v += sps; return v; };

        if (!strobeOn) {
            if (sinceAcq > (uint64_t)(0.4 * fs) && ratio > kTimingThresh) {
                const double target = nmax + (sps - 1) / 2.0;
                double dt = std::fmod(target - nCur, sps);
                if (dt < 0) dt += sps;
                tStrobe = nCur + dt + sps;
                strobeOn = true;
                havePrev = false;
            } else if (sinceAcq > (uint64_t)(6.0 * fs)) {
                dropToAcquire("no 1200 Bd clock found: not a Fleet Broadcast signal, or too weak");
            }
            return;
        }
        // symbol decisions
        while (strobeOn && tStrobe <= nCur) {
            const double ft = std::floor(tStrobe);
            const double w = tStrobe - ft;
            const cd y0 = yring[(size_t)ft % RS], y1 = yring[(size_t)(ft + 1) % RS];
            const cd sNow = y0 * (1 - w) + (ft + 1 <= nCur ? y1 : y0) * w;
            if (ratio > kTimingThresh) {
                const double err = wrap(nmax + (sps - 1) / 2.0 - tStrobe);
                tStrobe += spsEff + 0.05 * err;
                ppmAcc = ppmAcc * 0.999 + 0.001 * (0.05 * err) / sps * 1e6;
                badTiming = 0;
            } else {
                tStrobe += spsEff;
                badTiming++;
            }
            if (havePrev) {
                const cd d = sNow * std::conj(prevS);
                int bit = d.real() < 0 ? 1 : 0;
                if (invert) bit ^= 1;
                // frequency loop: the residual phase step of the decisions
                const double mag = std::abs(d);
                if (mag > 0) {
                    const double th = std::arg(d * d) / 2;   // -pi/2 .. pi/2 whatever the bit
                    const double dF = th * fbr::kBaud / (2 * kPi);
                    cfo += 0.01 * dF;
                    if (cfo > fbr::kMaxCarrierHz) cfo = fbr::kMaxCarrierHz;
                    if (cfo < -fbr::kMaxCarrierHz) cfo = -fbr::kMaxCarrierHz;
                    setMix(offsetSet + cfo);
                }
                absRe = absRe * 0.995 + 0.005 * std::fabs(d.real());
                imSq = imSq * 0.995 + 0.005 * d.imag() * d.imag();
                onBit(bit);
            }
            prevS = sNow; havePrev = true;
        }
        if (badTiming > (uint64_t)(3.0 * fbr::kBaud)) dropToAcquire("lost the 1200 Bd clock");
    }

    void onBit(int bit) {
        tel.symbols++;
        bitCount++;
        bits.push_back((uint8_t)bit);
        if (bits.size() > (size_t)kBitWindow + 64) bits.pop_front();
        if (tel.state == 1) {
            tel.state = 2;
            say("1200 Bd clock locked, bits flowing");
        }
        if (tel.framed && (bitCount - frameBase) >= 16) { /* frames counted in analyse() */ }
        if (++sinceAnalysis >= (uint64_t)kAnalyseEvery) { sinceAnalysis = 0; analyse(); }
    }
    uint64_t frameBase = 0;

    void analyse() {
        const size_t n = bits.size();
        if (n < (size_t)kBitMin) return;
        // the columns: the position of a bit in the global stream modulo 16; bits[] starts at global index bitCount - n
        const uint64_t g0 = bitCount - n;
        std::vector<std::vector<uint8_t>> col(16);
        for (size_t i = 0; i < n; i++) col[(size_t)((g0 + i) % 16)].push_back(bits[i]);
        FbrChannel res[16];
        int sCols = 0, sCol = -1;
        for (int c = 0; c < 16; c++) {
            const auto& v = col[(size_t)c];
            const size_t m = v.size();
            FbrChannel& r = res[c];
            r.hex = hex64(v);
            size_t ones = 0, trans = 0;
            for (size_t i = 0; i < m; i++) { ones += v[i]; if (i && v[i] != v[i - 1]) trans++; }
            r.ones = (float)ones / (float)m;
            r.activity = m > 1 ? (float)trans / (float)(m - 1) : 0;
            if (r.ones >= 0.9f || r.ones <= 0.1f) { r.cls = 'i'; continue; }
            if (r.activity >= 0.9f) { r.cls = 'r'; continue; }
            int bestP = 0;
            double bestAgree = 0;
            for (int P = 3; P <= 64; P++) {
                if ((int)m - P < 64) break;
                size_t agree = 0;
                for (size_t i = 0; i + P < m; i++) agree += v[i] == v[i + P];
                const double a = (double)agree / (double)(m - (size_t)P);
                if (a >= 0.8) { bestP = P; bestAgree = a; break; }
            }
            if (bestP) {
                r.cls = 'S'; r.period = bestP;
                sCols++; sCol = c;
                tel.syncQuality = (int)std::lround(bestAgree * 100);
                continue;
            }
            r.cls = (r.ones > 0.3f && r.ones < 0.7f && r.activity > 0.3f && r.activity < 0.7f) ? 'e' : '-';
        }
        const bool wasFramed = tel.framed;
        if (sCols == 1) {
            if (wasFramed && syncCol != sCol) { tel.blocksBad++; say("the sync channel moved: frame alignment redone"); }
            if (!wasFramed || syncCol != sCol) {
                char b[160];
                snprintf(b, sizeof b, "sync channel found, repeats every %d frames: %d channels framed", res[sCol].period, 16);
                say(b);
                frameBase = bitCount;
                tel.frames = 0;
            }
            syncCol = sCol; lostCount = 0;
            tel.framed = true; tel.ambiguous = false; tel.dataValid = true; tel.state = 3;
            tel.syncPeriod = res[sCol].period;
            // channel k is the column sync + k (the sync channel is channel 0)
            for (int k = 0; k < 16; k++) tel.ch[k] = res[(sCol + k) % 16];
            std::string st = "[";
            for (int k = 0; k < 16; k++) st += tel.ch[k].cls;
            tel.status = st + "]";
            // frames since the alignment: whole frames passed
            tel.frames = (bitCount - frameBase) / 16;
            tel.blocksOk = tel.frames;
            // the last frames: word = 16 bits starting at a sync column bit
            tel.frameLog.clear();
            // the latest whole frame ends at the global bit index with (index % 16) == (syncCol + 15) % 16
            int64_t endIdx = (int64_t)bitCount - 1;
            while ((endIdx % 16) != (syncCol + 15) % 16) endIdx--;
            for (int f = (int)kFbrFramesShown - 1; f >= 0; f--) {
                const int64_t last = endIdx - 16 * f;
                const int64_t first = last - 15;
                if (first < (int64_t)g0) continue;
                unsigned w = 0;
                for (int64_t i = first; i <= last; i++) w = (w << 1) | bits[(size_t)(i - (int64_t)g0)];
                char hb[8];
                snprintf(hb, sizeof hb, "%04X", w);
                tel.frameLog.push_back(hb);
            }
        } else {
            for (int c = 0; c < 16; c++) tel.ch[c] = res[c];   // unframed: the columns as they come, no channel numbers
            if (sCols > 1) {
                if (!tel.ambiguous) say("several columns repeat: the frame start cannot be told");
                tel.ambiguous = true;
            } else tel.ambiguous = false;
            if (wasFramed) {
                if (++lostCount >= 3) {
                    tel.framed = false; tel.dataValid = false; tel.state = 2; tel.blocksBad++; syncCol = -1;
                    tel.syncPeriod = 0; tel.status.clear(); tel.frameLog.clear();
                    say("the sync channel is gone: frame alignment lost");
                } else for (int k = 0; k < 16; k++) tel.ch[k] = res[(syncCol + k) % 16];
            } else {
                tel.syncPeriod = 0; tel.status.clear();
                std::string st = "[";
                for (int c = 0; c < 16; c++) st += tel.ch[c].cls;
                tel.status = st + "]";
            }
        }
        dirty = true;
    }

    void publish() {
        tel.timeSec = timeSec;
        tel.inputRate = rate;
        tel.cfoHz = cfo;
        tel.offsetHz = offsetSet + cfo;
        tel.centerMhz = centerHz / 1e6;
        tel.freqMhz = centerHz > 0 ? (centerHz + tel.offsetHz) / 1e6 : 0;
        tel.carrier = tel.state > 0 && centerHz > 0 ? fbr::carrierNear(tel.freqMhz) : "";
        tel.baud = fbr::kBaud * (1.0 + 0.0);
        tel.clockPpm = -ppmAcc;
        tel.inverted = invert;
        tel.eye = absRe > 0 ? (float)std::min(1.0, std::sqrt(imSq) / absRe) : 1.0f;
        tel.snrDb = (tel.state >= 2 && imSq > 0) ? (float)(10 * std::log10(std::max(1e-9, absRe * absRe / imSq))) : 0.0f;
        tel.levelDb = level > 0 ? (float)(10 * std::log10(level)) : -200.0f;
        tel.bits.clear();
        const size_t n = std::min(bits.size(), kFbrBitsShown);
        for (size_t i = bits.size() - n; i < bits.size(); i++) tel.bits += bits[i] ? '1' : '0';
        tel.log = log;
    }
};

FbrReceiver::FbrReceiver() : p_(new Impl) {}
FbrReceiver::~FbrReceiver() = default;

void FbrReceiver::configure(double inputRateHz) {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->rate = inputRateHz;
    p_->resetState(false);
}
void FbrReceiver::setSignalOffset(double hz) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (hz == p_->offsetSet) return;
    p_->offsetSet = hz;
    if (p_->rate > 0) { p_->cfo = 0; p_->setMix(hz); }
}
void FbrReceiver::setCenterHz(double hz) { std::lock_guard<std::mutex> lk(p_->mu); p_->centerHz = hz; p_->dirty = true; }
void FbrReceiver::setInvert(bool on) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (on == p_->invert) return;
    p_->invert = on;
    p_->bits.clear(); p_->bitCount = 0; p_->sinceAnalysis = 0;   // the stored bits have the old polarity
    if (p_->tel.framed) { p_->tel.framed = false; p_->tel.dataValid = false; p_->tel.state = 2; p_->syncCol = -1; p_->tel.status.clear(); p_->tel.frameLog.clear(); }
    p_->dirty = true;
}
bool FbrReceiver::ready() const { return p_->rate >= 38400; }
void FbrReceiver::reset() { std::lock_guard<std::mutex> lk(p_->mu); p_->timeSec = 0; p_->resetState(false); }
void FbrReceiver::clearResults() {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->log.clear();
    p_->tel.frameLog.clear();
    p_->tel.symbols = 0;
    p_->dirty = true;
}
void FbrReceiver::setLogCallback(std::function<void(const std::string&)> cb) { std::lock_guard<std::mutex> lk(p_->mu); p_->logCb = std::move(cb); }

void FbrReceiver::feed(const cf32* x, size_t n) {
    Impl& s = *p_;
    std::unique_lock<std::mutex> lk(s.mu, std::try_to_lock);
    if (!lk.owns_lock() || s.rate <= 0) return;
    for (size_t i = 0; i < n; i++) {
        const cd z(x[i].real(), x[i].imag());
        s.level = s.level * 0.9999 + 0.0001 * std::norm(z);
        s.mixPh *= s.mixRot;
        if ((i & 1023) == 0) { const double m = std::abs(s.mixPh); if (m > 0) s.mixPh /= m; }
        s.boxAcc += z * s.mixPh;
        if (++s.boxCount >= s.D) {
            s.boxCount = 0;
            const cd b = s.boxAcc / (double)s.D;
            s.boxAcc = 0;
            s.onBase(b);
        }
    }
    s.sampleCount += n;
    s.timeSec += (double)n / s.rate;
    s.sincePub += n;
    if (s.dirty || s.sincePub >= (uint64_t)(0.25 * s.rate)) { s.sincePub = 0; s.publish(); s.seq++; s.tel.seq = s.seq; s.dirty = false; }
}

bool FbrReceiver::telemetry(FbrTelemetry& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (p_->seq == lastSeq && lastSeq != 0) return false;
    p_->tel.seq = p_->seq;
    out = p_->tel;
    return true;
}

const fbr::Carrier* fbr::carriers(int& count) { count = 3; return kCarriers; }
std::string fbr::carrierNear(double mhz, double tolKhz) {
    for (const auto& c : kCarriers) if (std::fabs(c.mhz - mhz) * 1000.0 <= tolKhz) return c.name;
    return "";
}

ModeTuning fbrTuning() {
    ModeTuning t;
    t.stdMode = 872; t.id = "fbr"; t.name = "Fleet Broadcast (UHF SATCOM)";
    t.minMhz = 240; t.maxMhz = 270; t.defMhz = 250.450;
    t.sampleRate = 250000;                 // the signal is about 2.4 kHz wide; the rest is room for the Doppler shift and the radio's offset
    t.basebandHz = 0;
    t.bandwidthMhz = 0.25;
    t.minSampleRate = 38400;
    t.tuneOffsetHz = 30000;                // keep the radio's DC spike away from the carrier
    return t;
}

} // namespace dect2
