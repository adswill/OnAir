// DAB test transmitter: AAC-LC with the 960 transform for DAB+ (ISO/IEC 14496-3 raw_data_block of a channel pair element, long blocks, sine window, one
// scale factor per channel, no TNS / prediction / M/S) and the DAB+ audio superframe (TS 102 563). The transform and the quantiser follow
// drm_aacenc.cpp (the DRM test signal's encoder, which libavcodec's decoder accepts); the Huffman tables are the ones of drm_aactab.cpp. What differs
// from DRM: two channels, the standard syntax (no codeword reordering) and the sampling rates 32 and 48 kHz. libavcodec's own encoder cannot be used,
// it only makes 1024 sample frames.
#include "dect2/dab_gen.h"
#include "dect2/drm_fft.h"
#include "drm_aac_internal.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace dect2 {
namespace dabgen {

using namespace drm::aac;

namespace {

constexpr int kN = 960, kW = 1920;
constexpr double kPcmScale = 32768.0;

// scale factor bands of the 960 transform at 48 kHz (and 32 kHz, ISO/IEC 14496-3 table 4.x): 49 bands
const uint16_t kSwb[50] = {0, 4, 8, 12, 16, 20, 24, 28, 32, 36, 40, 48, 56, 64, 72, 80, 88, 96, 108, 120, 132, 144, 160, 176, 196, 216, 240, 264, 292, 320,
                           352, 384, 416, 448, 480, 512, 544, 576, 608, 640, 672, 704, 736, 768, 800, 832, 864, 896, 928, 960};
constexpr int kNumSfb = 49;

struct Bits {
    std::vector<uint8_t> b;      // one bit per byte
    void put(uint32_t v, int n) { for (int i = n - 1; i >= 0; i--) b.push_back((uint8_t)((v >> i) & 1u)); }
    size_t size() const { return b.size(); }
    void append(const Bits& o) { b.insert(b.end(), o.b.begin(), o.b.end()); }
};

// the bits of one code word (a pair or a quad): Huffman code, sign bits of an unsigned codebook, escape sequences of codebook 11
void unitBits(int cb, const int* v, Bits& out) {
    const CbInfo& ci = cbInfo(cb);
    int t[4];
    int esc[2] = {0, 0};
    for (int i = 0; i < ci.dim; i++) t[i] = ci.isSigned ? v[i] : std::abs(v[i]);
    if (cb == 11) for (int i = 0; i < 2; i++) if (t[i] > 15) { esc[i] = t[i]; t[i] = 16; }
    const int idx = cbIndex(cb, t);
    out.put(ci.code[idx], ci.bits[idx]);
    if (!ci.isSigned) for (int i = 0; i < ci.dim; i++) if (v[i]) out.put(v[i] < 0 ? 1 : 0, 1);
    if (cb == 11)
        for (int i = 0; i < 2; i++) {
            if (!esc[i]) continue;
            int nb = 4;
            while ((esc[i] >> (nb + 1)) != 0) nb++;
            for (int k = 4; k < nb; k++) out.put(1, 1);
            out.put(0, 1);
            out.put((uint32_t)(esc[i] - (1 << nb)), nb);
        }
}

int unitLength(int cb, const int* v) {
    const CbInfo& ci = cbInfo(cb);
    int t[4];
    int escLen = 0;
    for (int i = 0; i < ci.dim; i++) t[i] = ci.isSigned ? v[i] : std::abs(v[i]);
    if (cb == 11) for (int i = 0; i < 2; i++) if (t[i] > 15) { int nb = 4; while ((t[i] >> (nb + 1)) != 0) nb++; escLen += (nb - 4) + 1 + nb; t[i] = 16; }
    int len = ci.bits[cbIndex(cb, t)] + escLen;
    if (!ci.isSigned) for (int i = 0; i < ci.dim; i++) if (v[i]) len++;
    return len;
}

} // namespace

struct AacLcEncoder::Impl {
    int rate, ch;
    drm::DrmFft fft{kW};
    std::vector<float> win;
    std::vector<cf32> pre, post;
    std::vector<std::vector<float>> prev;     // per channel: the previous block
    std::vector<std::vector<float>> spec;     // per channel: the MDCT of previous and current block

    Impl(int r, int c) : rate(r), ch(c) {
        win.resize(kW);
        for (int n = 0; n < kW; n++) win[(size_t)n] = (float)std::sin(M_PI * (n + 0.5) / kW);
        pre.resize(kW);
        for (int n = 0; n < kW; n++) { const double a = -M_PI * n / kW; pre[(size_t)n] = cf32((float)std::cos(a), (float)std::sin(a)); }
        post.resize(kN);
        const double n0 = (kN + 1) / 2.0;
        for (int k = 0; k < kN; k++) { const double a = -2 * M_PI * n0 * (k + 0.5) / kW; post[(size_t)k] = cf32((float)std::cos(a), (float)std::sin(a)); }
        prev.assign((size_t)ch, std::vector<float>(kN, 0.f));
        spec.assign((size_t)ch, std::vector<float>(kN, 0.f));
    }

