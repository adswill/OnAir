// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// HT-mixed PHY, one stream, 20 MHz (see ht_phy.h; clause numbers: IEEE 802.11-2020). Convolutional code, Viterbi and scrambler are those of the non-HT receiver.
#include "dect2/xlink/ht_phy.h"
#include "dect2/common/mathconst.h"
#include "dect2/dsp/fftutil.h"
#include "dect2/wifi/wifi_ofdm.h"
#include "dect2/wifi/wifi_proto.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace dect2::xlink::ht {

using dect2::math::kPi;
using namespace dect2::wifi::ofdm;

namespace {
constexpr int kFftN = 64, kCpN = 16;
constexpr int kOffLsig = 128, kOffHtSig = 208, kOffHtLtf = 448, kOffData = 528;   // from the start of the first L-LTF symbol
constexpr int kW = 80, kLag = 16, kMinRun = 24, kSyncWait = 800, kSearch = 560, kKeep = 1600;
constexpr float kThresh = 0.5f;

const int8_t kLtf[53] = {1, 1, -1, -1, 1, 1, -1, 1, -1, 1, 1, 1, 1, 1, 1, -1, -1, 1, 1, -1, 1, -1, 1, 1, 1, 1, 0,
                         1, -1, -1, 1, 1, -1, 1, -1, 1, -1, -1, -1, -1, -1, 1, 1, -1, -1, 1, -1, 1, -1, 1, 1, 1, 1};
const int8_t kStf[53][2] = {
    {0,0},{0,0},{1,1},{0,0},{0,0},{0,0},{-1,-1},{0,0},{0,0},{0,0},{1,1},{0,0},{0,0},{0,0},{-1,-1},{0,0},{0,0},{0,0},{-1,-1},{0,0},{0,0},{0,0},{1,1},{0,0},{0,0},{0,0},
    {0,0},
    {0,0},{0,0},{0,0},{-1,-1},{0,0},{0,0},{0,0},{-1,-1},{0,0},{0,0},{0,0},{1,1},{0,0},{0,0},{0,0},{1,1},{0,0},{0,0},{0,0},{1,1},{0,0},{0,0},{0,0},{1,1},{0,0},{0,0}};
const int kPilotK[4] = {-21, -7, 7, 21};
const int kPilotBase[4] = {1, 1, 1, -1};

inline int bin(int k) { return (k + kFftN) % kFftN; }
inline bool isPilot(int k) { return k == -21 || k == -7 || k == 7 || k == 21; }

// HT-LTF(-28 .. 28), Equation 19-23: the L-LTF sequence with two more carriers on each side
int htLtf(int k) {
    if (k == -28 || k == -27) return 1;
    if (k == 27 || k == 28) return -1;
    return kLtf[k + 26];
}

const std::vector<int>& legacyData() {
    static const std::vector<int> v = [] { std::vector<int> r; for (int k = -26; k <= 26; k++) if (k && !isPilot(k)) r.push_back(k); return r; }();
    return v;
}
const std::vector<int>& htData() {
    static const std::vector<int> v = [] { std::vector<int> r; for (int k = -28; k <= 28; k++) if (k && !isPilot(k)) r.push_back(k); return r; }();
    return v;
}

// Interleaver of clause 19.3.11.8 for one stream at 20 MHz (NCOL 13, NROW 4 NBPSC): the output position of input bit k
int interleave(int k, int bpsc) {
    const int ncbps = 52 * bpsc, nrow = 4 * bpsc, s = std::max(1, bpsc / 2);
    const int i = nrow * (k % 13) + k / 13;
    return s * (i / s) + (i + ncbps - (13 * i) / ncbps) % s;
}

// Puncturing patterns of clause 17.3.5.6, over the A / B output pairs
const std::vector<uint8_t>& punctPattern(int p) {
    static const std::vector<uint8_t> t[4] = {{1, 1}, {1, 1, 1, 0, 0, 1}, {1, 1, 1, 0}, {1, 1, 1, 0, 0, 1, 1, 0, 0, 1}};
    return t[p];
}

// Per-axis constellation levels indexed by the bits of one axis (Tables 17-14 .. 17-17), b0 the first transmitted bit, as an integer b0 b1 ...
int axisLevel(int bits, int n) {
    if (n == 1) return bits ? 1 : -1;
    if (n == 2) { static const int t[4] = {-3, -1, 3, 1}; return t[bits]; }
    static const int t[8] = {-7, -5, -1, -3, 7, 5, 1, 3};
    return t[bits];
}
float constK(int bpsc) { return bpsc == 1 ? 1.f : bpsc == 2 ? 1.f / std::sqrt(2.f) : bpsc == 4 ? 1.f / std::sqrt(10.f) : 1.f / std::sqrt(42.f); }

cf32 mapBits(const uint8_t* b, int bpsc) {
    if (bpsc == 1) return cf32(b[0] ? 1.f : -1.f, 0);
    const int n = bpsc / 2;
    int i = 0, q = 0;
    for (int k = 0; k < n; k++) { i = i << 1 | b[k]; q = q << 1 | b[n + k]; }
    return cf32((float)axisLevel(i, n), (float)axisLevel(q, n)) * constK(bpsc);
}

// max-log soft bits of one axis into out[0 .. n - 1] (> 0: a one); y is in units of the constellation (before K)
void softAxis(float y, int n, float w, float* out) {
    const int m = 1 << n;
    for (int b = 0; b < n; b++) {
        float d0 = 1e30f, d1 = 1e30f;
        for (int v = 0; v < m; v++) {
            const float d = (y - (float)axisLevel(v, n)) * (y - (float)axisLevel(v, n));
            if (v >> (n - 1 - b) & 1) d1 = std::min(d1, d); else d0 = std::min(d0, d);
        }
        out[b] = (d0 - d1) * w;
    }
}

std::vector<cf32> ifftScaled(std::vector<cf32> X, int used) {
    static Fft fft(kFftN);
    fft.inverse(X.data());
    const float s = 1.0f / std::sqrt((float)used);
    for (cf32& v : X) v *= s;
    return X;
}

void putCp(std::vector<cf32>& out, const std::vector<cf32>& t, int cp) {
    out.insert(out.end(), t.begin() + (kFftN - cp), t.end());
    out.insert(out.end(), t.begin(), t.end());
}

void putLegacyBpsk(std::vector<cf32>& out, const uint8_t* coded48, int pol, bool quad) {
    std::vector<cf32> X(kFftN, cf32(0, 0));
    const auto& dc = legacyData();
    for (int k = 0; k < 48; k++) {
        const int j = 3 * (k % 16) + k / 16;
        const float v = coded48[k] ? 1.f : -1.f;
        X[(size_t)bin(dc[(size_t)j])] = quad ? cf32(0, v) : cf32(v, 0);
    }
    for (int i = 0; i < 4; i++) X[(size_t)bin(kPilotK[i])] = cf32((float)(kPilotBase[i] * pol), 0);
    putCp(out, ifftScaled(X, 52), kCpN);
}
} // namespace

