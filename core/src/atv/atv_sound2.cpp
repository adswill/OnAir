// Analogue TV second sound: NICAM-728 (ETSI EN 300 163) and A2 / Zweikanalton. See atv_sound2.h.
#include "dect2/atv/atv_sound2.h"
#include "atv_dsp.h"
#include "dect2/dsp/dsp_compat.h"
#include "dect2/dsp/exact_resampler.h"
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstring>

namespace dect2 {

using atvdsp::lowpass;
using dect2::math::kPi;

namespace {

// complex FIR decimator (I and Q kept apart), streaming
struct CDecim {
    std::vector<float> h, hi, hq, xi, xq;
    int d = 1, cnt = 0;
    void design(std::vector<float> t, int dd) { h = std::move(t); d = dd; hi.assign(h.size() - 1, 0.f); hq = hi; cnt = 0; }
    void reset() { std::fill(hi.begin(), hi.end(), 0.f); std::fill(hq.begin(), hq.end(), 0.f); cnt = 0; }
    size_t run(const float* ii, const float* qq, size_t n, float* oi, float* oq) {
        const size_t nt = h.size(), base = nt - 1;
        xi.resize(base + n); xq.resize(base + n);
        memcpy(xi.data(), hi.data(), base * sizeof(float)); memcpy(xi.data() + base, ii, n * sizeof(float));
        memcpy(xq.data(), hq.data(), base * sizeof(float)); memcpy(xq.data() + base, qq, n * sizeof(float));
        size_t o = 0;
        const size_t j0 = (size_t)(d - 1 - cnt);
        if (j0 < n) {
            o = (n - 1 - j0) / (size_t)d + 1;
            desamp(xi.data() + j0, d, h.data(), oi, (int)o, (int)nt);
            desamp(xq.data() + j0, d, h.data(), oq, (int)o, (int)nt);
        }
        cnt = (int)(((size_t)cnt + n) % (size_t)d);
        memcpy(hi.data(), xi.data() + n, base * sizeof(float)); memcpy(hq.data(), xq.data() + n, base * sizeof(float));
        return o;
    }
};

// the carrier moved to zero: x * exp(-j 2 pi f t), the phase continues from block to block
struct Mixer {
    double ph = 0;
    std::vector<float> mi, mq;
    void run(const cf32* x, size_t n, double fHz, double fs) {
        mi.resize(n); mq.resize(n);
        const double w = -2 * kPi * fHz / fs;
        std::complex<double> p = std::polar(1.0, ph);
        const std::complex<double> r = std::polar(1.0, w);
        // blocks of 256: a double phasor per block, float recurrence inside
        size_t i = 0;
        while (i < n) {
            const size_t m = std::min<size_t>(256, n - i);
            std::complex<float> pf((float)p.real(), (float)p.imag());
            const std::complex<float> rf((float)r.real(), (float)r.imag());
            for (size_t k = 0; k < m; k++) {
                const float xr = x[i + k].real(), xi = x[i + k].imag();
                mi[i + k] = xr * pf.real() - xi * pf.imag();
                mq[i + k] = xr * pf.imag() + xi * pf.real();
                pf *= rf;
            }
            p *= std::pow(r, (double)m);
            p /= std::abs(p);
            i += m;
        }
        ph = std::arg(p);
    }
};

// ITU-T J.17 as a one-pole one-zero filter at 32 kHz: de-emphasis G = (sqrt(75) + s') / (1 + s'), s' = s / 3000, bilinear, normalised to 0 dB at 2 kHz
struct J17 {
    double n0, n1, d0, d1, g;
    J17() {
        const double c = 2 * 32000.0 / 3000.0, r = std::sqrt(75.0);
        n0 = r + c; n1 = r - c; d0 = 1 + c; d1 = 1 - c;
        const double w = 2 * kPi * 2000.0 / 32000.0;
        const std::complex<double> z1 = std::polar(1.0, -w);
        g = std::abs((d0 + d1 * z1) / (n0 + n1 * z1));          // 1 / |G(2 kHz)|
    }
};
const J17& j17() { static const J17 f; return f; }

// bits
inline int parity6(uint32_t x) { return (int)(((x >> 4) ^ (x >> 5) ^ (x >> 6) ^ (x >> 7) ^ (x >> 8) ^ (x >> 9)) & 1u); }

// root raised cosine, `sps` samples a symbol, `span` symbols each side, unit energy
std::vector<float> rootRaisedCosine(double beta, int sps, int span) {
    const int n = 2 * span * sps + 1;
    std::vector<float> h((size_t)n);
    double e = 0;
    for (int i = 0; i < n; i++) {
        const double t = (double)(i - span * sps) / sps;
        double v;
        if (std::fabs(t) < 1e-9) v = 1 - beta + 4 * beta / kPi;
        else if (beta > 0 && std::fabs(std::fabs(4 * beta * t) - 1) < 1e-9)
            v = beta / std::sqrt(2.0) * ((1 + 2 / kPi) * std::sin(kPi / (4 * beta)) + (1 - 2 / kPi) * std::cos(kPi / (4 * beta)));
        else v = (std::sin(kPi * t * (1 - beta)) + 4 * beta * t * std::cos(kPi * t * (1 + beta))) / (kPi * t * (1 - (4 * beta * t) * (4 * beta * t)));
        h[(size_t)i] = (float)v;
        e += v * v;
    }
    const float g = (float)(1.0 / std::sqrt(e));
    for (auto& v : h) v *= g;
    return h;
}

const uint8_t kFaw[8] = {0, 1, 0, 0, 1, 1, 1, 0};

} // namespace

// ------------------------------------------------------------------------------------------------ NICAM shared tables

namespace nicam {

void scrambler(uint8_t out[720]) {
    // out[n] = out[n-5] ^ out[n-9], the nine bits before the start are ones: 0000 0111 1011 1110 0010 ...
    uint8_t h[720 + 9];
    for (int i = 0; i < 9; i++) h[i] = 1;
    for (int n = 0; n < 720; n++) { h[n + 9] = (uint8_t)(h[n + 9 - 5] ^ h[n]); out[n] = h[n + 9]; }
}

int deinterleavedIndex(int t) { return (t / 16) + 44 * (t % 16); }

int shiftFromScaleFactor(int r) {
    switch (r & 7) {
    case 7: return 4;
    case 6: return 3;
    case 5: return 2;
    case 3: return 1;
    default: return 0;
    }
}

int shiftForPeak(int peak) {
    if (peak < 512) return 0;
    if (peak < 1024) return 1;
    if (peak < 2048) return 2;
    if (peak < 4096) return 3;
    return 4;
}

int scaleFactorForPeak(int peak) {
    const int s = shiftForPeak(peak);
    if (s == 4) return 7;
    if (s == 3) return 6;
    if (s == 2) return 5;
    if (s == 1) return 3;
    // the fifth coding range, split into protection ranges 5, 6 and 7 by the level of the block
    return peak >= 256 ? 4 : peak >= 64 ? 2 : 1;
}

static void j17Coeffs(double& n0, double& n1, double& d0, double& d1) { const J17& f = j17(); n0 = f.n0; n1 = f.n1; d0 = f.d0; d1 = f.d1; }

float deEmphasis(Emphasis& s, float x) {
    double n0, n1, d0, d1;
    j17Coeffs(n0, n1, d0, d1);
    // G(z) = (n0 + n1 z^-1) / (d0 + d1 z^-1) ... of the form (r + s') / (1 + s'): y = (n0 x + n1 x1 - d1 y1) / d0
    const double y = (n0 * x + n1 * s.x1 - d1 * s.y1) / d0;
    s.x1 = x; s.y1 = y;
    return (float)(y * j17().g);
}

float preEmphasis(Emphasis& s, float x) {
    double n0, n1, d0, d1;
    j17Coeffs(n0, n1, d0, d1);
    // the inverse: (d0 + d1 z^-1) / (n0 + n1 z^-1)
    const double y = (d0 * x + d1 * s.x1 - n1 * s.y1) / n0;
    s.x1 = x; s.y1 = y;
    return (float)(y / j17().g);
}

} // namespace nicam

// ------------------------------------------------------------------------------------------------ NICAM receiver

struct NicamRx::Impl {
    double fs = 0, fs1 = 0;
    int d1 = 1;
    CDecim s1;
    Mixer mix;
    ExactResampler rs;                       // fs1 -> 4 samples a symbol
    bool rsOk = false;
    std::vector<float> t1i, t1q;
    std::vector<cf32> rin, r4;
    // matched filter
    bool wide = false, haveMf = false;
    std::vector<float> rrc;
    std::vector<cf32> rbuf, m;               // resampled input with history, matched filter output
    // symbol timing and carrier
    double pos = 0;
    cf32 sPrev = cf32(1, 0);
    bool havePrev = false;
    double freq = 0, ampEma = 0;
    double theta = 0;                        // phase rotation a symbol that is the carrier offset
    double levelEma = 0;
    std::vector<cf32> symq;
    // frame alignment
    std::vector<uint8_t> bits;
    size_t head = 0;
    bool locked = false;
    int misses = 0;
    uint8_t prbs[720];
    int frameNo = 0;                         // 1 .. 16 within the C0 sequence, 0 unknown
    int lastC0 = -1;
    // audio
    nicam::Emphasis em[2];
    ExactResampler rs48;
    bool rs48Ok = false;
    std::vector<cf32> a32, a48;
    int16_t lastL = 0, lastR = 0;
    NicamStatus st;
    uint64_t goodRun = 0;
    uint64_t absBits = 0;                    // bits pushed since the start
    std::vector<int> rejected;               // frame phases (absolute bit count mod 728) that looked like the frame alignment word but were not
    int c0Run = 0, mode3Run = 0, lockStart = 0;
    uint64_t lastBadMask = 0;                // which sound words of the last frame failed their parity check (diagnostics)
    bool badLock = false;