    void mdct(int c, const float* cur) {       // cur: kN samples of channel c
        std::vector<cf32> y(kW);
        std::vector<float>& pv = prev[(size_t)c];
        for (int n = 0; n < kW; n++) {
            const float x = n < kN ? pv[(size_t)n] : cur[n - kN];
            y[(size_t)n] = pre[(size_t)n] * (x * win[(size_t)n] * (float)kPcmScale);
        }
        fft.forward(y.data());
        for (int k = 0; k < kN; k++) spec[(size_t)c][(size_t)k] = 2.f * (y[(size_t)k] * post[(size_t)k]).real();
        std::memcpy(pv.data(), cur, sizeof(float) * kN);
    }

    // The individual_channel_stream of channel c with global gain gg (common_window = 0): global_gain, ics_info, section_data, scale_factor_data, no pulse,
    // TNS and gain control, spectral_data. false when a value is too large.
    bool channelStream(int c, int gg, Bits& s) const {
        const double g = std::pow(2.0, (gg - 100) / 4.0);
        std::vector<int> q(kN);
        for (int i = 0; i < kN; i++) {
            const double a = std::fabs((double)spec[(size_t)c][(size_t)i]) / g;
            const int v = (int)(std::pow(a, 0.75) + 0.4054);
            if (v > 8000) return false;
            q[(size_t)i] = spec[(size_t)c][(size_t)i] < 0 ? -v : v;
        }
        int last = -1;
        for (int b = 0; b < kNumSfb; b++) for (int i = kSwb[b]; i < kSwb[b + 1]; i++) if (q[(size_t)i]) last = b;
        const int maxSfb = last + 1;
        int cbOf[kNumSfb] = {0};
        for (int b = 0; b < maxSfb; b++) {
            int mx = 0;
            for (int i = kSwb[b]; i < kSwb[b + 1]; i++) mx = std::max(mx, std::abs(q[(size_t)i]));
            if (!mx) continue;
            int bestCb = 0, bestBits = 1 << 30;
            for (int cb = 1; cb <= 11; cb++) {
                const CbInfo& ci = cbInfo(cb);
                if (cb != 11 && mx > ci.lav) continue;
                int bits = 0;
                for (int i = kSwb[b]; i < kSwb[b + 1]; i += ci.dim) bits += unitLength(cb, &q[(size_t)i]);
                if (bits < bestBits) { bestBits = bits; bestCb = cb; }
            }
            cbOf[b] = bestCb;
        }
        s.put((uint32_t)gg, 8);
        s.put(0, 1); s.put(0, 2); s.put(0, 1);              // ics_reserved_bit, ONLY_LONG_SEQUENCE, sine window
        s.put((uint32_t)maxSfb, 6);
        s.put(0, 1);                                        // predictor_data_present
        for (int b = 0; b < maxSfb;) {                      // section_data: codebook (4 bits) and length (5 bits, escape 31)
            int e = b;
            while (e < maxSfb && cbOf[e] == cbOf[b]) e++;
            s.put((uint32_t)cbOf[b], 4);
            int len = e - b;
            while (len >= 31) { s.put(31, 5); len -= 31; }
            s.put((uint32_t)len, 5);
            b = e;
        }
        for (int b = 0; b < maxSfb; b++) if (cbOf[b]) s.put(kScaleCode[60], kScaleBits[60]);   // every scale factor equals the global gain: difference 0
        s.put(0, 1); s.put(0, 1); s.put(0, 1);              // pulse_data_present, tns_data_present, gain_control_data_present
        for (int b = 0; b < maxSfb; b++) {
            if (!cbOf[b]) continue;
            const CbInfo& ci = cbInfo(cbOf[b]);
            for (int i = kSwb[b]; i < kSwb[b + 1]; i += ci.dim) unitBits(cbOf[b], &q[(size_t)i], s);
        }
        return true;
    }

