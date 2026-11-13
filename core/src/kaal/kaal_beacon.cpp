// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// KAAL beacon: frame bits (port of kaal_beacon_v2.py), BPSK slice modulator and streaming receiver (port of kaal_beacon_slice.py).
#include "dect2/kaal/kaal_beacon.h"
#include "dect2/common/diag.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <deque>

namespace dect2 {
namespace kaal {

namespace {

constexpr double kPi = 3.14159265358979323846;
const char* const kAlphabet = " 0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ/-";   // 40 characters
constexpr int kGridBits = 25;
constexpr int kHdrFieldBits = 4 + 4 + 48 + 24 + 25 + 8 + 8 + kGridBits + 8;   // 154
constexpr int kCodeG[4] = {0171, 0133, 0165, 0117};
constexpr int kK = 7;
constexpr int kSyncLen = 64;
constexpr double kSliceBeta = 0.35;
constexpr int kSliceSpan = 8;
const uint64_t kSyncHdr = 0xdec9517668db6b8bULL;    // numpy RandomState(11).randint(0, 2, 64), MSB first
const uint64_t kSyncFull = 0x936dae59fb6c4644ULL;   // numpy RandomState(23).randint(0, 2, 64)

uint16_t crc16(const uint8_t* bits, size_t n) {
    uint32_t crc = 0xFFFF;
    for (size_t i = 0; i < n; i++) {
        const uint32_t top = (crc >> 15) & 1;
        crc = (crc << 1) & 0xFFFF;
        if (top ^ bits[i]) crc ^= 0x1021;
    }
    return (uint16_t)crc;
}

uint32_t crc32(const uint8_t* d, size_t n) {   // zlib / IEEE 802.3
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        c ^= d[i];
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return ~c;
}

void put(std::vector<uint8_t>& out, uint64_t v, int n) {
    for (int i = 0; i < n; i++) out.push_back((uint8_t)((v >> (n - 1 - i)) & 1));
}
uint64_t get(const std::vector<uint8_t>& b, size_t& pos, int n) {
    uint64_t v = 0;
    for (int i = 0; i < n; i++) v = (v << 1) | (b[pos + i] & 1);
    pos += n;
    return v;
}
int64_t sgn(uint64_t v, int n) { return v >= (1ULL << (n - 1)) ? (int64_t)v - (int64_t)(1ULL << n) : (int64_t)v; }

bool encCall(const std::string& in, uint64_t& v) {
    if (in.size() > 9) return false;
    std::string cs = in;
    for (char& c : cs) c = (char)std::toupper((unsigned char)c);
    cs.resize(9, ' ');
    v = 0;
    for (char c : cs) {
        const char* p = c ? std::strchr(kAlphabet, c) : nullptr;
        if (!p) return false;
        v = v * 40 + (uint64_t)(p - kAlphabet);
    }
    return true;
}
std::string decCall(uint64_t v) {
    char s[10];
    for (int i = 8; i >= 0; i--) { s[i] = kAlphabet[v % 40]; v /= 40; }
    s[9] = 0;
    std::string r(s);
    while (!r.empty() && r.back() == ' ') r.pop_back();
    return r;
}
bool encGrid(const std::string& g, uint64_t& v) {
    if (g.size() != 6) return false;
    const int f1 = std::toupper((unsigned char)g[0]) - 'A', f2 = std::toupper((unsigned char)g[1]) - 'A';
    if (g[2] < '0' || g[2] > '9' || g[3] < '0' || g[3] > '9') return false;
    const int d1 = g[2] - '0', d2 = g[3] - '0';
    const int s1 = std::tolower((unsigned char)g[4]) - 'a', s2 = std::tolower((unsigned char)g[5]) - 'a';
    if (f1 < 0 || f1 >= 18 || f2 < 0 || f2 >= 18 || s1 < 0 || s1 >= 24 || s2 < 0 || s2 >= 24) return false;
    v = ((((((uint64_t)f1 * 18 + f2) * 10 + d1) * 10 + d2) * 24 + s1) * 24 + s2);
    return true;
}
bool decGrid(uint64_t v, std::string& g) {
    const int s2 = (int)(v % 24); v /= 24;
    const int s1 = (int)(v % 24); v /= 24;
    const int d2 = (int)(v % 10); v /= 10;
    const int d1 = (int)(v % 10); v /= 10;
    const int f2 = (int)(v % 18), f1 = (int)(v / 18);
    if (f1 >= 18) return false;
    g.clear();
    g += (char)('A' + f1); g += (char)('A' + f2); g += (char)('0' + d1); g += (char)('0' + d2);
    g += (char)('a' + s1); g += (char)('a' + s2);
    return true;
}

inline int parity7(int x) { return __builtin_popcount((unsigned)x) & 1; }

std::vector<double> sliceRrc(int sps) {
    const int L = kSliceSpan * sps + 1;
    std::vector<double> t(L);
    const double b = kSliceBeta;
    double e = 0;
    for (int i = 0; i < L; i++) {
        const double x = (double)(i - kSliceSpan * sps / 2) / sps;
        double v;
        if (std::fabs(x) < 1e-9) v = 1 - b + 4 * b / kPi;
        else if (std::fabs(std::fabs(x) - 1 / (4 * b)) < 1e-9)
            v = b / std::sqrt(2.0) * ((1 + 2 / kPi) * std::sin(kPi / (4 * b)) + (1 - 2 / kPi) * std::cos(kPi / (4 * b)));
        else v = (std::sin(kPi * x * (1 - b)) + 4 * b * x * std::cos(kPi * x * (1 + b))) / (kPi * x * (1 - (4 * b * x) * (4 * b * x)));
        t[i] = v;
        e += v * v;
    }
    const double n = std::sqrt(e);
    for (double& v : t) v /= n;
    return t;
}

// trellis tables: for next state ns and the two previous states ps = ((ns << 1) & 63) | b, the four code signs (+1 = bit 0)
struct Trellis {
    float sign[2][64][4];
    Trellis() {
        for (int ns = 0; ns < 64; ns++)
            for (int b = 0; b < 2; b++) {
                const int ps = ((ns << 1) & 63) | b, reg = ((ns >> 5) << 6) | ps;
                for (int g = 0; g < 4; g++) sign[b][ns][g] = 1.0f - 2.0f * (float)parity7(reg & kCodeG[g]);
            }
    }
};
const Trellis& trellis() { static const Trellis t; return t; }

}  // namespace

// ------------------------------------------------------------------------------------------------ frame bits

std::vector<uint8_t> encodeBeaconBits(const BeaconFields& f, const std::string& textIn) {
    std::string text = textIn.substr(0, kBeaconMaxText);
    uint64_t call, grid;
    if (!encCall(f.callsign, call) || !encGrid(f.grid, grid)) return {};
    const int ft = (!text.empty() && f.frameType == 0) ? 1 : f.frameType;   // frame type 0 carries no text
    std::vector<uint8_t> b;
    b.reserve(kBeaconHeaderBits + 8 * text.size() + 32);
    put(b, (uint64_t)(f.version & 15), 4);
    put(b, (uint64_t)(ft & 15), 4);
    put(b, call, 48);
    put(b, (uint64_t)((int64_t)std::nearbyint(f.lat * (double)(1 << 23) / 90.0)) & 0xFFFFFF, 24);
    put(b, (uint64_t)((int64_t)std::nearbyint(f.lon * (double)(1 << 24) / 180.0)) & 0x1FFFFFF, 25);
    const long pv = std::lround(std::nearbyint((f.powerDbm + 40.0) * 2.0));
    put(b, (uint64_t)std::min(255L, std::max(0L, pv)), 8);
    put(b, (uint64_t)(f.linkQuality & 255), 8);
    put(b, grid, kGridBits);
    put(b, (uint64_t)text.size(), 8);
    put(b, crc16(b.data(), b.size()), 16);
    if (!text.empty()) {
        for (unsigned char ch : text) put(b, ch, 8);
        put(b, crc32((const uint8_t*)text.data(), text.size()), 32);
    }
    return b;
}

bool decodeBeaconBits(const std::vector<uint8_t>& bits, BeaconFields& f, std::string& text) {
    if (bits.size() < (size_t)kBeaconHeaderBits) return false;
    size_t p = kHdrFieldBits;
    if (crc16(bits.data(), kHdrFieldBits) != get(bits, p, 16)) return false;
    p = 0;
    BeaconFields o;
    o.version = (int)get(bits, p, 4);
    o.frameType = (int)get(bits, p, 4);
    o.callsign = decCall(get(bits, p, 48));
    o.lat = (double)sgn(get(bits, p, 24), 24) * 90.0 / (double)(1 << 23);
    o.lon = (double)sgn(get(bits, p, 25), 25) * 180.0 / (double)(1 << 24);
    o.powerDbm = -40.0 + 0.5 * (double)get(bits, p, 8);
    o.linkQuality = (int)get(bits, p, 8);
    if (!decGrid(get(bits, p, kGridBits), o.grid)) return false;
    const size_t n = (size_t)get(bits, p, 8);
    p += 16;
    std::string t;
    if (n) {
        if (n > kBeaconMaxText || bits.size() < p + 8 * n + 32) return false;
        std::vector<uint8_t> d(n);
        for (size_t i = 0; i < n; i++) { size_t q = p + 8 * i; d[i] = (uint8_t)get(bits, q, 8); }
        size_t q = p + 8 * n;
        if (crc32(d.data(), n) != (uint32_t)get(bits, q, 32)) return false;
        t.assign((const char*)d.data(), n);
    }
    f = o;
    text = t;
    return true;
}

// ------------------------------------------------------------------------------------------------ code

std::vector<uint8_t> beaconConvEncode(const std::vector<uint8_t>& in) {
    std::vector<uint8_t> out;
    out.reserve(4 * (in.size() + kK - 1));
    int st = 0;
    for (size_t i = 0; i < in.size() + kK - 1; i++) {
        const int u = i < in.size() ? (in[i] & 1) : 0;
        const int reg = (u << 6) | st;
        for (int g = 0; g < 4; g++) out.push_back((uint8_t)parity7(reg & kCodeG[g]));
        st = reg >> 1;
    }
    return out;
}

std::vector<uint8_t> beaconViterbi(const float* soft, size_t steps, size_t nInfo) {
    const Trellis& T = trellis();
    float pm[64], npm[64];
    for (int s = 0; s < 64; s++) pm[s] = -1e30f;
    pm[0] = 0;
    std::vector<uint8_t> tb(steps * 64);
    for (size_t t = 0; t < steps; t++) {
        const float* s = soft + 4 * t;
        for (int ns = 0; ns < 64; ns++) {
            const float* a = T.sign[0][ns];
            const float* b = T.sign[1][ns];
            const float m0 = pm[((ns << 1) & 63)] + a[0] * s[0] + a[1] * s[1] + a[2] * s[2] + a[3] * s[3];
            const float m1 = pm[((ns << 1) & 63) | 1] + b[0] * s[0] + b[1] * s[1] + b[2] * s[2] + b[3] * s[3];
            const bool sel = m1 > m0;
            tb[t * 64 + ns] = sel;
            npm[ns] = sel ? m1 : m0;
        }
        std::memcpy(pm, npm, sizeof pm);
    }
    int ns = 0;
    if (steps < nInfo + kK - 1) {
        float best = -1e37f;
        for (int s = 0; s < 64; s++) if (pm[s] > best) { best = pm[s]; ns = s; }
    }
    std::vector<uint8_t> bits(steps);
    for (size_t t = steps; t-- > 0;) {
        bits[t] = (uint8_t)(ns >> 5);
        ns = ((ns << 1) & 63) | tb[t * 64 + ns];
    }
    bits.resize(std::min(nInfo, steps));
    return bits;
}

// ------------------------------------------------------------------------------------------------ modulator

namespace {
std::vector<double> syncWord(bool full) {
    const uint64_t w = full ? kSyncFull : kSyncHdr;
    std::vector<double> s(kSyncLen);
    for (int i = 0; i < kSyncLen; i++) s[i] = ((w >> (63 - i)) & 1) ? 1.0 : -1.0;
    return s;
}
}  // namespace

std::vector<std::complex<float>> beaconSliceModulate(const BeaconFields& fIn, const std::string& text, double sampleRate) {
    BeaconFields f = fIn;
    f.frameType = text.empty() ? 0 : (f.frameType == 0 ? 1 : f.frameType);
    const std::vector<uint8_t> info = encodeBeaconBits(f, text);
    if (info.empty()) return {};
    const std::vector<uint8_t> coded = beaconConvEncode(info);
    std::vector<double> syms = syncWord(!text.empty());
    for (uint8_t c : coded) syms.push_back(1.0 - 2.0 * c);
    const int sps = std::max(1, (int)std::lround(sampleRate / kBeaconSymRate));
    const std::vector<double> h = sliceRrc(sps);
    const size_t n = syms.size() * sps + h.size() - 1;
    std::vector<double> bb(n, 0.0);
    for (size_t k = 0; k < syms.size(); k++)
        for (size_t i = 0; i < h.size(); i++) bb[k * sps + i] += syms[k] * h[i];
    double e = 0;
    for (double v : bb) e += v * v;
    const double sc = 1.0 / std::sqrt(e / (double)n);
    std::vector<std::complex<float>> out(n);
    for (size_t i = 0; i < n; i++) {
        const double a = 2 * kPi * kBeaconOffsetHz * (double)i / sampleRate;
        out[i] = std::complex<float>((float)(bb[i] * sc * std::cos(a)), (float)(bb[i] * sc * std::sin(a)));
    }
    return out;
}

// ------------------------------------------------------------------------------------------------ receiver

struct BeaconSliceRx::Impl {
    using cd = std::complex<double>;
    double fs = 48000;
    int sps = 48, D = 3, spsD = 16, L = 385, c = 192;
    std::vector<double> h;
    double phase = 0, dphi = 0;
    std::vector<std::complex<float>> xb;   // mixed-down samples from absolute index xbBase
    int64_t xbBase = 0, nIn = 0, nextM = 0;
    std::vector<std::complex<float>> mf;   // matched-filter output, every D-th input sample, from decimated index mfBase
    int64_t mfBase = 0;
    int64_t sinceAttempt = 1 << 30;
    int64_t gate = 4000;
    size_t mfCap = 0;
    struct Out { BeaconFields f; std::string text; };
    std::deque<Out> ready;

