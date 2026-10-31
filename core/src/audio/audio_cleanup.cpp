// Audio cleanup chain (audio_cleanup.h). Own implementation; algorithms from the literature, no code taken from elsewhere:
//  - noise tracking: R. Martin, "Noise Power Spectral Density Estimation Based on Optimal Smoothing and Minimum Statistics", IEEE Trans.
//    Speech Audio Process. 9(5), 2001 (smoothed periodogram, minimum over sliding sub-windows, fixed bias factor)
//  - gain: Wiener rule with the decision-directed a priori SNR, Y. Ephraim, D. Malah, IEEE Trans. ASSP 32(6), 1984
//  - notch/band filters: the trapezoidal state-variable filter, A. Simper (Cytomic) "Linear Trapezoidal Integrated SVF"
//  - biquads: R. Bristow-Johnson, "Audio EQ Cookbook"
#include "dect2/audio/audio_cleanup.h"
#include "dect2/audio/neural_nr.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace dect2 {
namespace {

constexpr float kPi = 3.14159265358979f;
constexpr float kTiny = 1e-25f;   // keeps recursive filters out of the denormal range

inline float dbToLin(float db) { return std::pow(10.f, db * 0.05f); }
inline float onePole(float tauSec, float rate) { return 1.f - std::exp(-1.f / std::max(1e-6f, tauSec * rate)); }
inline float softLim(float x) {
    const float a = std::fabs(x), k = 0.8f;
    if (a <= k) return x;
    const float y = k + (1.f - k) * std::tanh((a - k) / (1.f - k));
    return x < 0 ? -y : y;
}

struct Fft {   // radix-2, in place, forward (e^-j); the inverse is run(im, re) / n
    int n = 0, lg = 0;
    std::vector<float> wr, wi;
    std::vector<uint16_t> rev;
    void init(int size) {
        n = size; lg = 0; while ((1 << lg) < n) lg++;
        wr.resize(n / 2); wi.resize(n / 2); rev.resize(n);
        for (int i = 0; i < n / 2; i++) { wr[i] = std::cos(2 * kPi * i / n); wi[i] = -std::sin(2 * kPi * i / n); }
        for (int i = 0; i < n; i++) { int r = 0; for (int b = 0; b < lg; b++) if (i & (1 << b)) r |= 1 << (lg - 1 - b); rev[i] = (uint16_t)r; }
    }
    void run(float* re, float* im) const {
        for (int i = 0; i < n; i++) { const int j = rev[i]; if (j > i) { std::swap(re[i], re[j]); std::swap(im[i], im[j]); } }
        for (int len = 2; len <= n; len <<= 1) {
            const int half = len / 2, step = n / len;
            for (int s = 0; s < n; s += len)
                for (int k = 0; k < half; k++) {
                    const float cr = wr[k * step], ci = wi[k * step];
                    const int a = s + k, b = a + half;
                    const float tr = re[b] * cr - im[b] * ci, ti = re[b] * ci + im[b] * cr;
                    re[b] = re[a] - tr; im[b] = im[a] - ti; re[a] += tr; im[a] += ti;
                }
        }
    }
};

struct Svf {   // trapezoidal SVF: band-pass with peak gain 1/k; stable while its frequency moves
    float ic1 = 0, ic2 = 0, a1 = 0, a2 = 0, a3 = 0, k = 1;
    void set(float f, float q, float rate) {
        const float g = std::tan(kPi * std::min(f, 0.45f * rate) / rate);
        k = 1.f / q; a1 = 1.f / (1.f + g * (g + k)); a2 = g * a1; a3 = g * a2;
    }
    float bp(float v0) {   // unity-peak band-pass
        const float v3 = v0 - ic2, v1 = a1 * ic1 + a2 * v3, v2 = ic2 + a2 * ic1 + a3 * v3;
        ic1 = 2 * v1 - ic1 + kTiny; ic2 = 2 * v2 - ic2;
        return k * v1;
    }
    void reset() { ic1 = ic2 = 0; }
};

struct Biquad {
    float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
    void set(float nb0, float nb1, float nb2, float na0, float na1, float na2) { b0 = nb0 / na0; b1 = nb1 / na0; b2 = nb2 / na0; a1 = na1 / na0; a2 = na2 / na0; }
    void highpass(float f, float q, float rate) {
        const float w = 2 * kPi * f / rate, c = std::cos(w), al = std::sin(w) / (2 * q);
        set((1 + c) / 2, -(1 + c), (1 + c) / 2, 1 + al, -2 * c, 1 - al);
    }
    void peaking(float f, float q, float gainDb, float rate) {
        const float A = std::pow(10.f, gainDb / 40.f), w = 2 * kPi * f / rate, c = std::cos(w), al = std::sin(w) / (2 * q);
        set(1 + al * A, -2 * c, 1 - al * A, 1 + al / A, -2 * c, 1 - al / A);
    }
    float run(float x) { const float y = b0 * x + z1; z1 = b1 * x - a1 * y + z2 + kTiny; z2 = b2 * x - a2 * y; return y; }
    void reset() { z1 = z2 = 0; }
};

// Smooth 0..1 amount toward a target (per sample)
struct Ramp {
    float v = 0;
    float step(float target, float c) { v += (target - v) * c; return v; }
    bool idle(float target) const { return target <= 0.f && v < 1e-4f; }
};

// ---------------------------------------------------------------- noise reduction (per channel)
struct SpecNr {
    int N = 0, hop = 0, K = 0, V = 0;
    static constexpr int U = 6;                 // sub-windows in the minimum search: U * V frames (about 1.5 s)
    Fft fft;
    std::vector<float> win, inRing, ola, dry, re, im;
    std::vector<float> pbar, curMin, histMin, noise, gprev, aprev, gain, gsm;
    std::vector<float> hist;                    // U x K
    int wr = 0, rd = 0, dp = 0, cnt = 0, subCount = 0, hpos = 0;
    bool primed = false, histFull = false;
    float meanGainDb = 0;
    static constexpr float kBias = 1.9f;        // the minimum of a smoothed periodogram sits below the mean: compensation (calibrated in test_audio_cleanup)

