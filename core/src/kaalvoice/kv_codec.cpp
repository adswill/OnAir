// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// KAAL voice codec 4: the frame codec around the core (band split, high band, VAD / DTX / SID / comfort noise, noise suppression),
// and the FrameCodec wrappers of the old KAAL codecs.
#include "dect2/kaalvoice/kaal_voice.h"
#include "dect2/kaal/kaal_codec.h"
#include "dect2/kaal/kaal_codec3.h"
#include "kv_core.h"
#include "kv_enhance.h"
#include <cmath>
#include <cstring>

namespace dect2 {
namespace kaalvoice {

using namespace detail;

int profileBitsPer20ms(Profile p) {
    static const int b[6] = {96, 120, 160, 240, 320, 64};
    return (int)p < 6 ? b[(int)p] : 0;
}
const char* profileName(Profile p) {
    static const char* n[6] = {"4.8", "6.0", "8.0", "12", "16", "3.2"};
    return (int)p < 6 ? n[(int)p] : "?";
}

namespace {
constexpr int kNsHop16 = 80, kNsHop8 = 40;

Info infoOf(const Config& c, const Prof& P) {
    Info i;
    i.frameSamples = c.sampleRate / 1000 * c.frameMs;
    i.payloadBits = P.totalBits + (c.inBandType ? 1 : 0);
    i.payloadBytes = (i.payloadBits + 7) / 8;
    i.sidBits = kSidBits + (c.inBandType ? 1 : 0);
    i.sidBytes = c.inBandType ? i.payloadBytes : (kSidBits + 7) / 8;
    i.classBits[0] = P.classBits[0] + (c.inBandType ? 1 : 0);
    i.classBits[1] = P.classBits[1];
    i.classBits[2] = P.classBits[2];
    i.padBits = P.padBits;
    i.delaySamples = (c.sampleRate == 16000 ? kSplitDelay + 2 * kLA : kLA) +
                     (c.noiseSuppress ? (c.sampleRate == 16000 ? kNsHop16 : kNsHop8) : 0) + enhanceDelay(c, P);
    return i;
}

// ---- SID: non-predictive LSF (28 bits), LP residual level (6), high-band level (5), high-band tilt (1)
const int kSidLsfBits[kP] = {3, 3, 3, 3, 3, 3, 3, 3, 2, 2};
const double kSidLsfStep[kP] = {0.05, 0.07, 0.09, 0.10, 0.10, 0.10, 0.10, 0.09, 0.12, 0.10};
struct Sid { double lsf[kP]; double resDb, hbDb, hbTilt; };

void sidQuant(const Sid& in, uint8_t* out, Sid& q) {   // out: 5 bytes
    int pos = 0;
    std::memset(out, 0, 5);
    auto put = [&](int v, int nb) { for (int i = nb - 1; i >= 0; i--, pos++) if ((v >> i) & 1) out[pos >> 3] |= (uint8_t)(0x80 >> (pos & 7)); };
    const double* mu = LsfQ::mean();
    for (int i = 0; i < kP; i++) {
        const int n = 1 << kSidLsfBits[i];
        int k = (int)std::floor((in.lsf[i] - mu[i]) / kSidLsfStep[i] + (n - 1) * 0.5 + 0.5);
        k = k < 0 ? 0 : (k >= n ? n - 1 : k);
        put(k, kSidLsfBits[i]);
        q.lsf[i] = mu[i] + (k - (n - 1) * 0.5) * kSidLsfStep[i];
    }
    lsfStabilise(q.lsf, 0.025);
    int e = (int)std::floor(in.resDb / 1.5 + 0.5);
    e = e < 0 ? 0 : (e > 63 ? 63 : e);
    put(e, 6);
    q.resDb = e * 1.5;
    int h = (int)std::floor(in.hbDb / 3.0 + 0.5);
    h = h < 0 ? 0 : (h > 31 ? 31 : h);
    put(h, 5);
    q.hbDb = h == 0 ? -100.0 : h * 3.0;
    const int t = in.hbTilt < -4.5 ? 0 : 1;
    put(t, 1);
    q.hbTilt = t ? 0.0 : -9.0;
}

void sidDequant(const uint8_t* in, Sid& q) {
    int pos = 0;
    auto get = [&](int nb) { int x = 0; for (int i = 0; i < nb; i++, pos++) x = (x << 1) | ((in[pos >> 3] >> (7 - (pos & 7))) & 1); return x; };
    const double* mu = LsfQ::mean();
    for (int i = 0; i < kP; i++) {
        const int n = 1 << kSidLsfBits[i];
        q.lsf[i] = mu[i] + (get(kSidLsfBits[i]) - (n - 1) * 0.5) * kSidLsfStep[i];
    }
    lsfStabilise(q.lsf, 0.025);
    q.resDb = get(6) * 1.5;
    const int h = get(5);
    q.hbDb = h == 0 ? -100.0 : h * 3.0;
    q.hbTilt = get(1) ? 0.0 : -9.0;
}

// ---- DC blocker at the input (first order, 20 Hz: almost no phase shift at the pitch frequencies)
struct HighPass {
    double r = 0.99, x1 = 0, y1 = 0;
    void init(int fs) { r = 1.0 - 2.0 * kPi * 20.0 / fs; x1 = y1 = 0; }
    void run(float* x, int n) {
        for (int i = 0; i < n; i++) {
            const double y = x[i] - x1 + r * y1;
            x1 = x[i];
            y1 = y;
            x[i] = (float)y;
        }
    }
};

// ---- input noise suppressor: 50 % overlap sqrt-Hann frames of 2 hops, zero-padded FFT, minimum-tracking noise estimate,
// decision-directed Wiener gain with a -16 dB floor. Delay: one hop.
struct NoiseSuppressor {
    int M = 80, W = 160, F = 256;
    float win[160];
    float prev[80], tail[160];
    float S[129], Nn[129], G[129], gam[129];
    int frames = 0;
    void init(int fs) {
        M = fs == 16000 ? kNsHop16 : kNsHop8;
        W = 2 * M;
        F = fs == 16000 ? 256 : 128;
        for (int n = 0; n < W; n++) win[n] = (float)kvSin(kPi * (n + 0.5) / W);
        std::memset(prev, 0, sizeof prev);
        std::memset(tail, 0, sizeof tail);
        for (int k = 0; k <= F / 2; k++) { S[k] = 0; Nn[k] = 0; G[k] = 1; gam[k] = 1; }
        frames = 0;
    }
    void hop(const float* in, float* out) {
        float re[256], im[256];
        for (int n = 0; n < F; n++) { re[n] = 0; im[n] = 0; }
        for (int n = 0; n < M; n++) { re[n] = prev[n] * win[n]; re[M + n] = in[n] * win[M + n]; }
        std::memcpy(prev, in, sizeof(float) * (size_t)M);
        fft(re, im, F, false);
        for (int k = 0; k <= F / 2; k++) {
            const float p = re[k] * re[k] + im[k] * im[k];
            S[k] = frames == 0 ? p : 0.7f * S[k] + 0.3f * p;
            if (frames < 12) Nn[k] = frames == 0 ? S[k] : (Nn[k] * frames + S[k]) / (frames + 1);
            else Nn[k] = S[k] < Nn[k] * 1.004f ? S[k] : Nn[k] * 1.004f;
            const float nk = 1.5f * Nn[k] + 1e-3f;
            const float g = p / nk;
            float xi = 0.98f * G[k] * G[k] * gam[k] + 0.02f * (g > 1.0f ? g - 1.0f : 0.0f);
            float gk = xi / (1.0f + xi);
            if (gk < 0.16f) gk = 0.16f;
            G[k] = gk;
            gam[k] = g;
            re[k] *= gk; im[k] *= gk;
            if (k > 0 && k < F / 2) { re[F - k] = re[k]; im[F - k] = -im[k]; }
        }
        fft(re, im, F, true);
        const float sc = 1.0f / F;
        for (int n = 0; n < M; n++) out[n] = tail[n] + re[n] * sc * win[n];
        for (int n = 0; n < M; n++) tail[n] = re[M + n] * sc * win[M + n];
        frames++;
    }
};

// ---- voice activity detector: energy over an adaptive noise floor, helped by the pitch correlation, with a hangover
struct Vad {
    double floorDb = 0;
    int frames = 0, run = 0, hangMs = 0;
    bool decide(double eDb, double voicing, int frameMs) {
        if (frames == 0) floorDb = eDb;
        frames++;
        if (frames < 6 && eDb < floorDb) floorDb = eDb;
        bool sp = eDb > 18.0 && (eDb > floorDb + 9.0 || (eDb > floorDb + 4.0 && voicing > 0.5));
        if (eDb < floorDb) floorDb = 0.7 * floorDb + 0.3 * eDb;
        else floorDb += 0.05 * frameMs / 20.0;
        if (!sp && eDb < floorDb + 6.0) floorDb += 0.1 * (eDb - floorDb);
        if (sp) {
            run += frameMs;
            hangMs = run >= 60 ? 160 : 60;
            return true;
        }
        run = 0;
        if (hangMs > 0) { hangMs -= frameMs; return true; }
        return false;
    }
};

double quickVoicing(const float* x, int n) {
    double e0 = 0;
    for (int i = 0; i < n; i++) e0 += (double)x[i] * x[i];
    double best = 0;
    for (int T = kPitMin; T <= kPitMax && T < n - 20; T++) {
        double c = 0, e = 0;
        for (int i = T; i < n; i++) { c += (double)x[i] * x[i - T]; e += (double)x[i - T] * x[i - T]; }
        const double r = c / std::sqrt((e + 1.0) * (e0 + 1.0));
        if (r > best) best = r;
    }
    return best;
}

float clamp16(float v) { return v > 32767.0f ? 32767.0f : (v < -32768.0f ? -32768.0f : v); }
// soft limiter above 28000: no hard clipping when a loud or damaged frame overshoots
int16_t toS16(float v) {
    const float a = v < 0 ? -v : v;
    if (!(a <= 28000.0f)) {
        const float t = (a > 1e9f ? 1e9f : a) - 28000.0f, k = 4767.0f;
        const float y = 28000.0f + k * (t / (t + k));
        v = v < 0 ? -y : y;
    }
    return (int16_t)std::lround(clamp16(v));
}
}  // namespace

bool valid(const Config& c, std::string* why) {
    Prof P;
    return makeProf(c, P, why);
}

Info info(const Config& c) {
    Prof P;
    if (!makeProf(c, P, nullptr)) return Info();
    return infoOf(c, P);
}

bool typeOfLength(const Config& c, size_t nbytes, FrameType* t) {
    const Info i = info(c);
    if (i.frameSamples == 0) return false;
    if (nbytes == 0) { *t = FrameType::NoData; return true; }
    if (c.inBandType) {
        if (nbytes == (size_t)i.payloadBytes) { *t = FrameType::Speech; return true; }   // the first bit refines it
        return false;
    }
    if (nbytes == (size_t)i.payloadBytes) { *t = FrameType::Speech; return true; }
    if (nbytes == (size_t)i.sidBytes) { *t = FrameType::Sid; return true; }
    return false;
}

// ================================================================ encoder
struct Encoder::Impl {
    Config cfg;
    Prof P;
    Info inf;
    std::unique_ptr<CoreEncoder> core;
    std::unique_ptr<HbEncoder> hbe;
    Down2 down;
    Up2 up;
    HighPass hp;
    NoiseSuppressor ns;
    Vad vad;
    std::vector<float> xdelay;   // 16 kHz input delayed by the split delay
    FrameType lastSent = FrameType::Speech;
    int sinceSid = 0;
    Sid noise{}, sidSent{};
    bool haveNoise = false;
    long frames = 0, speech = 0;
    explicit Impl(const Config& c) : cfg(c) {
        std::string why;
        if (!makeProf(c, P, &why)) { Config d; d.sampleRate = c.sampleRate == 8000 ? 8000 : 16000; cfg = d; makeProf(d, P, nullptr); }
        inf = infoOf(cfg, P);
        core.reset(new CoreEncoder(P));
        hbe.reset(new HbEncoder(P));
        const double* mu = LsfQ::mean();
        for (int i = 0; i < kP; i++) sidSent.lsf[i] = mu[i];
        reset();
    }
    void reset() {
        core->reset();
        hbe->reset();
        down = Down2();
        up = Up2();
        hp.init(cfg.sampleRate);
        ns.init(cfg.sampleRate);
        vad = Vad();
        xdelay.assign((size_t)kSplitDelay, 0.0f);
        lastSent = FrameType::Speech;
        sinceSid = 0;
        haveNoise = false;
        frames = speech = 0;
    }
    FrameType encode(const int16_t* pcm, std::vector<uint8_t>& out) {
        const int n = inf.frameSamples, n8 = P.N8;
        float x[640], lb[320], hb[640];
        for (int i = 0; i < n; i++) x[i] = pcm[i];
        if (cfg.noiseSuppress) {
            const int M = ns.M;
            float y[640];
            for (int i = 0; i < n; i += M) ns.hop(x + i, y + i);
            std::memcpy(x, y, sizeof(float) * (size_t)n);
        }
        hp.run(x, n);
        double eDb;
        if (P.fs == 16000) {
            float ulb[640];
            down.run(x, n, lb);
            up.run(lb, n8, ulb);
            // hb = x delayed by the split delay minus the interpolated low band
            for (int i = 0; i < n; i++) {
                xdelay.push_back(x[i]);
                hb[i] = xdelay[(size_t)i] - ulb[i];
            }
            xdelay.erase(xdelay.begin(), xdelay.begin() + n);
            double el = 0, eh = 0;
            for (int i = 0; i < n8; i++) el += (double)lb[i] * lb[i];
            for (int i = 0; i < n; i++) eh += (double)hb[i] * hb[i];
            eDb = kvDb(el / n8 + eh / n);
        } else {
            std::memcpy(lb, x, sizeof(float) * (size_t)n);
            double el = 0;
            for (int i = 0; i < n8; i++) el += (double)lb[i] * lb[i];
            eDb = kvDb(el / n8);
        }
        bool active = true;
        if (cfg.dtx) active = vad.decide(eDb, quickVoicing(lb, n8), cfg.frameMs);
        frames++;
        FrameQ q;
        if (active && lastSent != FrameType::Speech) core->restart(sidSent.lsf);
        core->encode(lb, q);
        if (P.fs == 16000) hbe->encode(hb, q);
        out.clear();
        if (active) {
            speech++;
            out.assign((size_t)inf.payloadBytes, 0);
            if (cfg.inBandType) {
                std::vector<uint8_t> tmp((size_t)(P.totalBits + 7) / 8 + 1, 0);
                packFrame(P, q, tmp.data());
                for (int i = 0; i < P.totalBits; i++)   // shift by the type bit (0)
                    if ((tmp[(size_t)i >> 3] >> (7 - (i & 7))) & 1) out[(size_t)(i + 1) >> 3] |= (uint8_t)(0x80 >> ((i + 1) & 7));
            } else packFrame(P, q, out.data());
            lastSent = FrameType::Speech;
            haveNoise = false;
            return FrameType::Speech;
        }
        // non-speech: track the background, send a SID when it is new or has changed, otherwise nothing
        Sid cur;
        for (int i = 0; i < kP; i++) cur.lsf[i] = core->lsfUnq()[i];
        cur.resDb = core->residualDb();
        cur.hbDb = P.fs == 16000 ? hbe->levelDb() : 0.0;
        cur.hbTilt = P.fs == 16000 ? hbe->tiltDb() : 0.0;
        cur.resDb = cur.resDb < 0.0 ? 0.0 : cur.resDb;   // the SID range starts at 0 dB
        cur.hbDb = cur.hbDb < 0.0 ? 0.0 : cur.hbDb;
        if (!haveNoise) { noise = cur; haveNoise = true; }
        else {
            const double a = cfg.frameMs == 40 ? 0.75 : 0.85;   // about 130 ms averaging
            for (int i = 0; i < kP; i++) noise.lsf[i] = a * noise.lsf[i] + (1 - a) * cur.lsf[i];
            noise.resDb = a * noise.resDb + (1 - a) * cur.resDb;
            noise.hbDb = a * noise.hbDb + (1 - a) * cur.hbDb;
            noise.hbTilt = a * noise.hbTilt + (1 - a) * cur.hbTilt;
        }
        sinceSid += cfg.frameMs;
        double dl = 0;
        for (int i = 0; i < kP; i++) dl += (noise.lsf[i] - sidSent.lsf[i]) * (noise.lsf[i] - sidSent.lsf[i]);
        // a SID on the first silent frame, then every 480 ms, or sooner when the background has clearly changed
        const bool send = lastSent == FrameType::Speech || sinceSid >= 480 ||
                          (sinceSid >= 80 && (std::fabs(noise.resDb - sidSent.resDb) > 4.0 || std::fabs(noise.hbDb - (sidSent.hbDb < 0.0 ? 0.0 : sidSent.hbDb)) > 6.0 || dl > 0.04));
        if (!send) { lastSent = FrameType::NoData; return FrameType::NoData; }
        uint8_t s5[5];
        sidQuant(noise, s5, sidSent);
        sinceSid = 0;
        lastSent = FrameType::Sid;
        if (cfg.inBandType) {
            out.assign((size_t)inf.payloadBytes, 0);
            out[0] = 0x80;
            for (int i = 0; i < kSidBits; i++)
                if ((s5[i >> 3] >> (7 - (i & 7))) & 1) out[(size_t)(i + 1) >> 3] |= (uint8_t)(0x80 >> ((i + 1) & 7));
        } else out.assign(s5, s5 + 5);
        return FrameType::Sid;
    }
};

Encoder::Encoder(const Config& c) : p_(new Impl(c)) {}
Encoder::~Encoder() = default;
const Info& Encoder::info() const { return p_->inf; }
FrameType Encoder::encode(const int16_t* pcm, std::vector<uint8_t>& out) { return p_->encode(pcm, out); }
void Encoder::reset() { p_->reset(); }
double Encoder::speechFraction() const { return p_->frames ? (double)p_->speech / p_->frames : 0.0; }

// ================================================================ decoder
struct Decoder::Impl {
    Config cfg;
    Prof P;
    Info inf;
    std::unique_ptr<CoreDecoder> core;
    std::unique_ptr<HbDecoder> hbd;
    std::unique_ptr<Enhancer> enh;   // null when the enhancer is off
    std::shared_ptr<ExternalPostFilter> ext;
    EnhanceSide side;
    Up2 up;
    bool cng = false;           // comfort noise running
    Sid cur{}, target{};
    bool haveSid = false;
    float cngSyn[kP] = {};
    Rng rng;
    int lostRun = 0;            // consecutive concealed frames
    bool gap = true;            // the next speech frame follows a loss, comfort noise or a reset
    explicit Impl(const Config& c) : cfg(c) {
        if (!makeProf(c, P, nullptr)) { Config d; d.sampleRate = c.sampleRate == 8000 ? 8000 : 16000; cfg = d; makeProf(d, P, nullptr); }
        inf = infoOf(cfg, P);
        core.reset(new CoreDecoder(P));
        hbd.reset(new HbDecoder(P));
        if (enhanceActive(cfg, P)) enh.reset(new Enhancer(P, cfg.enhanceStages, cfg.postfilter));
        reset();
    }
    void reset() {
        core->reset();
        hbd->reset();
        if (enh) enh->reset();
        if (ext) ext->reset();
        up = Up2();
        cng = false;
        haveSid = false;
        std::memset(cngSyn, 0, sizeof cngSyn);
        rng.s = 0x434e4721u;
        const double* mu = LsfQ::mean();
        for (int i = 0; i < kP; i++) cur.lsf[i] = target.lsf[i] = mu[i];
        cur.resDb = target.resDb = 0;
        cur.hbDb = target.hbDb = -100;
        cur.hbTilt = target.hbTilt = 0;
        lostRun = 0;
        gap = true;
        side = EnhanceSide();
    }
    void comfortLb(float* lb) {   // one frame of comfort noise in the low band, parameters moving towards the last SID
        const int L = 40;
        const bool shape = enh && (cfg.enhanceStages & kEnhComfort);
        for (int s = 0; s < P.N8 / L; s++) {
            for (int i = 0; i < kP; i++) cur.lsf[i] = 0.9 * cur.lsf[i] + 0.1 * target.lsf[i];
            cur.resDb = 0.9 * cur.resDb + 0.1 * target.resDb;
            float a[kP + 1];
            lsfToLpc(cur.lsf, a);
            if (shape) weightLpc(a, 0.92, a);   // enhancer: no sharp peaks from the coarse SID spectrum
            const float g = (float)std::sqrt(kvFromDb(cur.resDb));
            float e[40];
            for (int n = 0; n < L; n++) e[n] = g * rng.gauss();
            iirA(a, e, lb + s * L, L, cngSyn);
        }
        cur.hbDb = 0.8 * cur.hbDb + 0.2 * target.hbDb;
        cur.hbTilt = 0.8 * cur.hbTilt + 0.2 * target.hbTilt;
    }
    // the high band of a speech / concealed frame (q == nullptr: concealed)
    void highBand(const FrameQ* q, const bool* timeBad, float* hb) {
        double L[8][kHbBands];
        const int nb = 4 * P.halves;
        hbd->levels(q, timeBad, L);
        if (enh) enh->highBand(core.get(), L, nb, hb);
        else hbd->synth(L, nb, hb);
        sideHb(L, nb);
    }
    void comfortHb(float* hb) {
        const int nb = 4 * P.halves;
        if (enh) {
            double L[8][kHbBands];
            hbd->comfortLevels(cur.hbDb, cur.hbTilt, L[0]);
            for (int b = 1; b < nb; b++) std::memcpy(L[b], L[0], sizeof L[0]);
            enh->highBand(nullptr, L, nb, hb);
            sideHb(L, nb);
        } else {
            hbd->comfort(cur.hbDb, cur.hbTilt, hb, inf.frameSamples);
            side.hbBlocks = 0;
        }
    }
    void sideHb(const double (*L)[kHbBands], int nb) {
        side.hbBlocks = nb;
        for (int b = 0; b < nb; b++)
            for (int j = 0; j < kHbBands; j++) side.hbLevelDb[b][j] = (float)L[b][j];
    }
    void sideCore(EnhanceSide::Kind k) {
        side.kind = k;
        side.subframes = P.S;
        side.subLen8 = P.L;
        double v = 0;
        for (int s = 0; s < P.S; s++) {
            const SubInfo& si = core->sub(s);
            for (int i = 0; i <= kP; i++) side.lpc[s][i] = si.aq[i];
            side.lag[s] = (float)si.lag;
            side.pitchGain[s] = (float)si.gp;
            side.codeGain[s] = (float)si.gc;
            v += si.gp;
        }
        side.voicing = (float)(v / P.S);
    }
    void finish(const float* lb, const float* hb, int16_t* pcm, Enhancer::Kind k, bool afterLoss, int lostBefore) {
        const int n = inf.frameSamples;
        float y[640];
        if (P.fs == 16000) {
            float u[640];
            up.run(lb, P.N8, u);
            for (int i = 0; i < n; i++) y[i] = u[i] + hb[i];
        } else {
            for (int i = 0; i < n; i++) y[i] = lb[i];
        }
        if (enh) enh->output(y, n, k, afterLoss, lostBefore);
        side.sampleRate = P.fs;
        side.frameSamples = n;
        side.subframes = P.S;   // the frame layout, also for comfort noise (which has no LPC of its own here)
        side.subLen8 = P.L;
        side.enhanced = enh != nullptr;
        side.lostRun = lostRun;
        side.noiseDb = (float)(haveSid ? cur.resDb : (enh ? enh->noiseDb() : -100.0));
        if (ext) ext->process(side, y);
        for (int i = 0; i < n; i++) pcm[i] = toS16(y[i]);
    }
    void decode(FrameType t, const uint8_t* data, size_t nbytes, int16_t* pcm, uint32_t bad) {
        float lb[320] = {}, hb[640] = {};
        bool lost = data == nullptr || (bad & kBadA);
        const uint8_t* body = data;
        std::vector<uint8_t> shifted;
        if (!lost && t != FrameType::NoData && cfg.inBandType) {
            if (nbytes != (size_t)inf.payloadBytes) lost = true;
            else {
                t = (data[0] & 0x80) ? FrameType::Sid : FrameType::Speech;
                const int nb = t == FrameType::Sid ? kSidBits : P.totalBits;
                shifted.assign((size_t)(nb + 7) / 8 + 1, 0);
                for (int i = 0; i < nb; i++)
                    if ((data[(size_t)(i + 1) >> 3] >> (7 - ((i + 1) & 7))) & 1) shifted[(size_t)i >> 3] |= (uint8_t)(0x80 >> (i & 7));
                body = shifted.data();
            }
        } else if (!lost && t == FrameType::Speech && nbytes != (size_t)inf.payloadBytes) lost = true;
        else if (!lost && t == FrameType::Sid && nbytes != (size_t)inf.sidBytes) lost = true;
        if (t == FrameType::NoData && data == nullptr && nbytes == 0 && !(bad & kBadA)) lost = false;
        const bool pf = cfg.postfilter && !enh;   // the enhancer has its own post-filter
        side.firstAfterGap = false;
        if (lost || t == FrameType::NoData) {
            if (cng || (t == FrameType::NoData && !lost && haveSid)) {
                cng = true;
                comfortLb(lb);
                if (enh) enh->comfortLow(lb);
                if (P.fs == 16000) comfortHb(hb);
                side.kind = EnhanceSide::ComfortNoise;
                gap = true;
                finish(lb, hb, pcm, Enhancer::Comfort, false, 0);
            } else {
                core->decode(nullptr, nullptr, pf, lb);
                if (enh) enh->lowBand(*core, true, lb);
                if (P.fs == 16000) highBand(nullptr, nullptr, hb);
                lostRun++;
                gap = true;
                sideCore(EnhanceSide::Concealed);
                finish(lb, hb, pcm, Enhancer::Concealed, false, lostRun);
            }
            return;
        }
        if (t == FrameType::Sid) {
            sidDequant(body, target);
            if (!cng) {   // first SID after speech: start from the SID values
                cur = target;
                const double* l = core->lsfLast();
                for (int i = 0; i < kP; i++) cur.lsf[i] = 0.5 * l[i] + 0.5 * target.lsf[i];
            }
            haveSid = true;
            cng = true;
            comfortLb(lb);
            if (enh) enh->comfortLow(lb);
            if (P.fs == 16000) comfortHb(hb);
            side.kind = EnhanceSide::ComfortNoise;
            gap = true;
            finish(lb, hb, pcm, Enhancer::Comfort, false, 0);
            return;
        }
        // speech
        float fade[320] = {};
        const bool fromCng = cng;
        if (cng) {   // both sides restart; the comfort noise fades out under the first speech frame
            core->restart(haveSid ? target.lsf : core->lsfLast());
            if (enh) enh->restart();
            comfortLb(fade);
            cng = false;
        }
        FrameQ q;
        bool fcbBad[kMaxSub], timeBad = false;
        unpackFrame(P, body, bad, q, fcbBad, &timeBad);
        core->decode(&q, fcbBad, pf, lb);
        if (enh) enh->lowBand(*core, false, lb);
        if (P.fs == 16000) highBand(&q, &timeBad, hb);
        if (fromCng)
            for (int i = 0; i < P.N8; i++) lb[i] += fade[i] * (float)(1.0 - (double)i / P.N8);
        const int lostBefore = lostRun;
        const bool afterLoss = lostRun > 0;
        lostRun = 0;
        sideCore(EnhanceSide::Speech);
        side.firstAfterGap = gap;
        gap = false;
        finish(lb, hb, pcm, Enhancer::Speech, afterLoss, lostBefore);
    }
};

Decoder::Decoder(const Config& c) : p_(new Impl(c)) {}
Decoder::~Decoder() = default;
const Info& Decoder::info() const { return p_->inf; }
void Decoder::decode(FrameType t, const uint8_t* data, size_t nbytes, int16_t* pcm, uint32_t bad) { p_->decode(t, data, nbytes, pcm, bad); }
void Decoder::reset() { p_->reset(); }
void Decoder::setPostFilter(std::shared_ptr<ExternalPostFilter> pf) { p_->ext = std::move(pf); if (p_->ext) p_->ext->reset(); }
const EnhanceSide& Decoder::lastSide() const { return p_->side; }
bool Decoder::enhancing() const { return p_->enh != nullptr; }

// ================================================================ FrameCodec wrappers
namespace {
class Kv4Codec : public FrameCodec {
public:
    explicit Kv4Codec(const Config& c) : e_(c), d_(c) {}
    CodecId id() const override { return CodecId::KV4; }
    const Info& info() const override { return e_.info(); }
    FrameType encode(const int16_t* pcm, std::vector<uint8_t>& out) override { return e_.encode(pcm, out); }
    void decode(FrameType t, const uint8_t* data, size_t n, int16_t* pcm, uint32_t bad) override { d_.decode(t, data, n, pcm, bad); }
private:
    Encoder e_;
    Decoder d_;
};

// The old codecs code frame t from input [(t - 1) * 320, (t + 1) * 320): the wrapper keeps the previous 320 samples and takes the
// middle frame of encode(prev + cur). Bits are one per byte in the old API and packed MSB first here.
template <class C, class D> class LegacyCodec : public FrameCodec {
public:
    LegacyCodec(CodecId id, C c, D* dec, int bits) : id_(id), c_(c), dec_(dec), bits_(bits) {
        inf_.frameSamples = 320;
        inf_.payloadBits = bits;
        inf_.payloadBytes = (bits + 7) / 8;
        inf_.sidBits = 0;
        inf_.sidBytes = 0;
        inf_.classBits[2] = bits;
        inf_.delaySamples = 320;
        prev_.assign(320, 0);
    }
    CodecId id() const override { return id_; }
    const Info& info() const override { return inf_; }
    FrameType encode(const int16_t* pcm, std::vector<uint8_t>& out) override {
        std::vector<int16_t> two(prev_);
        two.insert(two.end(), pcm, pcm + 320);
        prev_.assign(pcm, pcm + 320);
        const std::vector<uint8_t> b = c_.encode(two);
        out.assign((size_t)inf_.payloadBytes, 0);
        for (int i = 0; i < bits_; i++)
            if (b[(size_t)bits_ + i]) out[(size_t)i >> 3] |= (uint8_t)(0x80 >> (i & 7));
        return FrameType::Speech;
    }
    void decode(FrameType, const uint8_t* data, size_t n, int16_t* pcm, uint32_t bad) override {
        if (!data || n != (size_t)inf_.payloadBytes || bad) { push(nullptr, pcm); return; }
        std::vector<uint8_t> b((size_t)bits_);
        for (int i = 0; i < bits_; i++) b[(size_t)i] = (data[i >> 3] >> (7 - (i & 7))) & 1;
        push(b.data(), pcm);
    }
private:
    void push(const uint8_t* b, int16_t* pcm);
    CodecId id_;
    C c_;
    std::unique_ptr<D> dec_;
    int bits_;
    Info inf_;
    std::vector<int16_t> prev_;
};
template <> void LegacyCodec<KaalCodec3, KaalCodec3::StreamDecoder>::push(const uint8_t* b, int16_t* pcm) { dec_->push(b, (size_t)bits_, pcm); }
template <> void LegacyCodec<KaalCodec, KaalCodec::StreamDecoder>::push(const uint8_t* b, int16_t* pcm) { dec_->push(b, pcm); }
}  // namespace

std::unique_ptr<FrameCodec> makeFrameCodec(CodecId id, const Config& c) {
    if (id == CodecId::KV4) {
        if (!valid(c)) return nullptr;
        return std::unique_ptr<FrameCodec>(new Kv4Codec(c));
    }
    if (c.sampleRate != 16000 || c.frameMs != 20) return nullptr;
    if (id == CodecId::LegacyV3) {
        const int bits = c.payloadBits ? c.payloadBits : 320;
        if (!KaalCodec3::validBitrate(bits / 20) || bits % 20) return nullptr;
        return std::unique_ptr<FrameCodec>(new LegacyCodec<KaalCodec3, KaalCodec3::StreamDecoder>(
            id, KaalCodec3(bits / 20), new KaalCodec3::StreamDecoder(bits / 20), bits));
    }
    if (id == CodecId::LegacyV1) {
        if (c.payloadBits && c.payloadBits != 320) return nullptr;
        return std::unique_ptr<FrameCodec>(new LegacyCodec<KaalCodec, KaalCodec::StreamDecoder>(id, KaalCodec(), new KaalCodec::StreamDecoder(), 320));
    }
    return nullptr;
}

}  // namespace kaalvoice
}  // namespace dect2