    Impl() { nicam::scrambler(prbs); }

    void configure(double rate) {
        fs = rate;
        d1 = std::max(1, (int)std::lround(rate / 1.25e6));
        fs1 = rate / d1;
        s1.design(lowpass(250e3, std::min(1.0e6, fs1 - 250e3), rate, 55, 301), d1);
        rsOk = rs.configure(fs1, 4 * nicam::kSymbolRate, 8);
        rs48Ok = rs48.configure(32000.0, 48000.0, 8);
        reset();
    }

    void reset() {
        s1.reset(); mix.ph = 0;
        if (rsOk) rs.reset();
        if (rs48Ok) rs48.reset();
        rbuf.clear(); m.clear(); pos = 3; havePrev = false; sPrev = cf32(1, 0); theta = 0; ampEma = 0; levelEma = 0;
        bits.clear(); head = 0; locked = false; misses = 0; frameNo = 0; lastC0 = -1;
        em[0] = em[1] = nicam::Emphasis();
        lastL = lastR = 0;
        st = NicamStatus();
        goodRun = 0; absBits = 0; rejected.clear(); c0Run = mode3Run = 0; badLock = false;
        haveMf = false;
    }

    void design(bool w) {
        wide = w; haveMf = true;
        rrc = rootRaisedCosine(w ? 1.0 : 0.4, 4, 4);
        rbuf.assign(rrc.size() - 1, cf32(0, 0));
        m.clear(); pos = 3; havePrev = false;
    }

    // ---- the frame
    static int sext10(int v) { return v >= 512 ? v - 1024 : v; }