    void init(int n) {
        N = n; hop = n / 2; K = n / 2 + 1; fft.init(n);
        win.resize(n); for (int i = 0; i < n; i++) win[i] = std::sin(kPi * (i + 0.5f) / n);
        inRing.assign(n, 0.f); ola.assign(n, 0.f); dry.assign(n, 0.f); re.assign(n, 0.f); im.assign(n, 0.f);
        pbar.assign(K, 0.f); curMin.assign(K, 0.f); histMin.assign(K, 0.f); noise.assign(K, 0.f); gprev.assign(K, 1.f); aprev.assign(K, 0.f);
        gain.assign(K, 1.f); gsm.assign(K, 1.f); hist.assign((size_t)U * K, 0.f);
    }
    void setRate(float rate) { V = std::max(4, (int)std::lround(0.25f * rate / hop)); }
    void reset() {
        std::fill(inRing.begin(), inRing.end(), 0.f); std::fill(ola.begin(), ola.end(), 0.f); std::fill(dry.begin(), dry.end(), 0.f);
        std::fill(gprev.begin(), gprev.end(), 1.f); std::fill(aprev.begin(), aprev.end(), 0.f);
        wr = rd = dp = cnt = subCount = hpos = 0; primed = false; histFull = false;
    }
    // One 10 ms-ish frame: estimate noise, compute gains, resynthesise. s = strength 0..1; active = also apply (else only keep the delay)
    void frame(float s, bool track) {
        if (!track) return;
        for (int i = 0; i < N; i++) { re[i] = inRing[(wr + i) % N] * win[i]; im[i] = 0; }
        fft.run(re.data(), im.data());
        const float a = 0.88f, alpha = 0.94f;
        const float atten = 6.f + 22.f * s, gmin = dbToLin(-atten), ov = 1.f + 1.6f * s;
        if (!primed) {
            for (int k = 0; k < K; k++) { const float p = re[k] * re[k] + im[k] * im[k]; pbar[k] = p; curMin[k] = p; histMin[k] = p; aprev[k] = p; }
            for (int u = 0; u < U; u++) std::memcpy(&hist[(size_t)u * K], pbar.data(), (size_t)K * sizeof(float));
            primed = true; subCount = 0;
        }
        for (int k = 0; k < K; k++) {
            const float p = re[k] * re[k] + im[k] * im[k];
            pbar[k] = a * pbar[k] + (1 - a) * p;
            if (pbar[k] < curMin[k]) curMin[k] = pbar[k];
        }
        if (++subCount >= V) {
            subCount = 0;
            std::memcpy(&hist[(size_t)hpos * K], curMin.data(), (size_t)K * sizeof(float));
            hpos = (hpos + 1) % U;
            for (int k = 0; k < K; k++) { float m = hist[k]; for (int u = 1; u < U; u++) m = std::min(m, hist[(size_t)u * K + k]); histMin[k] = m; curMin[k] = pbar[k]; }
        }
        for (int k = 0; k < K; k++) noise[k] = kBias * std::min(histMin[k], curMin[k]) + 1e-12f;
        float sumDb = 0;
        for (int k = 0; k < K; k++) {
            const float nl = noise[k > 0 ? k - 1 : 0], nr = noise[k < K - 1 ? k + 1 : K - 1];
            const float nuse = (0.25f * nl + 0.5f * noise[k] + 0.25f * nr) * ov;
            const float p = re[k] * re[k] + im[k] * im[k];
            const float gam = p / nuse;
            const float xi = alpha * aprev[k] / nuse + (1 - alpha) * std::max(gam - 1.f, 0.f);
            float g = xi / (1.f + xi);
            g = std::max(g, std::max(gmin, gprev[k] * 0.8f));
            gain[k] = g;
        }
        for (int k = 0; k < K; k++) {
            const float gl = gain[k > 0 ? k - 1 : 0], gr = gain[k < K - 1 ? k + 1 : K - 1];
            const float g = 0.25f * gl + 0.5f * gain[k] + 0.25f * gr;
            gsm[k] = g; gprev[k] = g;
            const float p = re[k] * re[k] + im[k] * im[k];
            aprev[k] = g * g * p;
            sumDb += std::log10(g + 1e-6f);
            re[k] *= g; im[k] *= g;
            if (k > 0 && k < N / 2) { re[N - k] *= g; im[N - k] *= g; }
        }
        meanGainDb = 20.f * sumDb / K;
        fft.run(im.data(), re.data());   // inverse (swapped); the real signal is in re, scaled by N
        const float sc = 1.f / N;
        for (int i = 0; i < N; i++) ola[(rd + i) % N] += re[i] * sc * win[i];
    }
    // x in, returns wet (delayed by N) and the delayed dry through dryOut
    inline float tick(float x, float s, bool track, float& dryOut) {
        const float y = ola[rd]; ola[rd] = 0; rd = (rd + 1) % N;
        dryOut = dry[dp]; dry[dp] = x; dp = (dp + 1) % N;
        inRing[wr] = x; wr = (wr + 1) % N;
        if (++cnt >= hop) { cnt = 0; frame(s, track); }
        return y;
    }
};

// ---------------------------------------------------------------- automatic notch (per channel)
struct AutoNotch {
    static constexpr int S = 4;
    int M = 0, hopA = 0;
    Fft fft;
    std::vector<float> win, ring, re, im, mag;
    int wr = 0, cnt = 0;
    float rate = 48000;
    struct Track { bool used = false; float hz = 0, lvlDb = 0, dev = 0; int age = 0, miss = 0; bool confirmed = false; };
    Track tr[S];
    Svf svf[S];
    float f[S] = {}, g[S] = {}, fTarget[S] = {}, gTarget[S] = {};
    float cAtk = 0, cRel = 0;

