// MPT 1327 control channel decoder: MSK-style differential FFSK demodulator, per-phase sync search, codeword framing.
#include "dect2/mpt1327/mpt1327_rx.h"
#include <cmath>
#include <cstdio>
#include <complex>

namespace dect2::mpt1327 {

namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr double kFs = 24000.0;            // internal audio rate: 20 samples per bit
constexpr int kSpb = 20;
constexpr int kTaps = 31;
constexpr double kLockHold = 3.0;          // seconds a good codeword keeps the lock indicator
}

struct Decoder::Impl {
    Stats st;
    std::vector<Event> events;
    double rate = 0;
    // resampler to kFs (linear interpolation)
    double srcPos = 0;
    float prev = 0;
    bool havePrev = false;
    // demodulator
    double oscPhase = 0;
    float taps[kTaps];
    std::complex<float> zin[kTaps];
    int zpos = 0;
    std::complex<float> zring[kSpb];
    int zr = 0;
    double dc = 0;
    // bit sampling
    uint64_t sample = 0;
    uint64_t hist[kSpb] = {};
    float eye[kSpb] = {};
    // collection of the codeword after a sync
    bool collecting = false;
    bool inverted = false;
    int bitsLeft = 0;
    uint64_t nextBit = 0;
    uint64_t cwAcc = 0;
    double lastGoodT = -1e9;
    SyncMode mode = SyncMode::Auto;
    uint64_t stdSyncs = 0;      // sync words matched exactly (either polarity)
    bool tolActive = false;
    int tolVote[2] = {0, 0};   // tolerant framing: codewords seen with bit 1 set in the normal / inverted tone mapping
    static constexpr double kAutoWait = 8.0;   // seconds without any exact sync before the tolerant framing is switched on in Auto mode

    Impl() {
        // Hamming windowed sinc lowpass, cutoff 900 Hz
        double sum = 0;
        double h[kTaps];
        for (int i = 0; i < kTaps; i++) {
            const double x = i - (kTaps - 1) / 2.0;
            const double s = x == 0 ? 2 * 900.0 / kFs : std::sin(2 * kPi * 900.0 * x / kFs) / (kPi * x);
            h[i] = s * (0.54 - 0.46 * std::cos(2 * kPi * i / (kTaps - 1)));
            sum += h[i];
        }
        for (int i = 0; i < kTaps; i++) taps[i] = (float)(h[i] / sum);
        reset();
    }

    void reset() {
        st = Stats();
        events.clear();
        srcPos = 0; prev = 0; havePrev = false;
        oscPhase = 0; zpos = 0; zr = 0; dc = 0;
        for (auto& v : zin) v = {0, 0};
        for (auto& v : zring) v = {0, 0};
        sample = 0;
        for (auto& v : hist) v = 0;
        for (auto& e : eye) e = 0;
        collecting = false;
        lastGoodT = -1e9;
        stdSyncs = 0;
        tolVote[0] = tolVote[1] = 0;
        tolActive = (mode == SyncMode::Tolerant);
    }

    static bool plausible(uint64_t cw) { const int pc = __builtin_popcountll(cw); return pc >= 8 && pc <= 56; }

    void finishCodeword() { handleCodeword(inverted ? ~cwAcc : cwAcc); }

    void handleCodeword(uint64_t cw) {
        collecting = false;
        const double t = (double)sample / kFs;
        if (valid(cw) && (!tolActive || plausible(cw))) {
            st.good++;
            lastGoodT = t;
            if (tolActive) {   // codewords follow each other on the 64 bit grid: carry on without looking for a sync word
                int sys = -1;
                const uint16_t lo = (uint16_t)(cw & 0xFFFF);
                if ((cw >> 63) == 0 && (decodeCcsc(cw, sys) || __builtin_popcount(lo ^ kSync) <= 4)) {
                    if (sys < 0) sys = (int)field(cw, 2, 15);
                    st.ccsc++; st.sys = sys;
                }
                collecting = true; bitsLeft = 64; cwAcc = 0;
            }
            Message m;
            if (decodeAddress(cw, m)) {
                st.messages++;
                events.push_back({t, m});
            }
        } else {
            st.bad++;
        }
    }