    void configure(double rate) {
        fs = rate;
        sps = std::max(1, (int)std::lround(rate / kBeaconSymRate));
        D = 1;
        for (int d = std::max(1, sps / 16); d >= 1; d--) if (sps % d == 0) { D = d; break; }
        spsD = sps / D;
        h = sliceRrc(sps);
        L = (int)h.size();
        c = (L - 1) / 2;
        phase = 0;
        dphi = 2 * kPi * kBeaconOffsetHz / rate;
        xb.clear(); mf.clear();
        xbBase = nIn = nextM = mfBase = 0;
        sinceAttempt = 1 << 30;
        gate = (int64_t)(0.25 * rate / D);
        mfCap = (size_t)(kSyncLen + 4 * (kBeaconHeaderBits + 8 * kBeaconMaxText + 32 + kK) + 400) * spsD;
        ready.clear();
    }

    void feed(const std::complex<float>* x, size_t n) {
        if (h.empty()) configure(48000);
        for (size_t i = 0; i < n; i++) {
            const float cr = (float)std::cos(phase), ci = (float)-std::sin(phase);
            xb.push_back(x[i] * std::complex<float>(cr, ci));
            phase += dphi;
            if (phase > 2 * kPi) phase -= 2 * kPi;
        }
        nIn += (int64_t)n;
        while (nextM * D + c < nIn) {
            const int64_t n0 = nextM * D - c;   // absolute index of tap 0
            double ar = 0, ai = 0;
            for (int i = 0; i < L; i++) {
                const int64_t a = n0 + i;
                if (a < 0) continue;
                const std::complex<float> v = xb[(size_t)(a - xbBase)];
                ar += (double)v.real() * h[i];
                ai += (double)v.imag() * h[i];
            }
            mf.emplace_back((float)ar, (float)ai);
            nextM++;
            sinceAttempt++;
        }
        const int64_t keep = nextM * D - c;
        if (keep - xbBase > 8192) {
            xb.erase(xb.begin(), xb.begin() + (keep - xbBase));
            xbBase = keep;
        }
        if (mf.size() > mfCap + 8192) dropMf(mf.size() - mfCap);
    }

