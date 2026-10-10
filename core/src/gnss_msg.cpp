// Coded navigation messages: SBAS L1 and Galileo I/NAV (see gnss_msg.h).
#include "dect2/gnss_msg.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace dect2 {

namespace {
// the register is (input << 6) | state: bit 6 the current input, bit 5 the one before, ... bit 0 six before
constexpr unsigned kG1 = 0171, kG2 = 0133;
inline int parity7(unsigned v) { return __builtin_popcount(v & 0x7F) & 1; }

// G(X) = (1 + X) P(X), P(X) = X^23 + X^17 + X^13 + X^12 + X^11 + X^9 + X^8 + X^7 + X^5 + X^3 + 1 (Galileo OS SIS ICD Eq. 24, 25)
constexpr uint32_t crcPoly() {
    const uint32_t p = (1u << 23) | (1u << 17) | (1u << 13) | (1u << 12) | (1u << 11) | (1u << 9) | (1u << 8) | (1u << 7) | (1u << 5) | (1u << 3) | 1u;
    return (p << 1) ^ p;          // 25 bits, X^24 included
}
}

int convEncode(const uint8_t* bits, int n, bool inv, int state, uint8_t* out) {
    unsigned s = (unsigned)state & 63;
    for (int i = 0; i < n; i++) {
        const unsigned r = ((unsigned)(bits[i] & 1) << 6) | s;
        out[2 * i] = (uint8_t)parity7(r & kG1);
        out[2 * i + 1] = (uint8_t)(parity7(r & kG2) ^ (inv ? 1 : 0));
        s = r >> 1;
    }
    return (int)s;
}

void viterbiDecode(const float* soft, int n, bool inv, int startState, int endState, uint8_t* out) {
    // expected symbols of each (state, input): as +1 for logic 0, -1 for logic 1
    struct Tab { float e1[128], e2[128]; };
    static const Tab tab = [] {
        Tab t;
        for (unsigned r = 0; r < 128; r++) { t.e1[r] = parity7(r & kG1) ? -1.f : 1.f; t.e2[r] = parity7(r & kG2) ? -1.f : 1.f; }
        return t;
    }();
    const float* e1 = tab.e1;
    const float* e2 = tab.e2;
    const float s2 = inv ? -1.f : 1.f;
    std::vector<float> pm(64), nm(64);
    std::vector<uint8_t> dec((size_t)n * 64);       // the previous state's lowest bit (the one that drops out) for each new state
    const float kNeg = -1e30f;
    for (int s = 0; s < 64; s++) pm[s] = (startState < 0 || s == startState) ? 0.f : kNeg;
    for (int i = 0; i < n; i++) {
        const float a = soft[2 * i], b = soft[2 * i + 1] * s2;
        for (int ns = 0; ns < 64; ns++) {
            // new state ns = (input << 5) | (old >> 1): input = ns >> 5, old = ((ns & 31) << 1) | x
            const unsigned in = (unsigned)ns >> 5;
            float best = kNeg; int bx = 0;
            for (int x = 0; x < 2; x++) {
                const unsigned old = ((unsigned)(ns & 31) << 1) | (unsigned)x;
                const unsigned r = (in << 6) | old;
                const float m = pm[old] + a * e1[r] + b * e2[r];
                if (m > best) { best = m; bx = x; }
            }
            nm[ns] = best;
            dec[(size_t)i * 64 + ns] = (uint8_t)bx;
        }
        pm.swap(nm);
        // keep the metrics small
        float mx = kNeg;
        for (float v : pm) mx = std::max(mx, v);
        if (mx > 1e6f) for (auto& v : pm) if (v > kNeg / 2) v -= mx;
    }
    int s = 0;
    if (endState >= 0) s = endState;
    else { float mx = kNeg; for (int k = 0; k < 64; k++) if (pm[k] > mx) { mx = pm[k]; s = k; } }
    for (int i = n - 1; i >= 0; i--) {
        out[i] = (uint8_t)(s >> 5);
        s = ((s & 31) << 1) | dec[(size_t)i * 64 + s];
    }
}

uint32_t crc24q(const uint8_t* bits, int n) {
    const uint32_t g = crcPoly();
    uint32_t r = 0;
    for (int i = 0; i < n; i++) {
        const uint32_t fb = ((r >> 23) & 1u) ^ (bits[i] & 1u);
        r = (r << 1) & 0xFFFFFFu;
        if (fb) r ^= g & 0xFFFFFFu;
    }
    return r;
}

// ------------------------------------------------------------------ SBAS
void sbasBuildMessage(int index, int type, const uint8_t* data, uint8_t* out) {
    const uint8_t pre = kSbasPreamble[((index % 3) + 3) % 3];
    for (int i = 0; i < 8; i++) out[i] = (uint8_t)((pre >> (7 - i)) & 1);
    for (int i = 0; i < 6; i++) out[8 + i] = (uint8_t)((type >> (5 - i)) & 1);
    for (int i = 0; i < 212; i++) out[14 + i] = data[i] & 1;
    const uint32_t c = crc24q(out, 226);
    for (int i = 0; i < 24; i++) out[226 + i] = (uint8_t)((c >> (23 - i)) & 1);
}

