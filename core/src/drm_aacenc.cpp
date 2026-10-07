// DRM AAC encoder for the test signal (see drm_aacenc.h): MDCT with the 960 transform, one flat quantiser step for the whole frame, the Huffman codebooks
// of drm_aactab.cpp, section data, and the spectral data in the order of the codeword reordering (HCR) of clause 5.4.1.
#include "dect2/drm_aacenc.h"
#include "dect2/drm_fec.h"
#include "dect2/drm_fft.h"
#include "dect2/drm_gen.h"
#include "drm_aac_internal.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace dect2 { namespace drm {

using namespace aac;

namespace {

constexpr int kN = kFrame;            // spectral lines per frame
constexpr int kW = 2 * kN;            // window length
constexpr double kPcmScale = 32768.0; // the decoder's output is +-1.0 for 16 bit full scale

struct Bits {
    std::vector<uint8_t> b;           // one bit per byte
    void put(uint32_t v, int n) { for (int i = n - 1; i >= 0; i--) b.push_back((uint8_t)((v >> i) & 1u)); }
    size_t size() const { return b.size(); }
};

// the bits of one code word (a pair or a quad): Huffman code, sign bits of the non-zero values of an unsigned codebook, then the escape sequences
void unitBits(int cb, const int* v, std::vector<uint8_t>& out) {
    out.clear();
    const CbInfo& ci = cbInfo(cb);
    int t[4];
    int esc[2] = {0, 0};
    for (int i = 0; i < ci.dim; i++) t[i] = ci.isSigned ? v[i] : std::abs(v[i]);
    if (cb == 11) for (int i = 0; i < 2; i++) if (t[i] > 15) { esc[i] = t[i]; t[i] = 16; }
    const int idx = cbIndex(cb, t);
    for (int i = ci.bits[idx] - 1; i >= 0; i--) out.push_back((uint8_t)((ci.code[idx] >> i) & 1u));
    if (!ci.isSigned) for (int i = 0; i < ci.dim; i++) if (v[i]) out.push_back(v[i] < 0 ? 1 : 0);
    if (cb == 11) {
        for (int i = 0; i < 2; i++) {
            if (!esc[i]) continue;
            int nb = 4;
            while ((esc[i] >> (nb + 1)) != 0) nb++;          // the value is 2^nb + a word of nb bits
            for (int k = 4; k < nb; k++) out.push_back(1);
            out.push_back(0);
            const int word = esc[i] - (1 << nb);
            for (int k = nb - 1; k >= 0; k--) out.push_back((uint8_t)((word >> k) & 1));
        }
    }
}

int unitLength(int cb, const int* v) {
    const CbInfo& ci = cbInfo(cb);
    int t[4];
    int len = 0;
    for (int i = 0; i < ci.dim; i++) t[i] = ci.isSigned ? v[i] : std::abs(v[i]);
    int escLen = 0;
    if (cb == 11) for (int i = 0; i < 2; i++) if (t[i] > 15) { int nb = 4; while ((t[i] >> (nb + 1)) != 0) nb++; escLen += (nb - 4) + 1 + nb; t[i] = 16; }
    const int idx = cbIndex(cb, t);
    len = ci.bits[idx] + escLen;
    if (!ci.isSigned) for (int i = 0; i < ci.dim; i++) if (v[i]) len++;
    return len;
}

// The place of every bit of every code word in the spectral data of L bits, as the decoder reads them (drm_aac.cpp, hcrDecode): priority code words at the start
// of their segments, the others from the ends of the segments alternately, set by set. false when L is too small.
bool hcrPlace(const std::vector<HcrCw>& cws, const std::vector<int>& lens, int L, int Lc, std::vector<std::vector<int>>& where) {
    const int N = (int)cws.size();
    std::vector<int> segStart, segW;
    int pos = 0;
    for (int i = 0; i < N; i++) {
        const int w = std::min(kMaxCwLen[cws[(size_t)i].cb], Lc);
        if (pos + w > L) break;
        segStart.push_back(pos); segW.push_back(w); pos += w;
    }
    const int nSeg = (int)segStart.size();
    if (nSeg == 0) return false;
    where.assign((size_t)N, {});
    struct Free { int lo, hi; };
    std::vector<Free> fr((size_t)nSeg);
    for (int i = 0; i < nSeg; i++) {
        if (lens[(size_t)i] > segW[(size_t)i]) return false;
        for (int t = 0; t < lens[(size_t)i]; t++) where[(size_t)i].push_back(segStart[(size_t)i] + t);
        fr[(size_t)i] = {segStart[(size_t)i] + lens[(size_t)i], segStart[(size_t)i] + segW[(size_t)i]};
    }
    fr[(size_t)nSeg - 1].hi = L;                              // the bits behind the last segment belong to it
    const int nRest = N - nSeg;
    std::vector<int> placed((size_t)nRest, 0);
    std::vector<uint8_t> done((size_t)nRest, 0);
    int left = nRest;
    const int nSets = N / nSeg;
    for (int set = 1; set <= nSets && left > 0; set++) {
        const bool fromHi = (set & 1) != 0;
        for (int trial = 0; trial < nSeg; trial++) {
            for (int base = 0; base < nSeg; base++) {
                const int ci = base + (set - 1) * nSeg;
                if (ci >= nRest) break;
                const int sIdx = (trial + base) % nSeg;
                if (done[(size_t)ci] || fr[(size_t)sIdx].lo >= fr[(size_t)sIdx].hi) continue;
                const int need = lens[(size_t)(nSeg + ci)] - placed[(size_t)ci];
                const int take = std::min(need, fr[(size_t)sIdx].hi - fr[(size_t)sIdx].lo);
                for (int t = 0; t < take; t++) where[(size_t)(nSeg + ci)].push_back(fromHi ? --fr[(size_t)sIdx].hi : fr[(size_t)sIdx].lo++);
                placed[(size_t)ci] += take;
                if (placed[(size_t)ci] == lens[(size_t)(nSeg + ci)]) { done[(size_t)ci] = 1; left--; }
            }
        }
    }
    return left == 0;
}

} // namespace