Mcs mcsParams(int mcs) {
    static const Mcs t[8] = {{1, 26, 0}, {2, 52, 0}, {2, 78, 1}, {4, 104, 0}, {4, 156, 1}, {6, 208, 2}, {6, 234, 1}, {6, 260, 3}};
    return t[mcs & 7];
}

uint8_t htSigCrc(const uint8_t* bits) {
    unsigned c = 0xFF;
    for (int i = 0; i < 34; i++) {
        const unsigned fb = (c >> 7 & 1) ^ (bits[i] & 1u);
        c = (c << 1) & 0xFF;
        if (fb) c ^= 0x07;
    }
    return (uint8_t)(~c & 0xFF);
}

std::vector<cf32> htPpdu(const std::vector<uint8_t>& mpdu, int mcs, bool sgi, uint8_t seed) {
    const Mcs M = mcsParams(mcs);
    const int len = (int)mpdu.size();
    const int nsym = (8 * len + 16 + 6 + M.dbps - 1) / M.dbps;
    std::vector<cf32> out;
    // L-STF, L-LTF
    std::vector<cf32> S(kFftN, cf32(0, 0));
    const float sc = std::sqrt(13.0f / 6.0f);
    for (int k = -26; k <= 26; k++) S[(size_t)bin(k)] = cf32(kStf[k + 26][0] * sc, kStf[k + 26][1] * sc);
    const std::vector<cf32> st = ifftScaled(S, 52);
    for (int i = 0; i < 160; i++) out.push_back(st[(size_t)(i % kFftN)]);
    const std::vector<cf32>& lt = ltfSymbol();
    for (int i = 32; i < kFftN; i++) out.push_back(lt[(size_t)i]);
    for (int r = 0; r < 2; r++) out.insert(out.end(), lt.begin(), lt.end());
    // L-SIG: 6 Mbit/s, length from the time the HT part takes (Equation 19-5)
    const int txUs10 = 200 + 80 + 40 + 40 + nsym * (sgi ? 36 : 40);          // tenths of a microsecond
    const int lLen = ((txUs10 - 200 + 39) / 40) * 3 - 3;
    std::vector<uint8_t> sig = {1, 1, 0, 1, 0};
    for (int i = 0; i < 12; i++) sig.push_back((uint8_t)(lLen >> i & 1));
    uint8_t par = 0;
    for (uint8_t b : sig) par ^= b;
    sig.push_back(par);
    for (int i = 0; i < 6; i++) sig.push_back(0);
    putLegacyBpsk(out, convEncode(sig).data(), pilotPolarity(0), false);
    // HT-SIG (Figure 19-5): MCS, CBW 20, length, smoothing 1, not sounding 1, reserved 1, no aggregation, STBC 0, BCC, GI, Ness 0, CRC, tail
    std::vector<uint8_t> h;
    for (int i = 0; i < 7; i++) h.push_back((uint8_t)(mcs >> i & 1));
    h.push_back(0);
    for (int i = 0; i < 16; i++) h.push_back((uint8_t)(len >> i & 1));
    h.push_back(1); h.push_back(1); h.push_back(1); h.push_back(0);
    h.push_back(0); h.push_back(0); h.push_back(0); h.push_back(sgi ? 1 : 0);
    h.push_back(0); h.push_back(0);
    const uint8_t crc = htSigCrc(h.data());
    for (int i = 7; i >= 0; i--) h.push_back((uint8_t)(crc >> i & 1));
    for (int i = 0; i < 6; i++) h.push_back(0);
    const std::vector<uint8_t> hc = convEncode(h);
    putLegacyBpsk(out, hc.data(), pilotPolarity(1), true);
    putLegacyBpsk(out, hc.data() + 48, pilotPolarity(2), true);
    // HT-STF
    std::vector<cf32> S2(kFftN, cf32(0, 0));
    for (int k = -26; k <= 26; k++) S2[(size_t)bin(k)] = cf32(kStf[k + 26][0] * sc, kStf[k + 26][1] * sc);
    putCp(out, ifftScaled(S2, 56), kCpN);
    // HT-LTF
    std::vector<cf32> L(kFftN, cf32(0, 0));
    for (int k = -28; k <= 28; k++) if (k) L[(size_t)bin(k)] = cf32((float)htLtf(k), 0);
    putCp(out, ifftScaled(L, 56), kCpN);
    // DATA: SERVICE, PSDU, tail, pad; scramble; force the tail to zero; encode; puncture
    std::vector<uint8_t> d(16, 0);
    for (uint8_t b : mpdu) for (int k = 0; k < 8; k++) d.push_back((uint8_t)(b >> k & 1));
    const size_t tailAt = d.size();
    d.resize((size_t)nsym * (size_t)M.dbps, 0);
    scramble(d, seed ? seed : 0x5D);
    for (size_t i = 0; i < 6; i++) d[tailAt + i] = 0;
    const std::vector<uint8_t> enc = convEncode(d);
    const auto& pat = punctPattern(M.punct);
    std::vector<uint8_t> coded;
    for (size_t i = 0; i < enc.size(); i++) if (pat[i % pat.size()]) coded.push_back(enc[i]);
    const int ncbps = 52 * M.bpsc;
    const auto& dk = htData();
    std::vector<uint8_t> il((size_t)ncbps);
    for (int s = 0; s < nsym; s++) {
        for (int k = 0; k < ncbps; k++) il[(size_t)interleave(k, M.bpsc)] = coded[(size_t)s * ncbps + (size_t)k];
        std::vector<cf32> X(kFftN, cf32(0, 0));
        for (int c = 0; c < 52; c++) X[(size_t)bin(dk[(size_t)c])] = mapBits(il.data() + c * M.bpsc, M.bpsc);
        const int pol = pilotPolarity(s + 3);
        for (int i = 0; i < 4; i++) X[(size_t)bin(kPilotK[i])] = cf32((float)(kPilotBase[i] * pol), 0);
        putCp(out, ifftScaled(X, 56), sgi ? 8 : kCpN);
    }
    return out;
}