    void dropMf(size_t k) {
        k = std::min(k, mf.size());
        mf.erase(mf.begin(), mf.begin() + k);
        mfBase += (int64_t)k;
    }

    enum Result { Wait, Found, Dropped };

    Result attempt() {
        const size_t N = mf.size();
        const size_t sync = kSyncLen;
        if (N < (sync + 4 * (kBeaconHeaderBits + kK - 1) + 8) * spsD) return Wait;
        std::vector<double> E(N + 1, 0.0);
        for (size_t i = 0; i < N; i++) E[i + 1] = E[i] + std::norm(std::complex<double>(mf[i]));
        const size_t last = N - sync * spsD;   // candidate starts 0 .. last
        double bestMetric = -1;
        int bestKind = 0;
        size_t bestM = 0;
        cd bestC;
        for (int kind = 0; kind < 2; kind++) {
            const std::vector<double> w = syncWord(kind == 1);
            double pk = -1;
            size_t pm0 = 0;
            cd pc;
            for (size_t m = 0; m <= last; m++) {
                double ar = 0, ai = 0;
                for (size_t i = 0; i < sync; i++) {
                    const std::complex<float>& v = mf[m + i * spsD];
                    ar += w[i] * v.real();
                    ai += w[i] * v.imag();
                }
                const double a2 = ar * ar + ai * ai;
                if (a2 > pk) { pk = a2; pm0 = m; pc = cd(ar, ai); }
            }
            const double e = std::sqrt(E[pm0 + sync * spsD] - E[pm0] + 1e-30);
            const double metric = std::sqrt(pk) / e;
            if (metric > bestMetric) { bestMetric = metric; bestKind = kind; bestM = pm0; bestC = pc; }
        }
        const std::vector<double> wsync = syncWord(bestKind == 1);
        const size_t avail = (N - 1 - bestM) / spsD + 1;   // symbols from the sync start
        const size_t nAvail = avail > sync ? avail - sync : 0;
        const double symRate = (double)kBeaconSymRate;
        // The residual carrier offset: the two halves of the sync word give it (to +-15 Hz); the frame is 0.7 s long, so a few hertz are several turns of phase.
        // The estimate and its neighbours are tried one after the other, each with its own static phase; the CRC says which one is right.
        cd c1(0, 0), c2(0, 0);
        for (size_t i = 0; i < sync; i++) {
            const cd v = cd(mf[bestM + i * spsD]) * wsync[i];
            (i < sync / 2 ? c1 : c2) += v;
        }
        const double dfEst = std::arg(c2 * std::conj(c1)) / (2 * kPi * ((double)(sync / 2) / symRate));
        // no correction first (a clean carrier, and a noisy estimate must not spoil a good frame), then the estimate and its neighbours
        std::vector<double> cand = {0.0, dfEst, dfEst + 1, dfEst - 1, dfEst + 2, dfEst - 2, dfEst + 3.5, dfEst - 3.5, dfEst + 5, dfEst - 5};
        // pass 0: as they are; pass 1 (full frames only): with the carrier error measured over the header, when pass 0 found nothing
        for (int pass = 0; pass < (bestKind == 1 ? 2 : 1); pass++)
        for (double df : cand) {
            auto rot = [&](size_t i) { return std::polar(1.0, -2 * kPi * df * (double)i / symRate); };
            auto stat = [&]() {
                cd acc(0, 0);
                for (size_t i = 0; i < sync; i++) acc += cd(mf[bestM + i * spsD]) * rot(i) * wsync[i];
                return std::polar(1.0, -std::arg(acc));
            };
            cd ph = stat();
            auto softAt = [&](size_t i) { return (float)(std::complex<double>(mf[bestM + (sync + i) * spsD]) * rot(sync + i) * ph).real(); };
            bool refined = false;
            if (pass == 1 && nAvail >= 4 * kBeaconHeaderBits + 80) {
                // A full frame is 3.4 s long: a carrier error of a fraction of a hertz is turns of phase by its end. The header at its start (CRC-16) is
                // decoded first; if it is right its bits are known symbols, and the carrier error is measured over them (the sync word and 680 symbols)
                std::vector<float> rh(4 * (kBeaconHeaderBits + 40));
                for (size_t i = 0; i < rh.size(); i++) rh[i] = softAt(i);
                const std::vector<uint8_t> hb = beaconViterbi(rh.data(), kBeaconHeaderBits + 40, kBeaconHeaderBits + 40);
                size_t pp = kHdrFieldBits;
                if (crc16(hb.data(), kHdrFieldBits) == get(hb, pp, 16)) {
                    std::vector<uint8_t> hdr(hb.begin(), hb.begin() + kBeaconHeaderBits);
                    const std::vector<uint8_t> coded = beaconConvEncode(hdr);
                    const size_t M = sync + 4 * (size_t)kBeaconHeaderBits;
                    std::vector<cd> z(M);
                    for (size_t i = 0; i < M; i++) {
                        const double sym = i < sync ? wsync[i] : 1.0 - 2.0 * coded[i - sync];
                        z[i] = cd(mf[bestM + i * spsD]) * rot(i) * sym;
                    }
                    const size_t L = 128;
                    cd S(0, 0);
                    for (size_t i = 0; i + L < M; i++) S += z[i + L] * std::conj(z[i]);
                    df += std::arg(S) / (2 * kPi * (double)L / symRate);
                    ph = stat();
                    refined = true;
                }
            }
            if (pass == 1 && !refined) continue;
            size_t nInfo = kBeaconHeaderBits;
            if (bestKind == 1) {
                const size_t probe = kBeaconHeaderBits + 40;
                if (nAvail < 4 * probe) return Wait;
                std::vector<float> r(4 * probe);
                for (size_t i = 0; i < r.size(); i++) r[i] = softAt(i);
                const std::vector<uint8_t> pb = beaconViterbi(r.data(), probe, probe);
                unsigned n = 0;
                for (int i = 146; i < 154; i++) n = (n << 1) | pb[i];
                if (n > kBeaconMaxText) continue;
                nInfo = kBeaconHeaderBits + (n ? 8 * n + 32 : 0);
            }
            const size_t steps = nInfo + kK - 1, need = 4 * steps;
            if (nAvail < need) return Wait;
            std::vector<float> r(need);
            for (size_t i = 0; i < need; i++) r[i] = softAt(i);
            const std::vector<uint8_t> bits = beaconViterbi(r.data(), steps, nInfo);
            Out o;
            if (decodeBeaconBits(bits, o.f, o.text)) {
                ready.push_back(o);
                dropMf(bestM + (sync + need) * spsD);
                return Found;
            }
        }
        DIAG_DEBUG_EVERY(107799, 5.0, "A KAAL beacon sync word (metric %.2f) was found but its frame did not pass the CRC checks, so the frame is dropped.", bestMetric);
        dropMf(bestM + sync * spsD);
        return Dropped;
    }

    bool poll(BeaconFields& f, std::string& text) {
        while (ready.empty()) {
            if (sinceAttempt < gate) return false;
            const Result r = attempt();
            if (r == Wait) { sinceAttempt = 0; return false; }
            sinceAttempt = 1 << 30;
        }
        f = ready.front().f;
        text = ready.front().text;
        ready.pop_front();
        return true;
    }
};

BeaconSliceRx::BeaconSliceRx() : p_(new Impl) { p_->configure(48000); }
BeaconSliceRx::~BeaconSliceRx() { delete p_; }
void BeaconSliceRx::configure(double sampleRate) { p_->configure(sampleRate); }
void BeaconSliceRx::feed(const std::complex<float>* x, size_t n) { p_->feed(x, n); }
bool BeaconSliceRx::poll(BeaconFields& f, std::string& text) { return p_->poll(f, text); }

}  // namespace kaal
}  // namespace dect2