struct AacFrameEncoder::Impl {
    int rate = 24000;
    int nSfb = 0;
    const uint16_t* swb = nullptr;
    DrmFft fft{kW};
    std::vector<float> win;
    std::vector<cf32> pre, post;
    std::vector<float> prev;
    std::vector<float> spec;            // MDCT of the last two blocks, in the scale of 16 bit samples

    explicit Impl(int r) : rate(r) {
        swb = swbLong(rate, nSfb);
        win.resize(kW);
        for (int n = 0; n < kW; n++) win[(size_t)n] = (float)std::sin(M_PI * (n + 0.5) / kW);
        pre.resize(kW);
        for (int n = 0; n < kW; n++) { const double a = -M_PI * n / kW; pre[(size_t)n] = cf32((float)std::cos(a), (float)std::sin(a)); }
        post.resize(kN);
        const double n0 = (kN + 1) / 2.0;                       // (N/2 + 1) / 2 for a window of N = 2 * 960
        for (int k = 0; k < kN; k++) { const double a = -2 * M_PI * n0 * (k + 0.5) / kW; post[(size_t)k] = cf32((float)std::cos(a), (float)std::sin(a)); }
        prev.assign(kN, 0.f);
        spec.assign(kN, 0.f);
    }

    void mdct(const float* cur) {
        std::vector<cf32> y(kW);
        for (int n = 0; n < kW; n++) {
            const float x = n < kN ? prev[(size_t)n] : cur[n - kN];
            y[(size_t)n] = pre[(size_t)n] * (x * win[(size_t)n] * (float)kPcmScale);
        }
        fft.forward(y.data());
        // the analysis filter bank of ISO/IEC 14496-3 has a factor 2 in front of the sum (the decoder's inverse transform has 2 / N)
        for (int k = 0; k < kN; k++) spec[(size_t)k] = 2.f * (y[(size_t)k] * post[(size_t)k]).real();
        std::memcpy(prev.data(), cur, sizeof(float) * kN);
    }

    struct Coded {
        int gg = 0, maxSfb = 0;
        std::vector<int> q;             // quantised lines
        std::vector<int> bandCb;        // codebook per band
        Bits si;
        std::vector<HcrCw> cws;
        std::vector<int> lens;
        int sumLen = 0, Lc = 0;
    };