    // the smallest global gain (finest quantiser) whose channel stream fits in `bits`
    bool fit(int c, int bits, Bits& out) const {
        int lo = 40, hi = 255;
        auto fits = [&](int gg, Bits& b) { b.b.clear(); return channelStream(c, gg, b) && (int)b.size() <= bits; };
        Bits t;
        while (lo < hi) {
            const int mid = (lo + hi) / 2;
            if (fits(mid, t)) hi = mid; else lo = mid + 1;
        }
        for (int gg = lo; gg <= 255; gg++) if (fits(gg, out)) return true;     // the bisection assumes monotonic sizes: step up until it fits
        return false;
    }
};

AacLcEncoder::AacLcEncoder(int rateHz, int channels) : p_(std::make_unique<Impl>(rateHz == 32000 ? 32000 : 48000, channels == 1 ? 1 : 2)) {}
AacLcEncoder::~AacLcEncoder() = default;
int AacLcEncoder::rate() const { return p_->rate; }
int AacLcEncoder::channels() const { return p_->ch; }
void AacLcEncoder::reset() { for (auto& v : p_->prev) std::fill(v.begin(), v.end(), 0.f); }

bool AacLcEncoder::encode(const float* pcm, int budgetBytes, std::vector<uint8_t>& block) {
    Impl& d = *p_;
    std::vector<float> one((size_t)kN);
    for (int c = 0; c < d.ch; c++) {
        for (int i = 0; i < kN; i++) one[(size_t)i] = pcm[(size_t)i * (size_t)d.ch + (size_t)c];
        d.mdct(c, one.data());
    }
    const int total = budgetBytes * 8;
    Bits out;
    bool ok = true;
    // single_channel_element: id 0 (3 bits), instance tag (4 bits); channel_pair_element: id 1, tag, common_window = 0; then ID_END
    out.put(d.ch == 1 ? 0 : 1, 3); out.put(0, 4);
    if (d.ch == 2) out.put(0, 1);
    const int overhead = (int)out.size() + 3;
    int left = total - overhead;
    for (int c = 0; c < d.ch && ok; c++) {
        Bits s;
        const int share = c == d.ch - 1 ? left : left / d.ch;
        if (d.fit(c, share, s)) { left -= (int)s.size(); out.append(s); } else ok = false;
    }
    if (!ok) {         // nothing fits: a block without bands
        out.b.clear();
        out.put(d.ch == 1 ? 0 : 1, 3); out.put(0, 4);
        if (d.ch == 2) out.put(0, 1);
        for (int c = 0; c < d.ch; c++) {
            out.put(100, 8); out.put(0, 1); out.put(0, 2); out.put(0, 1); out.put(0, 6); out.put(0, 1);
            out.put(0, 1); out.put(0, 1); out.put(0, 1);
        }
    }
    out.put(7, 3);     // ID_END
    block.assign((size_t)budgetBytes, 0);
    for (size_t i = 0; i < out.size() && i < (size_t)total; i++) if (out.b[i]) block[i >> 3] |= (uint8_t)(0x80 >> (i & 7));
    return ok;
}

// ---------------------------------------------------------------- DAB+ audio superframe
void superframeLayout(int bitrate, int nAu, int* firstStart, std::vector<int>* auLen) {
    const int N = bitrate / 8;
    const int fs = 3 + (12 * (nAu - 1) + 7) / 8;        // firecode (2), header (1), the 12 bit start table of access units 1 .. nAu-1
    if (firstStart) *firstStart = fs;
    if (auLen) {
        const int avail = 110 * N - fs;
        auLen->assign((size_t)nAu, avail / nAu);
        auLen->back() = avail - (nAu - 1) * (avail / nAu);
    }
}

std::vector<uint8_t> buildSuperframe(const std::vector<std::vector<uint8_t>>& aus, int bitrate, bool dac48, bool stereo) {
    const int N = bitrate / 8, nAu = (int)aus.size();
    std::vector<uint8_t> pay((size_t)(110 * N), 0);
    int fs = 0;
    superframeLayout(bitrate, nAu, &fs, nullptr);
    pay[2] = (uint8_t)((dac48 ? 0x40 : 0) | (stereo ? 0x10 : 0));       // rfa, dac_rate, sbr_flag = 0, aac_channel_mode, ps_flag = 0, mpeg_surround = 0
    int pos = fs;
    for (int k = 0; k < nAu; k++) {
        if (k > 0) {                                     // au_start[k], 12 bits each, from byte 3
            const int bit = 12 * (k - 1), byte = 3 + bit / 8;
            if (bit % 8 == 0) { pay[(size_t)byte] = (uint8_t)(pos >> 4); pay[(size_t)byte + 1] = (uint8_t)((pay[(size_t)byte + 1] & 0x0F) | ((pos & 15) << 4)); }
            else { pay[(size_t)byte] = (uint8_t)((pay[(size_t)byte] & 0xF0) | (pos >> 8)); pay[(size_t)byte + 1] = (uint8_t)pos; }
        }
        std::memcpy(&pay[(size_t)pos], aus[(size_t)k].data(), aus[(size_t)k].size());
        pos += (int)aus[(size_t)k].size();
    }
    const uint16_t fc = fireCode(&pay[2], 9);
    pay[0] = (uint8_t)(fc >> 8); pay[1] = (uint8_t)fc;
    // 5 x ... RS(120,110) codewords, byte interleaved: codeword i holds the payload bytes i, i + N, i + 2N ...
    std::vector<uint8_t> sf((size_t)(120 * N));
    for (int i = 0; i < N; i++) {
        uint8_t data[110], cw[120];
        for (int j = 0; j < 110; j++) data[j] = pay[(size_t)j * (size_t)N + (size_t)i];
        rsEncode(data, cw);
        for (int j = 0; j < 120; j++) sf[(size_t)j * (size_t)N + (size_t)i] = cw[j];
    }
    return sf;
}

} // namespace dabgen
} // namespace dect2