    // m: soft value of the bit that ends at this sample (positive: the higher tone); the sample phase is sample mod kSpb
    void onBit(float m) {
        const int ph = (int)(sample % kSpb);
        hist[ph] = (hist[ph] << 1) | (m > 0 ? 1u : 0u);
        eye[ph] += (std::fabs(m) - eye[ph]) / 32.0f;
        if (collecting) {
            if (sample == nextBit) {
                cwAcc = (cwAcc << 1) | (m > 0 ? 1u : 0u);
                nextBit += kSpb;
                if (--bitsLeft == 0) finishCodeword();
            }
            return;
        }
        const uint64_t h = hist[ph];
        const uint16_t lo = (uint16_t)(h & 0xFFFF);
        const bool inv = lo == (uint16_t)~kSync;
        const bool exact = lo == kSync || inv;
        if (exact) stdSyncs++;
        else if (mode == SyncMode::Auto && !tolActive && stdSyncs == 0 && (double)sample / kFs > kAutoWait) tolActive = true;
        st.tolerant = tolActive;
        if (!exact) {
            if (!tolActive) return;
            // sync word not checked: a position is the end of a codeword when the 64 bits up to it have good check bits (either polarity)
            const bool vf = plausible(h) && valid(h), vi = vf;   // silence and constant carriers are valid all-zero codewords
            if (!vf && !vi) return;
            int best2 = ph;
            for (int q = 0; q < kSpb; q++) if (eye[q] > eye[best2] * 1.0001f) best2 = q;
            int d2 = best2 - ph;
            if (d2 > kSpb / 2) d2 -= kSpb;
            if (d2 <= -kSpb / 2) d2 += kSpb;
            // the code contains the all-ones word, so a codeword and its complement are both valid: the polarity is voted from bit 1
            // (address codewords, which dominate a control channel, have it set; the first tone mapping that gives more of them wins)
            tolVote[((h >> 63) & 1) ? 0 : 1]++;
            inverted = tolVote[1] > tolVote[0];
            nextBit = (uint64_t)((int64_t)sample + d2) + kSpb;   // the codeword that just ended is taken now, the next one follows on the grid
            handleCodeword(inverted ? ~h : h);
            return;
        }
        const uint64_t cwPrev = inv ? ~h : h;   // the last 64 bits: a control channel system codeword when this is a real sync
        int sys = 0;
        const bool real = valid(cwPrev) && decodeCcsc(cwPrev, sys);
        const double now = (double)sample / kFs;
        if (!real && now - lastGoodT >= kLockHold) {
            // a bare sync pattern (the start-up sequence has no system codeword before its first address codeword) is taken once the 16 bits
            // before it are the preamble of bit reversals (at most two bits wrong), or once the channel is locked
            const unsigned pre = (unsigned)(((inv ? ~h : h) >> 16) & 0xFFFF) ^ 0xAAAAu;
            if (__builtin_popcount(pre) > 2) return;
        }
        if (real) {
            st.ccsc++;
            st.good++;
            st.sys = sys;
            lastGoodT = now;
        }
        // all sample phases carry the same bit stream: take the following 64 bits at the phase with the widest eye
        int best = ph;
        for (int q = 0; q < kSpb; q++) if (eye[q] > eye[best] * 1.0001f) best = q;
        int d = best - ph;
        if (d > kSpb / 2) d -= kSpb;
        if (d <= -kSpb / 2) d += kSpb;
        inverted = inv;
        collecting = true;
        bitsLeft = 64;
        cwAcc = 0;
        nextBit = (uint64_t)((int64_t)sample + d) + kSpb;   // the first bit after the sync, at the chosen phase
    }

    void audioSample(float a) {   // one sample at kFs
        dc += ((double)a - dc) * (1.0 / (kFs * 2.0));
        const float x = (float)((double)a - dc);
        const double ph = oscPhase;
        oscPhase += 2 * kPi * 1500.0 / kFs;
        if (oscPhase > 2 * kPi) oscPhase -= 2 * kPi;
        zin[zpos] = std::complex<float>(x * (float)std::cos(ph), -x * (float)std::sin(ph));
        std::complex<float> z(0, 0);
        for (int k = 0; k < kTaps; k++) z += zin[(zpos - k + kTaps) % kTaps] * taps[k];
        zpos = (zpos + 1) % kTaps;
        const std::complex<float> old = zring[zr];
        zring[zr] = z;
        zr = (zr + 1) % kSpb;
        const std::complex<float> d = z * std::conj(old);   // phase step over one bit: +90 degrees for the higher tone, -90 for the lower
        onBit(d.imag());
        sample++;
        st.t = (double)sample / kFs;
        st.locked = st.t - lastGoodT < kLockHold;
    }

    void process(const float* a, size_t n, double fs) {
        if (fs != rate) { rate = fs; srcPos = 0; havePrev = false; }
        if (fs == kFs) { for (size_t i = 0; i < n; i++) audioSample(a[i]); return; }
        const double step = fs / kFs;   // input samples per output sample
        for (size_t i = 0; i < n; i++) {
            if (!havePrev) { prev = a[i]; havePrev = true; srcPos = 0; continue; }
            while (srcPos < 1.0) {      // outputs that fall between the previous input and a[i]
                audioSample((float)(prev + (a[i] - prev) * srcPos));
                srcPos += step;
            }
            srcPos -= 1.0;
            prev = a[i];
        }
    }
};

Decoder::Decoder() : p_(new Impl) {}
Decoder::~Decoder() = default;
void Decoder::reset() { p_->reset(); }
void Decoder::setSyncMode(SyncMode m) { p_->mode = m; p_->tolActive = (m == SyncMode::Tolerant); }
SyncMode Decoder::syncMode() const { return p_->mode; }
void Decoder::process(const float* audio, size_t n, double fs) { p_->process(audio, n, fs); }
const Stats& Decoder::stats() const { return p_->st; }
std::vector<Event> Decoder::takeEvents() { std::vector<Event> e; e.swap(p_->events); return e; }

}  // namespace dect2::mpt1327