    void decodeFrame(const uint8_t* f, std::vector<float>& l, std::vector<float>& r, int dualChannel) {
        uint8_t s[720];
        for (int k = 0; k < 720; k++) s[k] = (uint8_t)(f[8 + k] ^ prbs[k]);
        const int c0 = s[0], c1 = s[1], c2 = s[2], c3 = s[3], c4 = s[4];
        // the frame number in the 16 frame sequence of C0
        if (lastC0 == 0 && c0 == 1) frameNo = 1;
        else if (frameNo) frameNo = frameNo % 16 + 1;
        if (c0 == lastC0) c0Run++; else c0Run = 1;
        // C0 is one for 8 frames and zero for 8: a longer run is not a frame (a repeating sound can make the same bits come back at the alignment word's place)
        if (c0Run > 9) badLock = true;
        lastC0 = c0;
        st.reserveFlag = c4 != 0;
        const int mode = c3 ? 3 : (c1 == 0 && c2 == 0) ? 0 : (c1 == 0 && c2 == 1) ? 1 : (c1 == 1 && c2 == 0) ? 2 : 3;
        st.mode = mode;
        st.frames++;
        mode3Run = mode == 3 ? mode3Run + 1 : 0;
        if (mode3Run >= 8) badLock = true;
        if (mode == 3 || !frameNo) return;
        uint8_t fb[704];
        for (int t = 0; t < 704; t++) fb[nicam::deinterleavedIndex(t)] = s[16 + t];
        int val[64], par[64], ok[64];
        for (int w = 0; w < 64; w++) {
            int v = 0;
            for (int b = 0; b < 10; b++) v |= fb[11 * w + b] << b;
            val[w] = v;
            par[w] = fb[11 * w + 10] ^ parity6((uint32_t)v);       // the scale factor bit that was added, or zero
        }
        // the scale factors: a majority over the nine samples that carry each bit
        int sf[2][3] = {{0, 0, 0}, {0, 0, 0}};                         // [block][R2, R1, R0]; stereo: A and B; mono: the two blocks
        int rb[64];
        for (int w = 0; w < 64; w++) rb[w] = 0;
        if (mode == 0) {
            for (int ch = 0; ch < 2; ch++)
                for (int q = 0; q < 3; q++) {
                    int ones = 0;
                    for (int j = 0; j < 9; j++) ones += par[(2 * q + ch) + 6 * j];
                    sf[ch][q] = ones >= 5;
                    for (int j = 0; j < 9; j++) rb[(2 * q + ch) + 6 * j] = sf[ch][q];
                }
        } else {
            for (int blk = 0; blk < 2; blk++)
                for (int q = 0; q < 3; q++) {
                    int ones = 0;
                    for (int j = 0; j < 9; j++) ones += par[blk * 27 + q + 3 * j];
                    sf[blk][q] = ones >= 5;
                    for (int j = 0; j < 9; j++) rb[blk * 27 + q + 3 * j] = sf[blk][q];
                }
        }
        int shift[2];
        for (int b = 0; b < 2; b++) shift[b] = nicam::shiftFromScaleFactor((sf[b][0] << 2) | (sf[b][1] << 1) | sf[b][2]);
        int16_t out[64];
        for (int w = 0; w < 64; w++) {
            ok[w] = par[w] == rb[w];
            const int blk = mode == 0 ? (w & 1) : (w >= 32 ? 1 : 0);
            int16_t v = (int16_t)(sext10(val[w]) * (1 << shift[blk]));
            out[w] = v;
            st.samples++;
            if (!ok[w]) st.parityBad++;
        }
        // frame error rate, smoothed
        int bad = 0;
        lastBadMask = 0;
        for (int w = 0; w < 64; w++) { bad += !ok[w]; if (!ok[w]) lastBadMask |= 1ull << w; }
        st.errorRate += ((float)bad / 64.f - st.errorRate) * 0.02f;
        a32.clear();
        if (mode == 0) {
            for (int k = 0; k < 32; k++) {
                int16_t a = out[2 * k], b = out[2 * k + 1];
                if (!ok[2 * k]) a = lastL; else lastL = a;
                if (!ok[2 * k + 1]) b = lastR; else lastR = b;
                a32.push_back(cf32(nicam::deEmphasis(em[0], (float)a / 8192.f), nicam::deEmphasis(em[1], (float)b / 8192.f)));
            }
        } else {
            // M1 in the odd frames, M2 in the even ones; a single mono channel (with data) is always M1
            const bool m1 = (frameNo & 1) != 0;
            const bool want = mode == 2 ? m1 : (dualChannel == 0 ? m1 : !m1);
            if (!want) return;
            for (int k = 0; k < 64; k++) {
                int16_t a = out[k];
                if (!ok[k]) a = lastL; else lastL = a;
                const float y = nicam::deEmphasis(em[0], (float)a / 8192.f);
                a32.push_back(cf32(y, y));
            }
        }
        a48.clear();
        rs48.process(a32.data(), a32.size(), a48);
        for (const cf32& v : a48) { l.push_back(v.real()); r.push_back(v.imag()); }
    }

    int fawErrors(const uint8_t* p) const { int e = 0; for (int i = 0; i < 8; i++) e += p[i] != kFaw[i]; return e; }