std::vector<SbasMessage> sbasFindMessages(const float* soft, int n, int margin) {
    std::vector<SbasMessage> res;
    if (n < kSbasMsgBits + 2 * margin) return res;
    std::vector<uint8_t> b((size_t)n);
    viterbiDecode(soft, n, false, -1, -1, b.data());
    for (int pol = 0; pol < 2; pol++) {
        // the code is transparent: inverted symbols decode to the inverted bits
        std::vector<uint8_t> d(b);
        if (pol) for (auto& v : d) v ^= 1;
        for (int p = margin; p + kSbasMsgBits <= n - margin; p++) {
            unsigned v = 0;
            for (int i = 0; i < 8; i++) v = v << 1 | d[(size_t)p + i];
            if (v != kSbasPreamble[0] && v != kSbasPreamble[1] && v != kSbasPreamble[2]) continue;
            const uint8_t* m = &d[(size_t)p];
            uint32_t c = 0;
            for (int i = 0; i < 24; i++) c = c << 1 | m[226 + i];
            if (crc24q(m, 226) != c) continue;
            SbasMessage msg;
            msg.bitPos = p;
            msg.type = 0;
            for (int i = 0; i < 6; i++) msg.type = msg.type << 1 | m[8 + i];
            std::memcpy(msg.bits, m, kSbasMsgBits);
            res.push_back(msg);
        }
        if (!res.empty()) break;
    }
    return res;
}

// ------------------------------------------------------------------ Galileo I/NAV
void inavInterleave(const uint8_t* in, uint8_t* out) {
    // the encoded symbols are written column by column (30 columns of 8) and read out row by row (8 rows of 30)
    for (int k = 0; k < 240; k++) out[k] = in[(k % 30) * 8 + k / 30];
}
void inavDeinterleave(const float* in, float* out) {
    for (int k = 0; k < 240; k++) out[(k % 30) * 8 + k / 30] = in[k];
}

void inavEncodePart(const uint8_t* bits120, uint8_t* out250) {
    uint8_t enc[240];
    convEncode(bits120, 120, true, 0, enc);
    std::memcpy(out250, kInavSync, 10);
    inavInterleave(enc, out250 + 10);
}

void inavDecodePart(const float* soft240, uint8_t* bits120) {
    float d[240];
    inavDeinterleave(soft240, d);
    viterbiDecode(d, 120, true, 0, 0, bits120);
}

uint32_t inavField(const uint8_t* w, int first, int len) {
    uint32_t v = 0;
    for (int i = 0; i < len; i++) v = v << 1 | (w[first + i] & 1u);
    return v;
}
int32_t inavFieldSigned(const uint8_t* w, int first, int len) {
    const uint32_t v = inavField(w, first, len);
    if (len < 32 && (v >> (len - 1)) & 1u) return (int32_t)(v | ~((1u << len) - 1));
    return (int32_t)v;
}
void inavPut(uint8_t* w, int first, int len, uint32_t v) {
    for (int i = 0; i < len; i++) w[first + i] = (uint8_t)((v >> (len - 1 - i)) & 1u);
}

// The CRC covers the even part's Even/Odd, Page Type and Data (1/2) and the odd part's Even/Odd, Page Type, Data (2/2), OSNMA, SAR and Spare (ICD Table 36)
static void inavCrcInput(const uint8_t* even, const uint8_t* odd, uint8_t* m196) {
    std::memcpy(m196, even, 114);
    std::memcpy(m196 + 114, odd, 82);
}

void inavBuildPage(const uint8_t* word, const uint8_t* oss, int ssp, uint8_t* even, uint8_t* odd) {
    static const uint8_t kSsp[3] = {0x04, 0x2B, 0x2F};          // SSP1..3 before coding (ICD Table 83)
    std::memset(even, 0, 120); std::memset(odd, 0, 120);
    even[0] = 0; even[1] = 0;
    std::memcpy(even + 2, word, 112);
    odd[0] = 1; odd[1] = 0;
    std::memcpy(odd + 2, word + 112, 16);
    if (oss) std::memcpy(odd + 18, oss, 64);
    uint8_t m[196];
    inavCrcInput(even, odd, m);
    const uint32_t c = crc24q(m, 196);
    for (int i = 0; i < 24; i++) odd[82 + i] = (uint8_t)((c >> (23 - i)) & 1);
    const uint8_t sp = kSsp[((ssp % 3) + 3) % 3];
    for (int i = 0; i < 8; i++) odd[106 + i] = (uint8_t)((sp >> (7 - i)) & 1);
}

bool inavCheckPage(const uint8_t* even, const uint8_t* odd, uint8_t* word) {
    if (even[0] != 0 || odd[0] != 1 || even[1] != 0 || odd[1] != 0) return false;     // a nominal page, even part first
    uint8_t m[196];
    inavCrcInput(even, odd, m);
    uint32_t c = 0;
    for (int i = 0; i < 24; i++) c = c << 1 | odd[82 + i];
    if (crc24q(m, 196) != c) return false;
    std::memcpy(word, even + 2, 112);
    std::memcpy(word + 112, odd + 2, 16);
    return true;
}