    void init(int m, float r) {
        M = m; hopA = m / 2; rate = r; fft.init(m);
        win.resize(m); for (int i = 0; i < m; i++) win[i] = 0.5f - 0.5f * std::cos(2 * kPi * i / m);
        ring.assign(m, 0.f); re.assign(m, 0.f); im.assign(m, 0.f); mag.assign(m / 2 + 1, 0.f);
        cAtk = onePole(0.020f, r / 32.f); cRel = onePole(0.350f, r / 32.f);   // per control step (32 samples)
        reset();
    }
    void reset() {
        std::fill(ring.begin(), ring.end(), 0.f); wr = cnt = 0;
        for (int i = 0; i < S; i++) { tr[i] = Track(); svf[i].reset(); f[i] = fTarget[i] = 0; g[i] = gTarget[i] = 0; }
    }
    void analyse() {
        for (int i = 0; i < M; i++) { re[i] = ring[(wr + i) % M] * win[i]; im[i] = 0; }
        fft.run(re.data(), im.data());
        const int K = M / 2 + 1;
        const float sc = 4.f / M;   // Hann: a sinusoid of amplitude A reads A * M / 4
        for (int k = 0; k < K; k++) mag[k] = std::sqrt(re[k] * re[k] + im[k] * im[k]) * sc;
        const float binHz = rate / M;
        struct Cand { float hz, db; } cand[S]; int nc = 0;
        // candidates: local maxima well above their surroundings (a tone: narrow, the neighbours at +-4..10 bins are far lower)
        for (int k = 6; k < K - 6; k++) {
            const float m = mag[k];
            if (m < 0.0025f || m < mag[k - 1] || m < mag[k + 1] || m < mag[k - 2] || m < mag[k + 2]) continue;
            float fl = 0; int c = 0;
            for (int d = 4; d <= 10; d++) { if (k - d >= 1) { fl += mag[k - d] * mag[k - d]; c++; } if (k + d < K) { fl += mag[k + d] * mag[k + d]; c++; } }
            fl = std::sqrt(fl / std::max(1, c)) + 1e-9f;
            if (m < 8.f * fl) continue;   // 18 dB above the floor around it
            const float la = std::log(mag[k - 1] + 1e-12f), lb = std::log(m + 1e-12f), lc = std::log(mag[k + 1] + 1e-12f);
            const float den = la - 2 * lb + lc, off = den < -1e-9f ? 0.5f * (la - lc) / den : 0.f;
            Cand cd{(k + std::max(-0.5f, std::min(0.5f, off))) * binHz, 20.f * std::log10(m)};
            if (nc < S) cand[nc++] = cd;
            else { int w = 0; for (int i = 1; i < S; i++) if (cand[i].db < cand[w].db) w = i; if (cd.db > cand[w].db) cand[w] = cd; }
        }
        bool matched[S] = {};
        for (int t = 0; t < S; t++) {
            if (!tr[t].used) continue;
            int best = -1; float bd = 2.2f * binHz;
            for (int c = 0; c < nc; c++) { if (matched[c]) continue; const float d = std::fabs(cand[c].hz - tr[t].hz); if (d < bd) { bd = d; best = c; } }
            if (best >= 0) {
                matched[best] = true;
                Track& T = tr[t];
                T.hz += 0.6f * (cand[best].hz - T.hz);
                const float prev = T.lvlDb;
                T.lvlDb += 0.25f * (cand[best].db - T.lvlDb);
                T.dev += 0.2f * (std::fabs(cand[best].db - prev) - T.dev);
                T.age++; T.miss = 0;
                if (T.age >= 22 && T.dev < 2.5f) T.confirmed = true;   // about 0.25 s of a steady narrow tone at 48 kHz
            } else if (++tr[t].miss > 5) { tr[t] = Track(); }
        }
        for (int c = 0; c < nc; c++) {
            if (matched[c]) continue;
            for (int t = 0; t < S; t++) if (!tr[t].used) { tr[t].used = true; tr[t].hz = cand[c].hz; tr[t].lvlDb = cand[c].db; tr[t].dev = 0; tr[t].age = 1; tr[t].miss = 0; tr[t].confirmed = false; break; }
        }
        for (int t = 0; t < S; t++) {
            const bool on = tr[t].used && tr[t].confirmed && tr[t].miss <= 3;
            gTarget[t] = on ? 1.f : 0.f;
            if (tr[t].used) fTarget[t] = tr[t].hz;
            if (on && f[t] == 0) f[t] = fTarget[t];
        }
    }
    // in place on x[0..n); enable: the switch. Segments of 32 samples: the analysis sees the sound before the notch.
    void process(float* x, int n, bool enable) {
        for (int i0 = 0; i0 < n; i0 += 32) {
            const int m = std::min(32, n - i0);
            for (int j = 0; j < m; j++) {
                ring[wr] = x[i0 + j]; wr = (wr + 1) % M;
                if (++cnt >= hopA) { cnt = 0; if (enable) analyse(); else for (int t = 0; t < S; t++) gTarget[t] = 0; }
            }
            for (int t = 0; t < S; t++) {
                g[t] += (gTarget[t] - g[t]) * (gTarget[t] > g[t] ? cAtk : cRel);
                if (f[t] > 0 && fTarget[t] > 0) f[t] += (fTarget[t] - f[t]) * 0.35f;
                if (g[t] <= 1e-3f && gTarget[t] == 0.f) { g[t] = 0.f; svf[t].reset(); continue; }
                if (f[t] <= 0) continue;
                svf[t].set(f[t], std::max(8.f, std::min(60.f, f[t] / 25.f + 8.f)), rate);
                for (int j = 0; j < m; j++) x[i0 + j] -= g[t] * svf[t].bp(x[i0 + j]);
            }
        }
    }
    float slotHz(int t) const { return (tr[t].used && tr[t].confirmed && g[t] > 0.05f) ? tr[t].hz : 0.f; }
};

// Noise floor of a level in dB: the minimum over the last 4 s (four buckets), so it follows a changing background within seconds
// and a long speech passage does not pull it up.
struct FloorTracker {
    float bucket[4] = {}, cur = 1e9f; int n = 0, per = 1, warm = 0; bool primed = false;
    void init(float stepsPerSec) { per = std::max(1, (int)stepsPerSec); reset(); }
    void reset() { primed = false; n = 0; warm = per / 4; cur = 1e9f; for (auto& b : bucket) b = 1e9f; }
    float update(float db) {
        if (!primed && db < -100.f) return db;   // the silence before the first sound (the chain's own delay) says nothing about the floor
        if (warm > 0) { warm--; return db; }   // the envelope detector is still settling
        if (!primed) { primed = true; for (auto& b : bucket) b = db; cur = db; }
        cur = std::min(cur, db);
        if (++n >= per) { n = 0; bucket[3] = bucket[2]; bucket[2] = bucket[1]; bucket[1] = bucket[0]; bucket[0] = cur; cur = 1e9f; }
        float m = cur;
        for (float b : bucket) m = std::min(m, b);
        return m;
    }
};

// ---------------------------------------------------------------- one channel of the chain
struct Chan {
    float rate = 48000;
    SpecNr nr;
    std::unique_ptr<NeuralNr> nn;   // RNNoise (neural_nr.h), made in init() (never on the audio thread); null when the rate is unsupported
    bool nnWas = false;
    AutoNotch an;
    Svf manual; Ramp manG; float manHz = 0, manQ = 0;
    Ramp nrW, anW, eqW, dsW, lvW, gtW;
    // voice EQ
    Biquad hp, pres, warm;
    // de-esser
    Svf sib; float sibEnv = 0, fullEnv = 0, dsGain = 1;
    // gate
    FloorTracker gFloor, lFloor;
    float gEnvP = 0, gFloorDb = -90, gGain = 0.001f, gHang = 0; bool gOpen = true; bool gPrimed = false;
    // leveller
    float lP = 0, lGainDb = 0, lGainLin = 1; bool lPrimed = false;
    int ctl = 0;
    float cNrW = 0, cEq = 0, cGateA = 0, cGateR = 0, cEnv = 0, cLvDn = 0, cLvUp = 0;