struct HtRx::Impl {
    FrameCb cb;
    Stats st;
    std::vector<cf32> b;
    int64_t base = 0, n = 0;
    std::complex<double> C{0, 0};
    double P = 0;
    int run = 0;
    double bestM = 0, floorP = 1e30;
    std::complex<double> bestC{0, 0};
    int64_t trig = -1, skipUntil = 0;
    double cfoCoarse = 0;
    Fft fft{kFftN};

    bool pending = false;
    int64_t pd = 0, pend = 0;
    double pcfo = 0;
    int pmcs = 0, plen = 0, pnsym = 0;
    bool psgi = false;
    std::vector<cf32> H;       // L-LTF channel, k + 26
    std::vector<cf32> Ht;      // HT-LTF channel, k + 28
    float havg = 1, htavg = 1;

    const cf32& at(int64_t i) const { return b[(size_t)(i - base)]; }

    void reset() { b.clear(); base = n = 0; C = 0; P = 0; run = 0; bestM = 0; trig = -1; skipUntil = 0; pending = false; floorP = 1e30; }

    cf32 rot(int64_t i, double f, int64_t ref) const {
        const double a = -2 * kPi * f * (double)(i - ref) / kRate;
        const std::complex<double> v = std::complex<double>(at(i).real(), at(i).imag()) * std::polar(1.0, a);
        return cf32((float)v.real(), (float)v.imag());
    }