    // quantise with the step of scale factor gg and choose the codebooks; builds the side information up to (not including) the length fields
    bool code(int gg, Coded& c) const {
        const double g = std::pow(2.0, (gg - 100) / 4.0);
        c.gg = gg;
        c.q.assign(kN, 0);
        for (int i = 0; i < kN; i++) {
            const double a = std::fabs((double)spec[(size_t)i]) / g;
            int q = (int)(std::pow(a, 0.75) + 0.4054);
            if (q > 8000) return false;
            c.q[(size_t)i] = spec[(size_t)i] < 0 ? -q : q;
        }
        int last = -1;
        for (int b = 0; b < nSfb; b++) for (int i = swb[b]; i < swb[b + 1]; i++) if (c.q[(size_t)i]) last = b;
        c.maxSfb = last + 1;
        c.bandCb.assign((size_t)std::max(1, c.maxSfb), 0);
        for (int b = 0; b < c.maxSfb; b++) {
            int mx = 0;
            for (int i = swb[b]; i < swb[b + 1]; i++) mx = std::max(mx, std::abs(c.q[(size_t)i]));
            if (mx == 0) continue;
            int bestCb = 0, bestBits = 1 << 30;
            for (int cb = 1; cb <= 11; cb++) {
                const CbInfo& ci = cbInfo(cb);
                if (cb != 11 && mx > ci.lav) continue;
                int bits = 0;
                for (int i = swb[b]; i < swb[b + 1]; i += ci.dim) bits += unitLength(cb, &c.q[(size_t)i]);
                if (bits < bestBits) { bestBits = bits; bestCb = cb; }
            }
            c.bandCb[(size_t)b] = bestCb;
        }
        // the code words in HCR order
        int sfbCb[8][64] = {};
        for (int b = 0; b < c.maxSfb; b++) sfbCb[0][b] = c.bandCb[(size_t)b];
        const int groupLen[8] = {1};
        hcrSortedList(1, groupLen, c.maxSfb, sfbCb, swb, kN, c.cws);
        c.lens.assign(c.cws.size(), 0);
        c.sumLen = 0; c.Lc = 0;
        for (size_t i = 0; i < c.cws.size(); i++) {
            c.lens[i] = unitLength(c.cws[i].cb, &c.q[(size_t)c.cws[i].sp]);
            c.sumLen += c.lens[i];
            c.Lc = std::max(c.Lc, c.lens[i]);
        }
        if (c.Lc > 63) return false;
        // side information
        Bits& s = c.si;
        s.b.clear();
        s.put(0, 1); s.put(0, 2); s.put(0, 1);                  // reserved bit, only long windows, sine window
        s.put((uint32_t)c.maxSfb, 6);
        s.put(0, 1);                                            // no TNS
        s.put(0, 1);                                            // no pulses
        s.put((uint32_t)gg, 8);
        for (int b = 0; b < c.maxSfb;) {
            int e = b;
            while (e < c.maxSfb && c.bandCb[(size_t)e] == c.bandCb[(size_t)b]) e++;
            s.put((uint32_t)c.bandCb[(size_t)b], 5);
            int len = e - b;
            while (len >= 31) { s.put(31, 5); len -= 31; }
            s.put((uint32_t)len, 5);
            b = e;
        }
        for (int b = 0; b < c.maxSfb; b++)
            if (c.bandCb[(size_t)b]) s.put(kScaleCode[60], kScaleBits[60]);        // every scale factor equals the global gain: a difference of 0
        return true;
    }

    bool finish(Coded& c, int budgetBits, Bits& out, int& siOut) const {
        const int siBits = (int)c.si.size() + 14 + 6;
        siOut = siBits;
        const int room = budgetBits - siBits;
        if (room < 0) return false;
        if (c.cws.empty()) {
            out = c.si;
            out.put(0, 14); out.put(0, 6);
            return true;
        }
        // the shortest L that the decoder can read: bisect (the first L that works for every larger one in practice), then check
        int hi = 0;
        for (size_t i = 0; i < c.cws.size(); i++) hi += std::min(kMaxCwLen[c.cws[i].cb], c.Lc);
        int lo = c.sumLen;
        std::vector<std::vector<int>> where;
        if (hi > 16383) hi = 16383;
        if (!hcrPlace(c.cws, c.lens, hi, c.Lc, where)) return false;
        if (lo > hi) lo = hi;
        std::vector<std::vector<int>> w2;
        while (lo < hi) {
            const int mid = (lo + hi) / 2;
            if (hcrPlace(c.cws, c.lens, mid, c.Lc, w2)) { hi = mid; where.swap(w2); } else lo = mid + 1;
        }
        const int L = hi;
        if (L > room) return false;
        out = c.si;
        out.put((uint32_t)L, 14);
        out.put((uint32_t)c.Lc, 6);
        const size_t base = out.size();
        out.b.resize(base + (size_t)L, 0);
        std::vector<uint8_t> cwBits;
        for (size_t i = 0; i < c.cws.size(); i++) {
            unitBits(c.cws[i].cb, &c.q[(size_t)c.cws[i].sp], cwBits);
            for (size_t t = 0; t < cwBits.size(); t++) out.b[base + (size_t)where[i][t]] = cwBits[t];
        }
        return true;
    }
};