    void init(float r) {
        rate = r;
        int n = 128; while (n < 1024 && n < 0.0053f * r) n <<= 1;
        nr.init(n); nr.setRate(r);
        nn.reset(NeuralNr::rateSupported((int)r) ? new NeuralNr((int)r) : nullptr); nnWas = false;
        int m = 512; while (m < 4096 && m < 0.0213f * r) m <<= 1;
        an.init(m, r);
        gFloor.init(r / 32.f); lFloor.init(r / 32.f);
        cNrW = onePole(0.010f, r); cEq = onePole(0.020f, r);
        cGateA = onePole(0.002f, r); cGateR = onePole(0.030f, r); cEnv = onePole(0.025f, r);
        hp.highpass(100.f, 0.707f, r);
        pres.peaking(std::min(2800.f, 0.4f * r), 1.0f, 5.f, r);
        warm.peaking(280.f, 0.8f, -2.5f, r);
        reset();
    }
    void reset() {
        nr.reset(); an.reset(); manual.reset(); hp.reset(); pres.reset(); warm.reset(); sib.reset();
        manG = Ramp(); nrW = anW = eqW = dsW = lvW = gtW = Ramp();
        sibEnv = fullEnv = 0; dsGain = 1; gEnvP = 0; gFloorDb = -90; gFloor.reset(); lFloor.reset(); gGain = 0.001f; gHang = 0; gOpen = true; gPrimed = false;
        lP = 0; lGainDb = 0; lGainLin = 1; lPrimed = false;
    }