    void carriers(int64_t s, double f, int64_t ref, cf32* out, int edge) {
        cf32 t[kFftN];
        for (int i = 0; i < kFftN; i++) t[i] = rot(s + i, f, ref);
        fft.forward(t);
        for (int k = -edge; k <= edge; k++) out[k + edge] = t[bin(k)];
    }

    void feedOne(cf32 x) {
        b.push_back(x);
        const int64_t m = n++;
        P += (double)std::norm(x);
        if (m >= kLag) C += std::complex<double>(at(m - kLag)) * std::conj(std::complex<double>(x));
        if (m >= kW) P -= (double)std::norm(at(m - kW));
        if (m >= kW + kLag) { const cf32 u = at(m - kW - kLag), v = at(m - kW); C -= std::complex<double>(u) * std::conj(std::complex<double>(v)); }
        if (m < kW + kLag + 2 || m < skipUntil || pending || trig >= 0) { if (trig < 0) run = 0; return; }
        const double met = P > 1e-12 ? std::norm(C) / (P * P) : 0;
        floorP = std::min(floorP * 1.0001, P);      // the quietest recent window: a short training must stand out of it
        if (met > kThresh && P > 2.5 * floorP) {
            if (met > bestM) { bestM = met; bestC = C; }
            if (++run >= kMinRun) { trig = m; st.triggers++; cfoCoarse = -std::arg(bestC) * kRate / (2 * kPi * kLag); }
        } else { run = 0; bestM = 0; }
    }

    // 48 equalised symbols of a legacy-format symbol; quad: soft values from the Q axis
    void legacySym(int64_t s, int polIndex, bool quad, float* soft) {
        cf32 Y[53];
        carriers(s + kCpN, pcfo, pd, Y, 26);
        const int pol = pilotPolarity(polIndex);
        std::complex<double> acc(0, 0);
        for (int i = 0; i < 4; i++) acc += std::complex<double>(Y[kPilotK[i] + 26] / H[(size_t)(kPilotK[i] + 26)]) * (double)(kPilotBase[i] * pol);
        const double a = std::arg(acc);
        const auto& dk = legacyData();
        for (int j = 0; j < 48; j++) {
            const int k = dk[(size_t)j];
            const std::complex<double> e = std::complex<double>(Y[k + 26] / H[(size_t)(k + 26)]) * std::polar(1.0, -a);
            const float w = std::norm(H[(size_t)(k + 26)]) / havg;
            soft[j] = (float)(quad ? e.imag() : e.real()) * w;
        }
    }