    void pushBit(uint8_t b, std::vector<float>& l, std::vector<float>& r, int dualChannel) {
        bits.push_back(b);
        absBits++;
        const size_t have = bits.size() - head;
        if (locked) {
            if (have >= (size_t)nicam::kFrameBits) {
                const uint8_t* f = bits.data() + head;
                if (fawErrors(f) <= 1) { misses = 0; decodeFrame(f, l, r, dualChannel); }
                else if (++misses >= 4) badLock = true;
                head += nicam::kFrameBits;
                if (badLock) {
                    // not a frame structure after all (or the signal went): forget this alignment and do not take it again for a while
                    badLock = false; locked = false; st.locked = false; st.mode = -1; frameNo = 0; lastC0 = -1; c0Run = mode3Run = 0;
                    if (misses < 4) { rejected.push_back(lockStart); if (rejected.size() > 12) rejected.erase(rejected.begin()); }
                    misses = 0;
                    head = bits.size();
                }
            }
        } else if (have >= 2 * (size_t)nicam::kFrameBits + 8) {
            const uint8_t* f = bits.data() + bits.size() - (2 * nicam::kFrameBits + 8);
            const int phase = (int)((absBits - (uint64_t)(2 * nicam::kFrameBits + 8)) % (uint64_t)nicam::kFrameBits);
            bool skip = false;
            for (int q : rejected) skip = skip || q == phase;
            if (!skip && fawErrors(f) == 0 && fawErrors(f + nicam::kFrameBits) == 0 && fawErrors(f + 2 * nicam::kFrameBits) == 0) {
                lockStart = phase;
                locked = true; misses = 0; st.locked = true; st.frames = 0; st.samples = 0; st.parityBad = 0; st.errorRate = 0;
                head = (size_t)(f - bits.data());
                frameNo = 0; lastC0 = -1;
                em[0] = em[1] = nicam::Emphasis();
                // the two frames already in the buffer are decoded now
                while (bits.size() - head >= (size_t)nicam::kFrameBits) {
                    decodeFrame(bits.data() + head, l, r, dualChannel);
                    head += nicam::kFrameBits;
                }
            } else if (have > 4000) head = bits.size() - 2 * (size_t)nicam::kFrameBits - 8;
        }
        if (head > 8192) { bits.erase(bits.begin(), bits.begin() + (long)head); head = 0; }
    }

    // ---- symbols
    // cubic (four point Lagrange) interpolation between v[i] and v[i+1], i >= 1 and i + 2 < size
    static cf32 lerp(const std::vector<cf32>& v, double x) {
        const long i = (long)std::floor(x);
        const float f = (float)(x - (double)i);
        const cf32 y0 = v[(size_t)i - 1], y1 = v[(size_t)i], y2 = v[(size_t)i + 1], y3 = v[(size_t)i + 2];
        const float c0 = -f * (f - 1) * (f - 2) / 6, c1 = (f + 1) * (f - 1) * (f - 2) / 2, c2 = -(f + 1) * f * (f - 2) / 2, c3 = (f + 1) * f * (f - 1) / 6;
        return y0 * c0 + y1 * c1 + y2 * c2 + y3 * c3;
    }

    void symbols(std::vector<float>& l, std::vector<float>& r, int dualChannel) {
        const double kp = 0.01, ki = 0.00003;
        while (pos + 3 < (double)m.size() && pos >= 3) {
            const cf32 s0 = lerp(m, pos), mid = lerp(m, pos - 2);
            ampEma = ampEma > 0 ? ampEma + (std::abs(s0) - ampEma) * 0.002 : std::abs(s0);
            const float a = (float)std::max(ampEma, 1e-9);
            const cf32 s = s0 / a;
            if (havePrev) {
                // Gardner timing error; late sampling gives a positive value
                const double e = std::max(-1.0, std::min(1.0, (double)((s - sPrev).real() * (mid / a).real() + (s - sPrev).imag() * (mid / a).imag())));
                freq += ki * e;
                freq = std::max(-0.3, std::min(0.3, freq));
                pos += 4.0 - kp * e - freq;
                // the phase step of this symbol, less the carrier offset, to the nearest quarter turn
                const cf32 dd = s * std::conj(sPrev);
                double ang = std::atan2((double)dd.imag(), (double)dd.real()) - theta;
                const double q = std::round(ang / (kPi / 2));
                double res = ang - q * (kPi / 2);
                theta += 0.0005 * res;
                theta = std::max(-0.6, std::min(0.6, theta));
                int k = ((int)q % 4 + 4) % 4;
                // 0: 00, -90 degrees: 01, +90 degrees (-270): 10, 180: 11
                uint8_t b0, b1;
                if (k == 0) { b0 = 0; b1 = 0; } else if (k == 3) { b0 = 0; b1 = 1; } else if (k == 1) { b0 = 1; b1 = 0; } else { b0 = 1; b1 = 1; }
                pushBit(b0, l, r, dualChannel);
                pushBit(b1, l, r, dualChannel);
                levelEma += (std::norm(s0) - levelEma) * 0.001;
            } else pos += 4.0;
            sPrev = s; havePrev = true;
        }
        // drop what is behind us
        const long drop = (long)std::floor(pos) - 6;
        if (drop > 4096) { m.erase(m.begin(), m.begin() + drop); pos -= (double)drop; }
    }

