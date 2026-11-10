#include "dect2/umts/umts_bch.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace dect2 {
namespace umts {

namespace {
const int kP2[30] = {0, 20, 10, 5, 15, 25, 3, 13, 23, 8, 18, 28, 1, 11, 21, 6, 16, 26, 4, 14, 24, 19, 9, 29, 12, 2, 7, 22, 27, 17};
void crc16(const uint8_t* b, int n, uint8_t* parity) {     // gCRC16 = D16 + D12 + D5 + 1; p1 is the highest
    unsigned r = 0;
    for (int i = 0; i < n; i++) { const unsigned fb = ((r >> 15) & 1) ^ b[i]; r = (r << 1) & 0xFFFF; if (fb) r ^= 0x1021; }
    for (int i = 0; i < 16; i++) parity[i] = (r >> i) & 1;   // transmitted in reversed order (the lowest parity first): TS 25.212 4.2.1.2
}
inline void enc(unsigned& s, int b, int out[2]) {
    const unsigned r = ((s << 1) | (unsigned)b) & 0x1FF;
    out[0] = __builtin_parity(r & 0x171); out[1] = __builtin_parity(r & 0x1EB);   // G0 = 561 octal, G1 = 753 octal (bit 8 of the mask = newest input is the MSB in the octal form)
    s = r & 0xFF;
}
unsigned revBits(unsigned m) { unsigned r = 0; for (int i = 0; i < 9; i++) if (m & (1u << i)) r |= 1u << (8 - i); return r; }
void encodeBits(const uint8_t* in, int n, uint8_t* out) {
    static const unsigned g0 = revBits(0561), g1 = revBits(0753);
    unsigned s = 0;
    for (int i = 0; i < n; i++) {
        const unsigned r = ((s << 1) | in[i]) & 0x1FF;
        out[2 * i] = (uint8_t)__builtin_parity(r & g0); out[2 * i + 1] = (uint8_t)__builtin_parity(r & g1);
        s = r & 0xFF;
    }
}
// second interleaver of a frame of 270 bits: written row by row into 30 columns, columns permuted, read column by column
void interleave2(const uint8_t* in, uint8_t* out) { int p = 0; for (int c = 0; c < 30; c++) for (int r = 0; r < 9; r++) out[p++] = in[r * 30 + kP2[c]]; }
void deinterleave2(const float* in, float* out) { int p = 0; for (int c = 0; c < 30; c++) for (int r = 0; r < 9; r++) out[r * 30 + kP2[c]] = in[p++]; }
}  // namespace

std::vector<uint8_t> bchEncode(const uint8_t* tb) {
    uint8_t a[kBchInfo + 16 + 8] = {};
    memcpy(a, tb, kBchInfo);
    uint8_t par[16]; crc16(tb, kBchInfo, par);
    for (int i = 0; i < 16; i++) a[kBchInfo + i] = par[15 - i];
    uint8_t c[540]; encodeBits(a, kBchBits, c);
    // first interleaver (2 columns) and frame segmentation: odd/even coded bits go to frame 0 / 1
    uint8_t f0[270], f1[270], g0[270], g1[270];
    for (int i = 0; i < 270; i++) { f0[i] = c[2 * i]; f1[i] = c[2 * i + 1]; }
    interleave2(f0, g0); interleave2(f1, g1);
    std::vector<uint8_t> o(g0, g0 + 270); o.insert(o.end(), g1, g1 + 270);
    return o;
}

bool bchDecode(const float* soft, uint8_t* tb) {
    float f0[270], f1[270];
    deinterleave2(soft, f0); deinterleave2(soft + 270, f1);
    static const unsigned g0 = revBits(0561), g1 = revBits(0753);
    const int nSt = 256;
    std::vector<float> m(nSt, -1e9f), nw(nSt);
    m[0] = 0;
    std::vector<uint8_t> dec((size_t)nSt * 270);
    for (int i = 0; i < 270; i++) {
        std::fill(nw.begin(), nw.end(), -1e9f);
        const float y0 = f0[i], y1 = f1[i];
        for (int s = 0; s < nSt; s++) {
            if (m[s] < -1e8f) continue;
            for (int b = 0; b < 2; b++) {
                const unsigned r = (((unsigned)s << 1) | (unsigned)b) & 0x1FF;
                const int o0 = __builtin_parity(r & g0), o1 = __builtin_parity(r & g1);
                const float v = m[s] + (o0 ? -y0 : y0) + (o1 ? -y1 : y1);
                const int ns = r & 0xFF;
                if (v > nw[ns]) { nw[ns] = v; dec[(size_t)i * nSt + ns] = (uint8_t)s; }
            }
        }
        m.swap(nw);
    }
    int s = 0; std::vector<uint8_t> bits(270);
    for (int i = 269; i >= 0; i--) { bits[i] = s & 1; s = dec[(size_t)i * nSt + s]; }
    memcpy(tb, bits.data(), kBchInfo);
    uint8_t par[16]; crc16(tb, kBchInfo, par);
    for (int i = 0; i < 16; i++) if (bits[kBchInfo + i] != par[15 - i]) return false;
    return true;
}

namespace {
struct PutB { std::vector<uint8_t>& v; void put(uint32_t x, int n) { for (int i = n - 1; i >= 0; i--) v.push_back((x >> i) & 1); } };
struct GetB { const uint8_t* p; int pos, n; int get(int k) { int v = 0; for (int i = 0; i < k && pos < n; i++) v = v << 1 | p[pos++]; return v; } };
std::vector<uint8_t> completeBlock(int type, const std::vector<uint8_t>& sibBits, int sfnPrime) {
    std::vector<uint8_t> v; v.reserve(246); PutB b{v};
    b.put((uint32_t)(sfnPrime & 2047), 11); b.put(9, 4);   // the payload CHOICE: completeSIB is alternative 9
    b.put((uint32_t)type, 5);
    for (uint8_t x : sibBits) v.push_back(x);
    v.resize(246, 0);
    return v;
}
}  // namespace

std::vector<uint8_t> bchMakeMib(int mcc, int mnc, int mncDigits, int valueTag, int sfnPrime) {
    std::vector<uint8_t> v; PutB b{v};
    b.put(0, 1);                                 // no extension
    b.put((uint32_t)(valueTag - 1), 3);
    b.put(0, 2);                                 // plmn-Type: gsm-MAP
    b.put((uint32_t)(mcc / 100) % 10, 4); b.put((uint32_t)(mcc / 10) % 10, 4); b.put((uint32_t)mcc % 10, 4);
    b.put((uint32_t)(mncDigits - 2), 1);
    if (mncDigits == 3) b.put((uint32_t)(mnc / 100) % 10, 4);
    b.put((uint32_t)(mnc / 10) % 10, 4); b.put((uint32_t)mnc % 10, 4);
    return completeBlock(0, v, sfnPrime);
}
std::vector<uint8_t> bchMakeSib3(uint32_t cell, int sfnPrime) {
    std::vector<uint8_t> v; PutB b{v};
    b.put(0, 1);                                 // no extension
    b.put(0, 1);                                 // sib4indicator
    b.put(cell >> 16, 12); b.put(cell & 0xFFFF, 16);
    return completeBlock(3, v, sfnPrime);
}

void BchParser::sib(int type, const std::vector<uint8_t>& bits) {
    GetB g{bits.data(), 0, (int)bits.size()};
    if (type == 0) {
        g.get(1); g.get(3);                                // the extension flag, mib-ValueTag
        const int plmn = g.get(2);
        if (plmn == 0 || plmn == 2) {
            int mcc = g.get(4) * 100; mcc += g.get(4) * 10; mcc += g.get(4);
            const int digits = g.get(1) + 2;
            int mnc = 0; for (int i = 0; i < digits; i++) mnc = mnc * 10 + g.get(4);
            info_.mcc = mcc; info_.mnc = mnc; info_.mncDigits = digits; info_.haveMib = true;
        }
    } else if (type == 3) {
        g.get(1); g.get(1);                                // extension flag, sib4indicator
        const uint32_t hi = (uint32_t)g.get(12), lo = (uint32_t)g.get(16);
        info_.cellIdentity = hi << 16 | lo; info_.haveSib3 = true;
    }
}

void BchParser::feed(const uint8_t* tb) {
    GetB g{tb, 0, 246};
    info_.sfnPrime = g.get(11) * 2;
    const int ch = g.get(4);
    auto rest = [&](int bits) { std::vector<uint8_t> v; for (int i = 0; i < bits && g.pos < g.n; i++) v.push_back(tb[g.pos++]); return v; };
    auto firstSeg = [&](int bits) { const int t = g.get(5), c = g.get(4) + 1; seg_[t] = rest(bits); segNext_[t] = 1; segCount_[t] = c; };
    auto nextSeg = [&](int bits, bool last) { const int t = g.get(5), ix = g.get(4) + 1; auto d = rest(bits); if (segNext_.count(t) && ix == segNext_[t]) { auto& v = seg_[t]; v.insert(v.end(), d.begin(), d.end()); segNext_[t]++; if (last || segNext_[t] >= segCount_[t]) { sib(t, v); segNext_.erase(t); seg_.erase(t); } } };
    auto shortLen = [&]() { return g.get(8) + 1; };
    auto completeList = [&]() { const int cnt = g.get(4) + 1; for (int i = 0; i < cnt && g.pos < g.n; i++) { const int t = g.get(5); const int len = shortLen(); sib(t, rest(len)); } };
    auto lastShort = [&]() { const int t = g.get(5), ix = g.get(4) + 1; const int len = shortLen(); auto d = rest(len); if (segNext_.count(t) && ix == segNext_[t]) { auto& v = seg_[t]; v.insert(v.end(), d.begin(), d.end()); sib(t, v); segNext_.erase(t); seg_.erase(t); } };
    auto firstShort = [&]() { const int t = g.get(5), c = g.get(4) + 1; const int len = shortLen(); seg_[t] = rest(len); segNext_[t] = 1; segCount_[t] = c; };
    switch (ch) {
    case 1: firstSeg(222); break;
    case 2: nextSeg(222, false); break;
    case 3: lastShort(); break;
    case 4: lastShort(); firstShort(); break;
    case 5: lastShort(); completeList(); break;
    case 6: lastShort(); completeList(); firstShort(); break;
    case 7: completeList(); break;
    case 8: completeList(); firstShort(); break;
    case 9: { const int t = g.get(5); sib(t, rest(226)); } break;
    case 10: nextSeg(222, true); break;
    default: break;
    }
}

}  // namespace umts
}  // namespace dect2