    // in/out may alias. All stage switches come from s.
    void process(const float* in, float* out, int n, const AudioCleanupSettings& s, std::atomic<float>* slotOut) {
        if (out != in) std::memcpy(out, in, (size_t)n * sizeof(float));
        // 1. notch: automatic (tracks tones), manual
        if (s.autoNotch || an.g[0] > 1e-3f || an.g[1] > 1e-3f || an.g[2] > 1e-3f || an.g[3] > 1e-3f) an.process(out, n, s.autoNotch);
        for (int t = 0; t < AutoNotch::S; t++) slotOut[t].store(an.slotHz(t), std::memory_order_relaxed);
        {
            const float tgt = s.notch ? 1.f : 0.f;
            if (!manG.idle(tgt)) {
                const float hz = std::max(30.f, std::min(s.notchHz, 0.45f * rate));
                const float q = 60.f * std::pow(0.05f, s.notchWidth);   // width 0 -> Q 60, 1 -> Q 3
                if (hz != manHz || q != manQ) { manHz = hz; manQ = q; manual.set(hz, q, rate); }
                const float c = onePole(0.010f, rate);
                for (int i = 0; i < n; i++) { const float g = manG.step(tgt, c); out[i] -= g * manual.bp(out[i]); }
            } else manual.reset();
        }
        // 2. noise reduction (always carries the latency while the chain is on, so switching it does not shift the sound)
        {
            const float tgt = s.nr ? 1.f : 0.f;
            const bool track = s.nr || nrW.v > 1e-3f;
            for (int i = 0; i < n; i++) {
                float dryD; const float wet = nr.tick(out[i], s.nrStrength, track, dryD);
                const float w = nrW.step(tgt, cNrW);
                out[i] = dryD + w * (wet - dryD);
            }
            if (track) nrMean = nr.meanGainDb;
        }
        // 2b. neural voice noise remover (RNNoise, speech only; passes the samples through untouched while off)
        if (nn) {
            if (s.neural != nnWas) { nn->setEnabled(s.neural); if (s.neural) nn->reset(); nnWas = s.neural; }
            if (s.neural) { nn->setStrength(s.neuralStrength); nn->process(out, (size_t)n); }
        }
        // 3. voice EQ + de-esser
        {
            const float tgt = s.voice ? s.voiceAmount : 0.f;
            if (!eqW.idle(tgt)) {
                for (int i = 0; i < n; i++) {
                    const float a = eqW.step(tgt, cEq), x = out[i];
                    const float y = warm.run(pres.run(hp.run(x)));
                    out[i] = x + a * (y - x);
                }
            } else { hp.reset(); pres.reset(); warm.reset(); }
            const float dtg = (s.deess && rate >= 15000.f) ? 1.f : 0.f;
            if (!dsW.idle(dtg)) {
                const float fc = std::min(6500.f, 0.42f * rate);
                if (sibFc != fc) { sibFc = fc; sib.set(fc, 1.6f, rate); }
                const float atk = onePole(0.001f, rate), rel = onePole(0.040f, rate), sm = onePole(0.003f, rate);
                const float am = s.deessAmount, thr = 0.35f - 0.15f * am, maxCut = 0.25f + 0.6f * am;   // sibilance band vs. the whole sound
                for (int i = 0; i < n; i++) {
                    const float a = dsW.step(dtg, cEq), x = out[i];
                    const float b = sib.bp(x);
                    const float ab = std::fabs(b), ax = std::fabs(x);
                    sibEnv += (ab - sibEnv) * (ab > sibEnv ? atk : rel);
                    fullEnv += (ax - fullEnv) * (ax > fullEnv ? atk : rel);
                    float want = 1.f;
                    if (fullEnv > 1e-4f) { const float r = sibEnv / fullEnv; if (r > thr) { const float q = thr / r; want = std::max(1.f - maxCut, q * q); } }
                    dsGain += (want - dsGain) * (want < dsGain ? atk * 2.f : sm);
                    out[i] = x - a * (1.f - dsGain) * b;
                }
            } else { sib.reset(); sibEnv = fullEnv = 0; dsGain = 1; }
        }
        // 4. silence gate: mute below the tracked noise floor + offset, with a hang
        {
            const bool on = s.gate;
            if (on || gGain < 0.999f) {
                const float subDt = 32.f / rate;
                for (int i = 0; i < n; i++) {
                    const float x = out[i];
                    if (!gPrimed && x * x > 1e-10f) gEnvP = x * x;
                    gEnvP += (x * x - gEnvP) * cEnv;
                    if (++gCtl >= 32) {
                        gCtl = 0;
                        const float db = 10.f * std::log10(gEnvP + 1e-12f);
                        gPrimed = gPrimed || db > -100.f;
                        gFloorDb = gFloor.update(db);
                        const bool want = on ? (db > gFloorDb + s.gateOffsetDb && db > -75.f) : true;
                        if (want) { gHang = 0.15f; gOpen = true; } else { gHang -= subDt; if (gHang <= 0) gOpen = false; }
                        if (!on) gOpen = true;
                    }
                    gGain += ((gOpen ? 1.f : 0.001f) - gGain) * (gOpen ? cGateA : cGateR);
                    out[i] = x * gGain;
                }
            } else { gPrimed = false; gFloor.reset(); gGain = 0.001f; gOpen = true; gHang = 0; gEnvP = 0; gGain = 1.f; }
        }
        // 5. leveller + soft limiter
        {
            const bool run = s.level || std::fabs(lGainDb) > 0.05f;
            if (run) {
                const float dn = onePole(0.008f, rate / 32.f), up = onePole(0.600f, rate / 32.f);
                const float amount = s.level ? std::max(0.f, std::min(1.f, s.levelAmount)) : 0.f;
                float prevLin = lGainLin;
                for (int i = 0; i < n; i += 32) {
                    const int m = std::min(32, n - i);
                    float e = 0; for (int j = 0; j < m; j++) e += out[i + j] * out[i + j];
                    if (!lPrimed && e / m > 1e-10f) { lP = e / m; lPrimed = true; }
                    lP += ((e / m) - lP) * (1.f - std::exp(-(float)m / (0.05f * rate)));
                    const float db = 10.f * std::log10(lP + 1e-12f);
                    const float lFloorDb = lFloor.update(db);
                    float want = std::max(-18.f, std::min(18.f, (s.levelTargetDb - db) * amount));
                    if (db < -62.f || (s.levelHold && db < lFloorDb + 8.f)) want = std::min(want, lGainDb);   // never boost the noise
                    lGainDb += (want - lGainDb) * (want < lGainDb ? dn : up);
                    lGainLin = std::pow(10.f, lGainDb / 20.f);
                    for (int j = 0; j < m; j++) {
                        const float gl = prevLin + (lGainLin - prevLin) * (float)(j + 1) / m;
                        out[i + j] = softLim(out[i + j] * gl);
                    }
                    prevLin = lGainLin;
                }
            } else { lPrimed = false; lFloor.reset(); lGainDb = 0; lGainLin = 1; lP = 0; }
        }
    }
    float sibFc = 0, nrMean = 0;
    int gCtl = 0;
};

}  // namespace