    void process(const cf32* x, size_t n, double carrierHz, bool w, int dualChannel, std::vector<float>& l, std::vector<float>& r) {
        if (!rsOk || !rs48Ok || n == 0) return;
        if (!haveMf || w != wide) design(w);
        mix.run(x, n, carrierHz, fs);
        t1i.resize(n / (size_t)d1 + 4); t1q.resize(t1i.size());
        const size_t n1 = s1.run(mix.mi.data(), mix.mq.data(), n, t1i.data(), t1q.data());
        if (!n1) return;
        rin.resize(n1);
        for (size_t i = 0; i < n1; i++) rin[i] = cf32(t1i[i], t1q[i]);
        r4.clear();
        rs.process(rin.data(), n1, r4);
        if (r4.empty()) return;
        // matched filter on the resampled stream
        const size_t nt = rrc.size();
        const size_t old = rbuf.size();
        rbuf.insert(rbuf.end(), r4.begin(), r4.end());
        const size_t outN = rbuf.size() - (nt - 1);
        (void)old;
        const size_t base = m.size();
        m.resize(base + outN);
        for (size_t k = 0; k < outN; k++) {
            float ar = 0, ai = 0;
            const cf32* p = rbuf.data() + k;
            for (size_t i = 0; i < nt; i++) { ar += rrc[i] * p[i].real(); ai += rrc[i] * p[i].imag(); }
            m[base + k] = cf32(ar, ai);
        }
        rbuf.erase(rbuf.begin(), rbuf.begin() + (long)outN);
        // apply the carrier-offset correction slowly through theta: it is done in the symbol decision
        symbols(l, r, dualChannel);
        st.levelDb = levelEma > 1e-12 ? (float)(10 * std::log10(levelEma)) : -120.f;
        st.carrierErrHz = theta / (2 * kPi) * nicam::kSymbolRate;
    }
};

void NicamRx::configure(double fs) { p_ = std::make_shared<Impl>(); if (fs >= 7.9e6) p_->configure(fs); }
void NicamRx::reset() { if (p_ && p_->fs > 0) p_->reset(); }
void NicamRx::process(const cf32* x, size_t n, double carrierHz, bool wide, int dualChannel, std::vector<float>& l, std::vector<float>& r) {
    if (p_ && p_->fs > 0) p_->process(x, n, carrierHz, wide, dualChannel, l, r);
}
NicamStatus NicamRx::status() const { return p_ ? p_->st : NicamStatus(); }
bool NicamRx::audioValid() const {
    if (!p_) return false;
    const NicamStatus& s = p_->st;
    return s.locked && s.frames > 40 && s.errorRate < 0.15f && s.mode >= 0 && s.mode <= 2;
}

// ------------------------------------------------------------------------------------------------ NICAM transmitter

struct NicamTx::Impl {
    NicamTxConfig c;
    bool ok = false;
    uint8_t prbs[720];
    std::vector<float> rrc;                  // 16 samples a symbol
    cf32 hist[9];                            // the last nine symbols
    int hp = 0;
    double phaseAcc = 0;                     // DQPSK phase in quarter turns, 0 .. 3
    int quarter = 0;
    ExactResampler rs;
    std::vector<cf32> queue, tmp;
    size_t qpos = 0;
    uint64_t frameNo = 0;
    uint64_t sampleL = 0, sampleR = 0;       // sample counters of the two audio channels
    nicam::Emphasis em[2];
    double carPh = 0;
    double scale = 1;

    explicit Impl(const NicamTxConfig& cc) : c(cc) {
        nicam::scrambler(prbs);
        rrc = rootRaisedCosine(c.wide ? 1.0 : 0.4, 16, 4);
        for (auto& h : rrc) h *= 4.f;                         // unit energy over 16 samples a symbol: x4 makes the shaped signal unit rms
        for (auto& h : hist) h = cf32(0, 0);
        ok = rs.configure(16 * nicam::kSymbolRate, c.rate, 8);
    }

    int16_t toneSample(int ch, uint64_t n) {
        const double f = ch == 0 ? c.toneLHz : c.toneRHz;
        if (f <= 0) return 0;
        const double v = c.amp * std::sin(2 * kPi * f * (double)n / 32000.0);
        const float y = nicam::preEmphasis(em[ch], (float)v);
        return (int16_t)std::lround(std::max(-1.0f, std::min(1.0f, y)) * 8191.f);
    }

    // one frame of 728 bits
    void frame(uint8_t* f) {
        const uint64_t fn = frameNo++;                       // 0 based; the C0 sequence: 8 frames of ones, then 8 of zeros
        const int c0 = ((fn / 8) & 1) ? 0 : 1;
        const int fno = (int)(fn % 16) + 1;                  // frame number 1 .. 16 of the C0 sequence
        const bool stereo = c.mode == 0;
        int16_t smp[64];
        if (stereo) for (int k = 0; k < 32; k++) { smp[2 * k] = toneSample(0, sampleL++); smp[2 * k + 1] = toneSample(1, sampleR++); }
        else {
            const int ch = (fno & 1) ? 0 : 1;                 // M1 in the odd frames
            for (int k = 0; k < 64; k++) smp[k] = toneSample(ch, ch == 0 ? sampleL++ : sampleR++);
        }
        // blocks of 32 samples: scale factors, 10 bit words, parity
        int shift[2], sf[2];
        for (int b = 0; b < 2; b++) {
            int peak = 0;
            for (int k = 0; k < 32; k++) peak = std::max(peak, std::abs((int)smp[stereo ? 2 * k + b : 32 * b + k]));
            shift[b] = nicam::shiftForPeak(peak);
            sf[b] = nicam::scaleFactorForPeak(peak);
        }
        uint8_t fb[704];
        for (int w = 0; w < 64; w++) {
            const int blk = stereo ? (w & 1) : (w >= 32 ? 1 : 0);
            const int v10 = ((int)smp[w] >> shift[blk]) & 0x3FF;
            uint32_t u = (uint32_t)v10;
            int p = parity6(u);
            // the scale factor bit that goes into this parity bit
            int rbit = 0;
            if (w < 54) {
                if (stereo) { const int k = w % 6; rbit = (sf[k & 1] >> (2 - k / 2)) & 1; }
                else if (w < 27) rbit = (sf[0] >> (2 - w % 3)) & 1;
                else rbit = (sf[1] >> (2 - (w - 27) % 3)) & 1;
            }
            p ^= rbit;
            for (int b = 0; b < 10; b++) fb[11 * w + b] = (uint8_t)((u >> b) & 1);
            fb[11 * w + 10] = (uint8_t)p;
        }
        uint8_t s[720];
        s[0] = (uint8_t)c0;
        s[1] = stereo ? 0 : 0; s[2] = stereo ? 0 : 1; s[3] = 0; s[4] = 1;                 // C1 C2 C3: stereo 000, dual 010; C4: the analogue sound carries the same programme
        for (int k = 5; k < 16; k++) s[k] = 0;                                                 // additional data
        for (int t = 0; t < 704; t++) s[16 + t] = fb[nicam::deinterleavedIndex(t)];
        for (int i = 0; i < 8; i++) f[i] = kFaw[i];
        for (int k = 0; k < 720; k++) f[8 + k] = (uint8_t)(s[k] ^ prbs[k]);
    }