int inavParseWord(const uint8_t* w, GalNav& g, int* tow) {
    const int type = (int)inavField(w, 0, 6);
    if (tow) *tow = -1;
    GpsEphemeris& e = g.eph;
    e.galileo = true;
    const double sc = kPi;
    auto setIod = [&](int k) {
        const int iod = (int)inavField(w, 6, 10);
        // a new data set: forget the words of the old one
        for (int j = 1; j <= 4; j++) if (j != k && g.iod[j] >= 0 && g.iod[j] != iod) g.iod[j] = -1;
        g.iod[k] = iod;
        e.iode2 = e.iode3 = e.iodc = iod;
    };
    switch (type) {
    case 1:
        setIod(1);
        e.toe = inavField(w, 16, 14) * 60.0;
        e.m0 = inavFieldSigned(w, 30, 32) * std::ldexp(1.0, -31) * sc;
        e.e = inavField(w, 62, 32) * std::ldexp(1.0, -33);
        e.sqrtA = inavField(w, 94, 32) * std::ldexp(1.0, -19);
        break;
    case 2:
        setIod(2);
        e.omega0 = inavFieldSigned(w, 16, 32) * std::ldexp(1.0, -31) * sc;
        e.i0 = inavFieldSigned(w, 48, 32) * std::ldexp(1.0, -31) * sc;
        e.omega = inavFieldSigned(w, 80, 32) * std::ldexp(1.0, -31) * sc;
        e.idot = inavFieldSigned(w, 112, 14) * std::ldexp(1.0, -43) * sc;
        break;
    case 3:
        setIod(3);
        e.omegaDot = inavFieldSigned(w, 16, 24) * std::ldexp(1.0, -43) * sc;
        e.dn = inavFieldSigned(w, 40, 16) * std::ldexp(1.0, -43) * sc;
        e.cuc = inavFieldSigned(w, 56, 16) * std::ldexp(1.0, -29);
        e.cus = inavFieldSigned(w, 72, 16) * std::ldexp(1.0, -29);
        e.crc = inavFieldSigned(w, 88, 16) * std::ldexp(1.0, -5);
        e.crs = inavFieldSigned(w, 104, 16) * std::ldexp(1.0, -5);
        e.uraIndex = (int)inavField(w, 120, 8);
        break;
    case 4:
        setIod(4);
        g.svid = (int)inavField(w, 16, 6);
        e.cic = inavFieldSigned(w, 22, 16) * std::ldexp(1.0, -29);
        e.cis = inavFieldSigned(w, 38, 16) * std::ldexp(1.0, -29);
        e.toc = inavField(w, 54, 14) * 60.0;
        e.af0 = inavFieldSigned(w, 68, 31) * std::ldexp(1.0, -34);
        e.af1 = inavFieldSigned(w, 99, 21) * std::ldexp(1.0, -46);
        e.af2 = inavFieldSigned(w, 120, 6) * std::ldexp(1.0, -59);
        break;
    case 5:
        g.ai[0] = inavField(w, 6, 11) * std::ldexp(1.0, -2);
        g.ai[1] = inavFieldSigned(w, 17, 11) * std::ldexp(1.0, -8);
        g.ai[2] = inavFieldSigned(w, 28, 14) * std::ldexp(1.0, -15);
        g.bgdE1E5a = inavFieldSigned(w, 47, 10) * std::ldexp(1.0, -32);
        g.bgdE1E5b = inavFieldSigned(w, 57, 10) * std::ldexp(1.0, -32);
        g.e1bHs = (int)inavField(w, 69, 2);
        g.e1bDvs = (int)inavField(w, 72, 1);
        g.wn = (int)inavField(w, 73, 12);
        if (tow) *tow = (int)inavField(w, 85, 20);
        g.w5 = true;
        e.tgd = g.bgdE1E5b;
        e.health = (g.e1bHs != 0 || g.e1bDvs != 0) ? 1 : 0;
        break;
    case 6:
        if (tow) *tow = (int)inavField(w, 105, 20);
        break;
    case 10: {
        const uint32_t a0 = inavField(w, 86, 16), a1 = inavField(w, 102, 12), t0 = inavField(w, 114, 8), wn0 = inavField(w, 122, 6);
        // all ones: no offset available (ICD 5.1.8)
        if (a0 == 0xFFFF && a1 == 0xFFF && t0 == 0xFF && wn0 == 0x3F) { g.ggtoValid = false; break; }
        g.a0g = inavFieldSigned(w, 86, 16) * std::ldexp(1.0, -35);
        g.a1g = inavFieldSigned(w, 102, 12) * std::ldexp(1.0, -51);
        g.t0g = t0 * 3600.0;
        g.wn0g = (int)wn0;
        g.ggtoValid = true;
        break;
    }
    case 0:
        if (tow && inavField(w, 6, 2) == 2) { *tow = (int)inavField(w, 108, 20); g.wn = (int)inavField(w, 96, 12); }
        break;
    default: break;
    }
    return type;
}

} // namespace dect2