    bool sync() {
        const std::vector<cf32>& L = ltfSymbol();
        const int64_t r0 = std::max(base, trig - 260);
        const int64_t rEnd = std::min(n - (kFftN * 2 + 4), r0 + kSearch);
        if (rEnd <= r0) { st.noSync++; return false; }
        const size_t cnt = (size_t)(rEnd - r0) + 2 * kFftN + 4;
        std::vector<cf32> y(cnt);
        for (size_t i = 0; i < cnt && r0 + (int64_t)i < n; i++) y[i] = rot(r0 + (int64_t)i, cfoCoarse, r0);
        std::vector<float> mm((size_t)(rEnd - r0));
        double mean = 0;
        for (size_t d = 0; d < mm.size(); d++) {
            std::complex<float> a(0, 0);
            for (int i = 0; i < kFftN; i++) a += y[d + (size_t)i] * std::conj(L[(size_t)i]);
            mm[d] = std::abs(a);
            mean += mm[d];
        }
        mean /= (double)mm.size();
        size_t bd = 0; float bv = 0;
        for (size_t d = 0; d + kFftN < mm.size(); d++) { const float v = mm[d] + mm[d + kFftN]; if (v > bv) { bv = v; bd = d; } }
        if (bv < 6 * mean) { st.noSync++; return false; }
        std::complex<double> cc(0, 0);
        for (int i = 0; i < kFftN; i++) cc += std::complex<double>(y[bd + (size_t)i]) * std::conj(std::complex<double>(y[bd + (size_t)kFftN + (size_t)i]));
        const double f = cfoCoarse - std::arg(cc) * kRate / (2 * kPi * kFftN);
        const int64_t d0 = r0 + (int64_t)bd;
        if (d0 < base || d0 + kOffData + 4 > n) { st.noSync++; return false; }
        pcfo = f; pd = d0;
        cf32 A[53], B[53];
        carriers(d0, f, d0, A, 26);
        carriers(d0 + kFftN, f, d0, B, 26);
        H.assign(53, cf32(0, 0));
        double hp = 0;
        for (int k = -26; k <= 26; k++) {
            if (!k) continue;
            H[(size_t)(k + 26)] = (A[k + 26] + B[k + 26]) * 0.5f * (float)kLtf[k + 26];
            hp += std::norm(H[(size_t)(k + 26)]);
        }
        havg = (float)(hp / 52);
        if (havg < 1e-12f) { st.noSync++; return false; }
        // L-SIG: must say 6 Mbit/s with even parity
        float ls[48], de[48];
        legacySym(d0 + kOffLsig, 0, false, ls);
        for (int k = 0; k < 48; k++) de[k] = ls[3 * (k % 16) + k / 16];
        uint8_t lb[24];
        viterbiDecode(de, 24, lb);
        uint8_t par = 0;
        for (int i = 0; i < 18; i++) par ^= lb[i];
        const int rate = lb[0] | lb[1] << 1 | lb[2] << 2 | lb[3] << 3;
        if (par || rate != kRate6) { st.nonHt++; skipUntil = d0 + kOffLsig + 80; return false; }
        // HT-SIG: the two symbols must sit on the Q axis
        float s1[48], s2[48];
        legacySym(d0 + kOffHtSig, 1, true, s1);
        legacySym(d0 + kOffHtSig + 80, 2, true, s2);
        float s1i[48];
        legacySym(d0 + kOffHtSig, 1, false, s1i);
        double qe = 0, ie = 0;
        for (int i = 0; i < 48; i++) { qe += std::fabs(s1[i]); ie += std::fabs(s1i[i]); }
        if (qe < 1.3 * ie) { st.nonHt++; return false; }
        std::vector<float> c(96);
        for (int k = 0; k < 48; k++) { c[(size_t)k] = s1[3 * (k % 16) + k / 16]; c[(size_t)(48 + k)] = s2[3 * (k % 16) + k / 16]; }
        uint8_t h[48];
        viterbiDecode(c.data(), 48, h);
        uint8_t crc = 0;
        for (int i = 0; i < 8; i++) crc = (uint8_t)(crc << 1 | h[34 + i]);
        int tail = 0;
        for (int i = 42; i < 48; i++) tail |= h[i];
        if (tail || crc != htSigCrc(h)) { st.sigBad++; return false; }
        int mcs = 0, len = 0;
        for (int i = 0; i < 7; i++) mcs |= h[i] << i;
        for (int i = 0; i < 16; i++) len |= h[8 + i] << i;
        const bool cbw40 = h[7], agg = h[27], ldpc = h[30], sgi = h[31];
        const int stbc = h[28] | h[29] << 1, ness = h[32] | h[33] << 1;
        if (mcs > 7 || cbw40 || agg || ldpc || stbc || ness) { st.unsupported++; return false; }
        if (len < 14) { st.sigBad++; return false; }
        const Mcs M = mcsParams(mcs);
        pmcs = mcs; psgi = sgi; plen = len;
        pnsym = (8 * len + 16 + 6 + M.dbps - 1) / M.dbps;
        pend = d0 + kOffData + (int64_t)pnsym * (sgi ? 72 : 80);
        return true;
    }