// ---------------------------------------------------------------- AudioCleanup
struct AudioCleanup::Impl {
    int rate = 0;
    Chan ch[2];
    std::vector<float> inL, inR, outL, outR;
    Ramp master;
    bool wasIdle = true;
    float cMaster = 0;
    std::atomic<float> slot[2][AutoNotch::S];
};

AudioCleanup::AudioCleanup() : p_(new Impl), cfg_(&AudioCleanupConfig::global()) {
    for (auto& a : p_->slot) for (auto& b : a) b.store(0.f);
}
AudioCleanup::~AudioCleanup() = default;

void AudioCleanup::prepare(int rate) {
    if (rate < 8000) rate = 8000;
    p_->rate = rate;
    for (auto& c : p_->ch) c.init((float)rate);
    p_->inL.assign(kMaxChunk, 0.f); p_->inR = p_->inL; p_->outL = p_->inL; p_->outR = p_->inL;
    p_->cMaster = onePole(0.010f, (float)rate);
    p_->master = Ramp(); p_->wasIdle = true;
}
void AudioCleanup::reset() { for (auto& c : p_->ch) c.reset(); p_->master = Ramp(); p_->wasIdle = true; }
int AudioCleanup::latencySamples() const { return p_->ch[0].nr.N; }
float AudioCleanup::autoNotchHz(int slot) const { return slot >= 0 && slot < AutoNotch::S ? p_->slot[0][slot].load(std::memory_order_relaxed) : 0.f; }