    // a frame of symbols, shaped at 16 samples a symbol, to the output rate
    void more() {
        uint8_t f[728];
        frame(f);
        std::vector<cf32> shaped;
        shaped.reserve(364 * 16);
        for (int k = 0; k < 364; k++) {
            const uint8_t a = f[2 * k], b = f[2 * k + 1];
            // 00: 0, 01: -90, 10: -270 (+90), 11: -180
            const int step = (a == 0 && b == 0) ? 0 : (a == 0 && b == 1) ? 3 : (a == 1 && b == 0) ? 1 : 2;
            quarter = (quarter + step) & 3;
            static const cf32 pt[4] = {cf32(1, 0), cf32(0, 1), cf32(-1, 0), cf32(0, -1)};
            hist[hp] = pt[quarter];
            hp = (hp + 1) % 9;
            // the pulse of each of the last nine symbols at this symbol's 16 phases
            for (int ph = 0; ph < 16; ph++) {
                cf32 acc(0, 0);
                for (int j = 0; j < 9; j++) {
                    // symbol j symbols back: tap index 16 * j + ph (the pulse spans 4 symbols each side: 129 taps; the newest symbol is at the centre)
                    const cf32 sy = hist[(hp - 1 - j + 18) % 9];
                    const int idx = 16 * j + ph;
                    if (idx < (int)rrc.size()) acc += sy * rrc[(size_t)idx];
                }
                shaped.push_back(acc);
            }
        }
        rs.process(shaped.data(), shaped.size(), queue);
    }

    void add(cf32* out, size_t n, double carrierHz) {
        if (!ok) return;
        while (queue.size() - qpos < n) more();
        const double w = 2 * kPi * carrierHz / c.rate;
        std::complex<double> p = std::polar(1.0, carPh);
        const std::complex<double> r = std::polar(1.0, w);
        size_t i = 0;
        while (i < n) {
            const size_t mlen = std::min<size_t>(256, n - i);
            std::complex<float> pf((float)p.real(), (float)p.imag());
            const std::complex<float> rf((float)r.real(), (float)r.imag());
            const float lv = (float)c.level;
            for (size_t k = 0; k < mlen; k++) { out[i + k] += queue[qpos + i + k] * pf * lv; pf *= rf; }
            p *= std::pow(r, (double)mlen);
            p /= std::abs(p);
            i += mlen;
        }
        carPh = std::arg(p);
        qpos += n;
        if (qpos > 65536) { queue.erase(queue.begin(), queue.begin() + (long)qpos); qpos = 0; }
    }
};

NicamTx::NicamTx(const NicamTxConfig& c) : p_(std::make_unique<Impl>(c)) {}
NicamTx::~NicamTx() = default;
void NicamTx::add(cf32* out, size_t n, double carrierHz) { p_->add(out, n, carrierHz); }

// ------------------------------------------------------------------------------------------------ A2 receiver

namespace {

// one FM carrier: mixer, two decimating filters to about 250 kHz, discriminator, de-emphasis, 15 kHz low-pass, 48 kHz audio
struct FmChain {
    double fs = 0, fs1 = 0, fs2 = 0;
    int d1 = 1, d2 = 5, d3 = 5;
    Mixer mix;
    CDecim s1, s2, s3;
    ExactResampler rs;
    bool ok = false;
    std::vector<float> t1i, t1q, t2i, t2q, t3i, t3q, disc, aud, zero;
    std::vector<cf32> rin, rout;
    cf32 prev = cf32(1, 0);
    double deemY = 0, deemX1 = 0;
    double carrierEnv = 0, envSq = 0; size_t envN = 0;