    void decodeData() {
        // channel from the HT-LTF
        cf32 Y[57];
        carriers(pd + kOffHtLtf + kCpN, pcfo, pd, Y, 28);
        Ht.assign(57, cf32(0, 0));
        double hp = 0;
        for (int k = -28; k <= 28; k++) { if (!k) continue; Ht[(size_t)(k + 28)] = Y[k + 28] * (float)htLtf(k); hp += std::norm(Ht[(size_t)(k + 28)]); }
        htavg = (float)(hp / 56);
        if (htavg < 1e-12f) { st.framesBad++; return; }
        const Mcs M = mcsParams(pmcs);
        const int cps = psgi ? 8 : kCpN, sym = kFftN + cps, ncbps = 52 * M.bpsc;
        const auto& dk = htData();
        const auto& pat = punctPattern(M.punct);
        // positions of the kept bits in the rate 1/2 stream
        const int total = pnsym * ncbps;
        std::vector<float> coded((size_t)total);
        std::vector<float> tmp((size_t)ncbps);
        double err = 0; int nErr = 0;
        for (int s = 0; s < pnsym; s++) {
            cf32 Z[57];
            carriers(pd + kOffData + (int64_t)s * sym + cps, pcfo, pd, Z, 28);
            const int pol = pilotPolarity(s + 3);
            std::complex<double> acc(0, 0);
            cf32 E[57];
            for (int k = -28; k <= 28; k++) E[k + 28] = k ? Z[k + 28] / Ht[(size_t)(k + 28)] : cf32(0, 0);
            for (int i = 0; i < 4; i++) acc += std::complex<double>(E[kPilotK[i] + 28]) * (double)(kPilotBase[i] * pol);
            const double a = std::arg(acc);
            double num = 0, den = 0;
            for (int i = 0; i < 4; i++) {
                const std::complex<double> v = std::complex<double>(E[kPilotK[i] + 28]) * (double)(kPilotBase[i] * pol) * std::polar(1.0, -a);
                num += kPilotK[i] * std::arg(v); den += (double)kPilotK[i] * kPilotK[i];
            }
            const double slope = den > 0 ? num / den : 0;
            const float K = constK(M.bpsc);
            for (int c = 0; c < 52; c++) {
                const int k = dk[(size_t)c];
                std::complex<double> e = std::complex<double>(E[k + 28]) * std::polar(1.0, -(a + slope * k));
                const float w = std::norm(Ht[(size_t)(k + 28)]) / htavg;
                if (M.bpsc == 1) { tmp[(size_t)c] = (float)e.real() * w; const double dr = std::fabs(e.real()) - 1; err += dr * dr + e.imag() * e.imag(); nErr++; }
                else {
                    softAxis((float)e.real() / K, M.bpsc / 2, w, tmp.data() + c * M.bpsc);
                    softAxis((float)e.imag() / K, M.bpsc / 2, w, tmp.data() + c * M.bpsc + M.bpsc / 2);
                    const double ri = std::round((e.real() / K + (M.bpsc == 2 ? 1 : M.bpsc == 4 ? 3 : 7)) / 2) * 2 - (M.bpsc == 2 ? 1 : M.bpsc == 4 ? 3 : 7);
                    const double rq = std::round((e.imag() / K + (M.bpsc == 2 ? 1 : M.bpsc == 4 ? 3 : 7)) / 2) * 2 - (M.bpsc == 2 ? 1 : M.bpsc == 4 ? 3 : 7);
                    err += ((e.real() / K - ri) * (e.real() / K - ri) + (e.imag() / K - rq) * (e.imag() / K - rq)) * K * K; nErr++;
                }
            }
            for (int k = 0; k < ncbps; k++) coded[(size_t)s * ncbps + (size_t)k] = tmp[(size_t)interleave(k, M.bpsc)];
        }
        // depuncture into the rate 1/2 stream
        const int nb = 16 + 8 * plen + 6;
        std::vector<float> full((size_t)2 * nb, 0.f);
        size_t out = 0;
        for (int i = 0; i < 2 * nb && out < coded.size(); i++) if (pat[(size_t)i % pat.size()]) full[(size_t)i] = coded[out++];
        std::vector<uint8_t> bits((size_t)nb);
        viterbiDecode(full.data(), nb, bits.data());
        unsigned stt = 0;
        for (int i = 0; i < 7; i++) stt |= (unsigned)bits[(size_t)i] << (6 - i);
        std::vector<uint8_t> rest(bits.begin() + 7, bits.end());
        scramble(rest, (uint8_t)stt);
        std::vector<uint8_t> f((size_t)plen, 0);
        for (int i = 0; i < plen; i++) for (int k = 0; k < 8; k++) f[(size_t)i] |= (uint8_t)(rest[(size_t)(9 + 8 * i + k)] << k);
        const size_t nn = f.size() - 4;
        const uint32_t got = (uint32_t)f[nn] | (uint32_t)f[nn + 1] << 8 | (uint32_t)f[nn + 2] << 16 | (uint32_t)f[nn + 3] << 24;
        if (wifi::crc32(f.data(), nn) != got) { st.framesBad++; return; }
        st.framesOk++;
        HtInfo in;
        in.mcs = pmcs; in.shortGi = psgi; in.length = plen; in.cfoHz = pcfo; in.at = pd;
        in.snrDb = nErr ? (float)(-10 * std::log10(err / nErr + 1e-9)) : 0;
        double pw = 0;
        const int64_t a = std::max(base, pd - 320), z = std::min(n, pend);
        for (int64_t i = a; i < z; i++) pw += std::norm(at(i));
        in.rssiDb = (float)(10 * std::log10(pw / (double)std::max<int64_t>(1, z - a) + 1e-20));
        if (cb) cb(f.data(), nn, in);
    }