AacFrameEncoder::AacFrameEncoder(int rateHz) : p_(std::make_unique<Impl>(rateHz == 12000 ? 12000 : 24000)) {}
AacFrameEncoder::~AacFrameEncoder() = default;
int AacFrameEncoder::rateHz() const { return p_->rate; }
void AacFrameEncoder::reset() { std::fill(p_->prev.begin(), p_->prev.end(), 0.f); }

bool AacFrameEncoder::encode(const float* pcm, int budgetBytes, std::vector<uint8_t>& frame, int* siBits) {
    Impl& d = *p_;
    d.mdct(pcm);
    const int budgetBits = budgetBytes * 8;
    Impl::Coded c;
    Bits out;
    int si = 0;
    bool ok = false;
    // the smallest step that fits: bisect on the estimate (side information + code word bits), then step up until the real frame fits
    int lo = 40, hi = 250;
    while (lo < hi) {
        const int mid = (lo + hi) / 2;
        Impl::Coded t;
        const bool good = d.code(mid, t);
        const int est = good ? (int)t.si.size() + 20 + t.sumLen + t.sumLen / 40 + 8 : 1 << 30;
        if (good && est <= budgetBits) hi = mid; else lo = mid + 1;
    }
    for (int gg = lo; gg <= 255 && !ok; gg++) {
        if (!d.code(gg, c)) continue;
        ok = d.finish(c, budgetBits, out, si);
    }
    if (!ok) {                                                  // nothing fits: a frame without bands
        out.b.clear();
        out.put(0, 1); out.put(0, 2); out.put(0, 1); out.put(0, 6); out.put(0, 1); out.put(0, 1); out.put(100, 8); out.put(0, 14); out.put(0, 6);
        si = (int)out.size();
    }
    frame.assign((size_t)budgetBytes, 0);
    for (size_t i = 0; i < out.size() && i < (size_t)budgetBits; i++) if (out.b[i]) frame[i >> 3] |= (uint8_t)(0x80 >> (i & 7));
    if (siBits) *siBits = si;
    return ok;
}

// ---------------------------------------------------------------- test sounds

namespace {

uint64_t mix64(uint64_t x) {
    x += 0x9e3779b97f4a7c15ull;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
    return x ^ (x >> 31);
}
double unit(uint64_t x) { return (double)(mix64(x) >> 11) / 9007199254740992.0; }   // [0, 1)

// a short tune (the first bars of Ode to Joy), frequencies in Hz and lengths in quarter notes
struct Note { double hz; double len; };
const Note kTune[] = {
    {329.63, 1}, {329.63, 1}, {349.23, 1}, {392.00, 1}, {392.00, 1}, {349.23, 1}, {329.63, 1}, {293.66, 1},
    {261.63, 1}, {261.63, 1}, {293.66, 1}, {329.63, 1}, {329.63, 1.5}, {293.66, 0.5}, {293.66, 2},
    {329.63, 1}, {329.63, 1}, {349.23, 1}, {392.00, 1}, {392.00, 1}, {349.23, 1}, {329.63, 1}, {293.66, 1},
    {261.63, 1}, {261.63, 1}, {293.66, 1}, {329.63, 1}, {293.66, 1.5}, {261.63, 0.5}, {261.63, 2},
};

} // namespace