    void configure(double rate) {
        fs = rate;
        d1 = std::max(1, (int)std::lround(rate / 1.25e6));
        fs1 = rate / d1;
        d2 = std::max(1, (int)std::lround(fs1 / 250e3));
        fs2 = fs1 / d2;
        s1.design(lowpass(250e3, std::min(1.0e6, fs1 - 250e3), rate, 55, 301), d1);
        s2.design(lowpass(100e3, std::min(150e3, fs2 - 100e3), fs1, 55, 301), d2);
        d3 = std::max(1, (int)std::lround(fs2 / 50e3));
        s3.design(lowpass(15e3, 22e3, fs2, 50, 301), d3);
        ok = rs.configure(fs2 / d3, 48000.0, 8);
        reset();
    }
    void reset() {
        s1.reset(); s2.reset(); s3.reset(); mix.ph = 0; if (ok) rs.reset(); prev = cf32(1, 0); deemY = deemX1 = 0; envSq = 0; envN = 0;
    }
    // appends 48 kHz audio (full deviation = 1) to `out`; `discOut` (if not null) gets the raw discriminator at fs2 (full deviation = 1)
    size_t run(const cf32* x, size_t n, double fc, double devHz, double tauUs, std::vector<float>& out, std::vector<float>* discOut) {
        if (!ok) return 0;
        mix.run(x, n, fc, fs);
        t1i.resize(n / (size_t)d1 + 4); t1q.resize(t1i.size());
        const size_t n1 = s1.run(mix.mi.data(), mix.mq.data(), n, t1i.data(), t1q.data());
        t2i.resize(n1 / (size_t)d2 + 4); t2q.resize(t2i.size());
        const size_t n2 = s2.run(t1i.data(), t1q.data(), n1, t2i.data(), t2q.data());
        if (!n2) return 0;
        disc.resize(n2); aud.resize(n2);
        for (size_t j = 0; j < n2; j++) {
            const cf32 z(t2i[j], t2q[j]);
            const cf32 pr = z * std::conj(prev);
            prev = z;
            disc[j] = (float)(std::atan2((double)pr.imag(), (double)pr.real()) * fs2 / (2 * kPi) / devHz);
            envSq += std::norm(z); envN++;
        }
        if (discOut) discOut->insert(discOut->end(), disc.begin(), disc.end());
        // de-emphasis of 1 / (1 + s tau), bilinear
        const double k = 2 * fs2 * tauUs * 1e-6, a = (1 - k) / (1 + k), kk = (1 + a) / 2;
        for (size_t j = 0; j < n2; j++) {
            const double xin = disc[j];
            if (tauUs <= 0) { aud[j] = (float)xin; continue; }
            deemY = kk * (xin + deemX1) - a * deemY;
            deemX1 = xin;
            aud[j] = (float)deemY;
        }
        zero.assign(n2, 0.f);
        t3i.resize(n2 / (size_t)d3 + 4); t3q.resize(t3i.size());
        const size_t n3 = s3.run(aud.data(), zero.data(), n2, t3i.data(), t3q.data());
        rin.resize(n3);
        for (size_t j = 0; j < n3; j++) rin[j] = cf32(t3i[j], 0.f);
        rout.clear();
        rs.process(rin.data(), n3, rout);
        for (const cf32& v : rout) out.push_back(v.real());
        return rout.size();
    }
};

// Goertzel magnitude of x[0..n) at f (Hz) for the sample rate fsr
double goertzel(const std::vector<float>& x, double f, double fsr) {
    const double w = 2 * kPi * f / fsr, c = 2 * std::cos(w);
    double s1 = 0, s2 = 0;
    for (float v : x) { const double s0 = v + c * s1 - s2; s2 = s1; s1 = s0; }
    return std::sqrt(std::max(0.0, s1 * s1 + s2 * s2 - c * s1 * s2));
}

} // namespace

struct A2Rx::Impl {
    double fs = 0, fs2 = 0;
    FmChain c1, c2;
    std::vector<float> a1, a2, disc2;
    // pilot analysis: the discriminator of carrier 2 mixed to baseband at the pilot (and at a noise reference), boxcar, envelope
    struct Branch {
        double ph = 0, w = 0; int dec = 1, cnt = 0;
        std::complex<float> acc1 = 0, acc2 = 0;       // two cascaded boxcars (running sums by decimation block)
        double sum1r = 0, sum1i = 0, sum2r = 0, sum2i = 0;
        std::vector<float> env;
        double envSum = 0; size_t envCnt = 0;
    } pil, noi;
    std::vector<float> win;                           // envelope of the pilot at 5 kHz, a window
    double winDc = 0, noiDc = 0; int winN = 0;
    A2Status st;
    int cand = -1, candRun = 0;
    double levelEma = 0;

    void configure(double rate) {
        fs = rate;
        c1.configure(rate); c2.configure(rate);
        fs2 = c2.fs2;
        const int dec = std::max(1, (int)std::lround(fs2 / 5000.0));
        pil.dec = noi.dec = dec;
        pil.w = -2 * kPi * a2::kPilotHz / fs2;
        noi.w = -2 * kPi * 80000.0 / fs2;
        reset();
    }
    void reset() {
        c1.reset(); c2.reset();
        for (Branch* b : {&pil, &noi}) { b->ph = 0; b->cnt = 0; b->sum1r = b->sum1i = b->sum2r = b->sum2i = 0; b->env.clear(); }
        win.clear(); winDc = noiDc = 0; winN = 0; st = A2Status(); cand = -1; candRun = 0;
        pilEnvPrevR = pilEnvPrevI = 0;
    }
    double pilEnvPrevR = 0, pilEnvPrevI = 0;

    // block mean over `dec` samples, twice (a triangle of 2 dec taps): done as running block means
    void branch(Branch& b, const std::vector<float>& d, std::vector<float>& envOut) {
        envOut.clear();
        for (float v : d) {
            const std::complex<double> z((double)v * std::cos(b.ph), (double)v * std::sin(b.ph));
            b.ph += b.w;
            if (b.ph < -2 * kPi) b.ph += 2 * kPi;
            b.sum1r += z.real(); b.sum1i += z.imag();
            if (++b.cnt >= b.dec) {
                const double r1 = b.sum1r / b.dec, i1 = b.sum1i / b.dec;
                b.sum1r = b.sum1i = 0; b.cnt = 0;
                // second boxcar over two decimated values
                const double r2 = 0.5 * (r1 + (&b == &pil ? pilEnvPrevR : b.sum2r)), i2 = 0.5 * (i1 + (&b == &pil ? pilEnvPrevI : b.sum2i));
                if (&b == &pil) { pilEnvPrevR = r1; pilEnvPrevI = i1; } else { b.sum2r = r1; b.sum2i = i1; }
                envOut.push_back((float)std::hypot(r2, i2));
            }
        }
    }

    std::vector<float> envP, envN;
    void pilot() {
        branch(pil, disc2, envP);
        branch(noi, disc2, envN);
        for (size_t i = 0; i < envP.size() && i < envN.size(); i++) {
            win.push_back(envP[i]);
            noiDc += envN[i];
            winDc += envP[i];
            if (++winN >= (int)std::lround(0.1 * fs2 / pil.dec)) {         // 100 ms
                const double dc = winDc / winN, nz = noiDc / winN;
                const double fsr = fs2 / pil.dec;
                std::vector<float> w0(win);
                for (auto& v : w0) v = (float)(v - dc);
                const double m117 = dc > 0 ? 2 * goertzel(w0, a2::kStereoToneHz, fsr) / ((double)winN * dc) : 0;
                const double m274 = dc > 0 ? 2 * goertzel(w0, a2::kDualToneHz, fsr) / ((double)winN * dc) : 0;
                const bool present = dc > 1.8 * nz && dc > 0.004;
                int mode = -1;
                if (present) mode = (m117 > 0.2 && m117 > 2 * m274) ? 1 : (m274 > 0.2 && m274 > 2 * m117) ? 2 : 0;
                st.pilotDb = (float)(20 * std::log10(std::max(dc, 1e-9) / std::max(nz, 1e-9)));
                st.carrier2 = present;
                if (mode == cand) candRun++; else { cand = mode; candRun = 1; }
                if (candRun >= 2 && st.mode != cand) st.mode = cand;
                win.clear(); winDc = noiDc = 0; winN = 0;
            }
        }
    }