    void step() {
        for (;;) {
            if (pending) {
                if (n < pend + 4) return;
                decodeData();
                pending = false; skipUntil = pend; trig = -1; run = 0; bestM = 0;
                continue;
            }
            if (trig >= 0 && n >= trig + kSyncWait) {
                if (sync()) pending = true;
                trig = -1; run = 0; bestM = 0;
                if (pending) continue;
            }
            return;
        }
    }

    void trim() {
        int64_t keep = n - kKeep;
        if (trig >= 0) keep = std::min(keep, trig - 300);
        if (pending) keep = std::min(keep, pd - 400);
        if (keep - base > (1 << 17)) { b.erase(b.begin(), b.begin() + (ptrdiff_t)(keep - base)); base = keep; }
    }
};

HtRx::HtRx() : p_(std::make_unique<Impl>()) {}
HtRx::~HtRx() = default;
void HtRx::setCallback(FrameCb cb) { p_->cb = std::move(cb); }
void HtRx::reset() { p_->reset(); }
const HtRx::Stats& HtRx::stats() const { return p_->st; }
void HtRx::feed(const cf32* x, size_t cnt) {
    Impl& m = *p_;
    for (size_t i = 0; i < cnt; i++) {
        const float re = x[i].real(), im = x[i].imag();
        m.feedOne(std::isfinite(re) && std::isfinite(im) ? x[i] : cf32(0, 0));
        if ((m.trig >= 0 && m.n >= m.trig + kSyncWait) || (m.pending && m.n >= m.pend + 4)) m.step();
    }
    m.trim();
}

} // namespace dect2::xlink::ht