const float* AudioCleanup::process(const float* in, int frames, float* scratch, bool* active) {
    if (active) *active = false;
    Impl& d = *p_;
    if (d.rate == 0 || frames <= 0 || frames > kMaxChunk) return in;
    const AudioCleanupSettings s = cfg_->get();
    const float tgt = s.master ? 1.f : 0.f;
    if (d.master.idle(tgt)) { d.wasIdle = true; gateOpen_.store(true, std::memory_order_relaxed); return in; }
    if (d.wasIdle) { for (auto& c : d.ch) c.reset(); d.wasIdle = false; }
    if (active) *active = true;
    bool mono = true;
    for (int i = 0; i < frames; i++) {
        // a NaN or infinity from a demodulator would stay in the recursive stages (noise estimate, filters) for good: it becomes silence
        const float l = std::isfinite(in[2 * i]) ? in[2 * i] : 0.f, r = std::isfinite(in[2 * i + 1]) ? in[2 * i + 1] : 0.f;
        d.inL[i] = l; d.inR[i] = r;
        if (std::fabs(l - r) > 1e-6f) mono = false;
    }
    d.ch[0].process(d.inL.data(), d.outL.data(), frames, s, d.slot[0]);
    if (mono) std::memcpy(d.outR.data(), d.outL.data(), (size_t)frames * sizeof(float));
    else d.ch[1].process(d.inR.data(), d.outR.data(), frames, s, d.slot[1]);
    for (int i = 0; i < frames; i++) {
        const float m = d.master.step(tgt, d.cMaster);
        scratch[2 * i] = d.inL[i] + m * (d.outL[i] - d.inL[i]);
        scratch[2 * i + 1] = d.inR[i] + m * (d.outR[i] - d.inR[i]);
    }
    gateOpen_.store(!s.gate || d.ch[0].gOpen, std::memory_order_relaxed);
    nrGainDb_.store(s.nr ? d.ch[0].nrMean : 0.f, std::memory_order_relaxed);
    cfg_->statNrDb.store(s.nr ? d.ch[0].nrMean : 0.f, std::memory_order_relaxed);
    cfg_->statGateOpen.store(!s.gate || d.ch[0].gOpen, std::memory_order_relaxed);
    { float hz = 0; for (int t = 0; t < AutoNotch::S; t++) if (d.ch[0].an.slotHz(t) > 0) { hz = d.ch[0].an.slotHz(t); break; } cfg_->statNotchHz.store(hz, std::memory_order_relaxed); }
    return scratch;
}