    void process(const cf32* x, size_t n, double f1, double devKhz, double tauUs, std::vector<float>& l, std::vector<float>& r) {
        if (!c1.ok || !c2.ok || n == 0) return;
        a1.clear(); a2.clear(); disc2.clear();
        const double dev = devKhz * 1e3;
        c1.run(x, n, f1, dev, tauUs, a1, nullptr);
        c2.run(x, n, f1 + a2::kCarrier2OffsetHz, dev, tauUs, a2, &disc2);
        pilot();
        const size_t m = std::min(a1.size(), a2.size());
        const int mode = st.carrier2 ? st.mode : -1;
        for (size_t i = 0; i < m; i++) {
            if (mode == 1) { l.push_back(2 * a1[i] - a2[i]); r.push_back(a2[i]); }
            else if (mode == 2) { l.push_back(a1[i]); r.push_back(a2[i]); }
            else { l.push_back(a1[i]); r.push_back(a1[i]); }
        }
        if (c2.envN >= (size_t)(0.05 * fs2)) { st.levelDb = (float)(10 * std::log10(std::max(c2.envSq / (double)c2.envN, 1e-12))); c2.envSq = 0; c2.envN = 0; }
    }
};

void A2Rx::configure(double fs) { p_ = std::make_shared<Impl>(); if (fs >= 7.9e6) p_->configure(fs); }
void A2Rx::reset() { if (p_ && p_->fs > 0) p_->reset(); }
void A2Rx::process(const cf32* x, size_t n, double carrier1Hz, double devKhz, double preEmphUs, std::vector<float>& l, std::vector<float>& r) {
    if (p_ && p_->fs > 0) p_->process(x, n, carrier1Hz, devKhz, preEmphUs, l, r);
}
A2Status A2Rx::status() const { return p_ ? p_->st : A2Status(); }

// ------------------------------------------------------------------------------------------------ A2 transmitter

struct A2Tx::Impl {
    A2TxConfig c;
    std::complex<double> toneL, toneR, stepL, stepR, pilot, pilotStep, modOsc, modStep;
    double ph1 = 0, ph2 = 0;
    uint64_t n = 0;
    double gainL, gainR;
    explicit Impl(const A2TxConfig& cc) : c(cc) {
        auto g = [&](double f) { const double w = 2 * kPi * f * c.preUs * 1e-6; return std::sqrt(1 + w * w); };
        gainL = g(c.toneLHz); gainR = g(c.toneRHz);
        toneL = toneR = pilot = modOsc = 1.0;
        stepL = std::polar(1.0, 2 * kPi * c.toneLHz / c.rate);
        stepR = std::polar(1.0, 2 * kPi * c.toneRHz / c.rate);
        pilotStep = std::polar(1.0, 2 * kPi * a2::kPilotHz / c.rate);
        const double fm = c.mode == 1 ? a2::kStereoToneHz : c.mode == 2 ? a2::kDualToneHz : 0;
        modStep = std::polar(1.0, 2 * kPi * fm / c.rate);
    }
    void add(cf32* out, size_t cnt, double f1) {
        const double w1 = 2 * kPi * f1 / c.rate, w2 = 2 * kPi * (f1 + a2::kCarrier2OffsetHz) / c.rate;
        const double dk = 2 * kPi * a2::kDevHz / c.rate;
        for (size_t i = 0; i < cnt; i++) {
            const double sl = c.amp * gainL * toneL.imag(), sr = c.amp * gainR * toneR.imag();
            // second carrier: R (stereo), sound B (dual) or nothing but the pilot (mono)
            const double m2 = (c.mode == 0 ? 0.0 : sr);
            const double pa = (c.mode == 0 ? 1.0 : 1.0 + 0.5 * modOsc.imag());
            const double pil = 0.05 * pa * pilot.imag();
            ph2 += w2 + dk * (m2 + pil);
            out[i] += cf32((float)(c.level2 * std::cos(ph2)), (float)(c.level2 * std::sin(ph2)));
            if (c.withFirst) {
                const double m1 = c.mode == 1 ? 0.5 * (sl + sr) : sl;
                ph1 += w1 + dk * m1;
                out[i] += cf32((float)(c.level1 * std::cos(ph1)), (float)(c.level1 * std::sin(ph1)));
            }
            toneL *= stepL; toneR *= stepR; pilot *= pilotStep; modOsc *= modStep;
            if ((++n & 4095) == 0) { toneL /= std::abs(toneL); toneR /= std::abs(toneR); pilot /= std::abs(pilot); modOsc /= std::abs(modOsc); }
            if ((n & 0xFFFFF) == 0) { ph1 = std::fmod(ph1, 2 * kPi); ph2 = std::fmod(ph2, 2 * kPi); }
        }
    }
};

A2Tx::A2Tx(const A2TxConfig& c) : p_(std::make_unique<Impl>(c)) {}
A2Tx::~A2Tx() = default;
void A2Tx::add(cf32* out, size_t n, double carrier1Hz) { p_->add(out, n, carrier1Hz); }
double A2Tx::firstCarrierAudio(const A2TxConfig& c, double t) {
    auto g = [&](double f) { const double w = 2 * kPi * f * c.preUs * 1e-6; return std::sqrt(1 + w * w); };
    const double sl = c.amp * g(c.toneLHz) * std::sin(2 * kPi * c.toneLHz * t), sr = c.amp * g(c.toneRHz) * std::sin(2 * kPi * c.toneRHz * t);
    return c.mode == 1 ? 0.5 * (sl + sr) : sl;
}

} // namespace dect2