void drmTestSound(DrmTestSound kind, int rateHz, uint32_t seed, uint64_t first, int n, float* out) {
    const double fs = (double)rateHz;
    switch (kind) {
    case DrmTestSound::kSilence:
        std::fill(out, out + n, 0.f);
        return;
    case DrmTestSound::kTone:
        for (int i = 0; i < n; i++) out[i] = (float)(0.5 * std::sin(2 * M_PI * 1000.0 * (double)((first + (uint64_t)i) % (uint64_t)rateHz) / fs));
        return;
    case DrmTestSound::kNoise:
        // a few tones that change every 100 ms plus white noise 34 dB down: no two frames are alike
        for (int i = 0; i < n; i++) {
            const uint64_t t = first + (uint64_t)i;
            const uint64_t seg = t / (uint64_t)(rateHz / 10);
            double v = 0;
            for (int k = 0; k < 5; k++) {
                const double f = 150.0 + 3000.0 * unit(seed * 7919u + seg * 13u + (uint64_t)k);
                const double a = 0.08 + 0.05 * unit(seed * 104729u + seg * 17u + (uint64_t)k);
                v += a * std::sin(2 * M_PI * f * (double)t / fs);
            }
            v += 0.02 * (2 * unit(((uint64_t)seed << 40) ^ (t * 2654435761ull)) - 1);
            out[i] = (float)v;
        }
        return;
    case DrmTestSound::kMelody:
    default: {
        const double q = 0.3 * fs;                              // a quarter note: 0.3 s
        double total = 0;
        for (const Note& nt : kTune) total += nt.len;
        const double period = total * q;
        for (int i = 0; i < n; i++) {
            double t = std::fmod((double)(first + (uint64_t)i), period);
            double start = 0;
            const Note* cur = &kTune[0];
            for (const Note& nt : kTune) { if (t < (start + nt.len * q)) { cur = &nt; break; } start += nt.len * q; }
            const double pos = t - start, len = cur->len * q;
            const double sec = pos / fs, secLen = len / fs;
            // attack 12 ms, decay to 70 %, release over the last 80 ms
            double env = std::min(1.0, sec / 0.012) * (0.7 + 0.3 * std::exp(-sec / 0.12));
            if (secLen - sec < 0.08) env *= std::max(0.0, (secLen - sec) / 0.08);
            const double ph = 2 * M_PI * cur->hz * (pos / fs);
            const double v = std::sin(ph) + 0.45 * std::sin(2 * ph) + 0.2 * std::sin(3 * ph) + 0.08 * std::sin(4 * ph);
            out[i] = (float)(0.25 * env * v);
        }
        return;
    }
    }
}

}} // namespace dect2::drm

// ---------------------------------------------------------------- the source for the test signal

namespace dect2 {
using namespace drm;

namespace {

class DrmAacSource : public DrmAudioSource {
public:
    DrmAacSource(int rateHz, DrmTestSound kind, uint32_t seed) : enc_(rateHz), rate_(rateHz), kind_(kind), seed_(seed) {}
    void nextSuperFrame(int numFrames, int payload, std::vector<std::vector<uint8_t>>& frames, std::vector<uint8_t>& crc) override {
        frames.assign((size_t)numFrames, {});
        crc.assign((size_t)numFrames, 0);
        const int base = payload / numFrames, extra = payload - base * numFrames;
        float pcm[aac::kFrame];
        for (int f = 0; f < numFrames; f++) {
            drmTestSound(kind_, rate_, seed_, block_ * (uint64_t)aac::kFrame, aac::kFrame, pcm);
            block_++;
            int si = 0;
            enc_.encode(pcm, base + (f < extra ? 1 : 0), frames[(size_t)f], &si);
            // the CRC covers the side information (clause 5.4.1: mono1 and mono2)
            std::vector<uint8_t> bits((size_t)si);
            for (int i = 0; i < si; i++) bits[(size_t)i] = (uint8_t)((frames[(size_t)f][(size_t)i >> 3] >> (7 - (i & 7))) & 1);
            crc[(size_t)f] = (uint8_t)crc8(bits.data(), bits.size());
        }
    }
    std::string describe() const override {
        static const char* names[] = {"melody", "1 kHz tone", "silence", "noise"};
        return std::string("AAC-LC ") + (rate_ == 12000 ? "12" : "24") + " kHz mono, " + names[(int)kind_];
    }
private:
    AacFrameEncoder enc_;
    int rate_;
    DrmTestSound kind_;
    uint32_t seed_;
    uint64_t block_ = 0;
};

} // namespace

std::unique_ptr<DrmAudioSource> makeDrmAacSource(const DrmTxConfig& cfg, int kind) {
    if (cfg.audioCoding != 0 || cfg.audioSbr != 0 || cfg.audioMode != 0) return nullptr;
    if (cfg.audioRateHz != 12000 && cfg.audioRateHz != 24000) return nullptr;
    if (kind < 0 || kind > 3) return nullptr;
    return std::make_unique<DrmAacSource>(cfg.audioRateHz, (DrmTestSound)kind, cfg.seed);
}

} // namespace dect2