// ---------------------------------------------------------------- config and presets
AudioCleanupConfig& AudioCleanupConfig::global() { static AudioCleanupConfig c; return c; }

void AudioCleanupConfig::set(const AudioCleanupSettings& s) {
    auto cl = [](float v, float lo, float hi) { return std::max(lo, std::min(hi, v)); };
    nrStrength = cl(s.nrStrength, 0, 1); notchHz = cl(s.notchHz, 30, 20000); notchWidth = cl(s.notchWidth, 0, 1);
    levelTargetDb = cl(s.levelTargetDb, -40, -6); levelAmount = cl(s.levelAmount, 0, 1); gateOffsetDb = cl(s.gateOffsetDb, 3, 30);
    voiceAmount = cl(s.voiceAmount, 0, 1); deessAmount = cl(s.deessAmount, 0, 1);
    neural = s.neural; neuralStrength = cl(s.neuralStrength, 0, 1);
    nr = s.nr; autoNotch = s.autoNotch; notch = s.notch; level = s.level; levelHold = s.levelHold; gate = s.gate; voice = s.voice; deess = s.deess;
    recCleaned = s.recCleaned; skipSilence = s.skipSilence;
    master = s.master;
}
AudioCleanupSettings AudioCleanupConfig::get() const {
    AudioCleanupSettings s;
    s.master = master; s.neural = neural; s.neuralStrength = neuralStrength; s.nr = nr; s.nrStrength = nrStrength; s.autoNotch = autoNotch; s.notch = notch; s.notchHz = notchHz; s.notchWidth = notchWidth;
    s.level = level; s.levelTargetDb = levelTargetDb; s.levelAmount = levelAmount; s.levelHold = levelHold; s.gate = gate; s.gateOffsetDb = gateOffsetDb;
    s.voice = voice; s.voiceAmount = voiceAmount; s.deess = deess; s.deessAmount = deessAmount; s.recCleaned = recCleaned; s.skipSilence = skipSilence;
    return s;
}

const char* audioProfileName(AudioProfile p) {
    switch (p) {
        case AudioProfile::Off: return "Data (off)";
        case AudioProfile::Generic: return "Generic";
        case AudioProfile::AM: return "AM / airband";
        case AudioProfile::NFM: return "NFM / VHF / UHF";
        case AudioProfile::WFM: return "WFM broadcast";
        case AudioProfile::SSB: return "SSB / HF";
        case AudioProfile::CW: return "CW";
        case AudioProfile::DigitalVoice: return "Digital voice";
        default: return "";
    }
}

AudioCleanupSettings audioCleanupPreset(AudioProfile p) {
    AudioCleanupSettings s;   // master off: the user switches it on
    switch (p) {
        case AudioProfile::Off: break;   // data modes: everything off, bypass
        case AudioProfile::Generic: s.nr = true; s.nrStrength = 0.5f; s.autoNotch = true; s.level = true; s.levelAmount = 0.6f; break;
        case AudioProfile::AM: s.nr = true; s.nrStrength = 0.5f; s.autoNotch = true; s.level = true; s.levelAmount = 0.8f; s.gate = true; s.gateOffsetDb = 10.f; s.voice = true; s.voiceAmount = 0.3f; break;
        case AudioProfile::NFM: s.nr = true; s.nrStrength = 0.6f; s.autoNotch = true; s.level = true; s.levelAmount = 0.7f; s.gate = true; s.gateOffsetDb = 9.f; s.voice = true; s.voiceAmount = 0.5f; s.deess = true; s.deessAmount = 0.4f; break;
        case AudioProfile::WFM: s.level = true; s.levelAmount = 0.4f; s.levelHold = false; s.levelTargetDb = -18.f; break;   // music: no noise reduction, no gate, a gentle leveller + limiter
        case AudioProfile::SSB: s.nr = true; s.nrStrength = 0.7f; s.autoNotch = true; s.level = true; s.levelAmount = 0.8f; s.voice = true; s.voiceAmount = 0.4f; break;
        case AudioProfile::CW: s.nr = true; s.nrStrength = 0.3f; s.level = true; s.levelAmount = 0.5f; break;   // the audio filter of the mode comes first; the auto notch would remove the wanted tone
        case AudioProfile::DigitalVoice: s.level = true; s.levelAmount = 0.5f; break;   // the vocoder output is clean: only the level
        default: break;
    }
    return s;
}

}  // namespace dect2
