// 3G-ALE BW0 receiver, PDU reader and test generator; see hfu_ale3g.h. Clean-room from the text of MIL-STD-188-141B Appendix C.
#include "dect2/hfutil/hfu_ale3g.h"
#include "dect2/hfutil/hfu_serial.h"
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>

namespace dect2::hfu {

namespace {
constexpr double kPi = 3.14159265358979323846;
using cf = std::complex<float>;
constexpr double kFs = 24000.0, kBaud = 2400.0, kCarrier = 1800.0;
constexpr int kSps = 10;
constexpr int kNTlc = 256, kNPre = 384, kNData = 832;

// C.5.1.3.1 table C-V (TLC/AGC guard), C.5.1.3.2 table C-VI (BW0 acquisition preamble), C.5.1.3.6 table C-IX (BW0 PN sequence)
constexpr uint8_t kTlc[256] = {
    2,6,1,6,1,6,3,0,6,0,1,1,5,0,0,6,2,6,2,1,6,2,3,2,7,6,4,3,0,2,3,5,2,7,5,1,5,1,7,6,1,7,1,5,4,4,0,7,2,2,6,2,2,2,6,3,3,3,7,7,3,2,4,5,
    0,7,4,7,7,7,2,3,1,6,7,6,5,7,0,5,1,0,7,6,2,4,0,2,7,5,5,4,1,5,1,5,6,7,3,0,2,7,6,6,4,0,7,4,3,2,2,6,6,7,4,7,2,0,2,7,2,1,5,4,6,2,3,2,
    1,6,0,7,1,1,2,6,2,2,0,2,2,3,6,7,1,7,1,7,1,5,7,7,2,2,2,0,4,3,4,2,0,6,7,6,0,5,0,7,1,7,4,1,2,3,4,6,7,2,2,0,6,4,4,6,6,4,2,2,6,5,3,4,
    2,3,5,7,7,1,0,0,0,3,1,2,0,1,6,2,7,4,4,3,2,5,4,5,6,4,2,5,6,2,2,4,7,0,6,2,3,7,2,5,4,2,4,1,5,5,3,6,1,1,3,2,7,5,7,0,7,3,5,0,0,1,2,0};
constexpr uint8_t kPre[384] = {
    7,7,7,7,5,4,3,1,1,2,0,2,7,2,2,0,1,3,4,7,5,3,7,7,4,3,1,0,1,1,5,2,1,6,0,0,4,7,6,2,2,3,6,0,5,1,7,6,1,6,1,7,6,6,6,1,7,3,0,4,7,1,2,2,
    3,3,6,7,7,1,7,3,1,5,0,3,3,4,5,2,5,2,5,3,1,7,2,1,5,7,6,1,2,5,3,5,3,6,2,0,7,5,6,6,0,1,4,2,5,4,1,1,7,0,0,6,6,7,5,6,3,7,4,0,2,6,3,6,
    4,5,1,0,0,4,5,5,4,7,1,5,1,5,6,7,3,3,5,2,2,2,7,2,3,3,0,4,1,4,1,3,6,0,7,2,6,1,5,0,1,4,1,1,7,0,7,4,0,2,4,5,3,0,0,3,1,2,6,4,6,5,2,6,
    0,0,7,3,5,3,4,0,6,2,7,2,3,3,7,6,7,1,0,0,6,7,3,1,5,5,0,2,3,4,2,7,7,4,5,2,1,6,1,0,4,7,1,6,1,2,4,0,3,6,5,4,5,4,4,6,1,2,5,1,3,6,2,7,
    2,6,7,4,7,3,0,1,5,0,5,3,4,5,0,7,3,2,7,0,3,2,7,0,6,1,6,7,7,1,4,2,6,7,7,4,2,7,2,7,3,7,6,3,2,6,5,6,6,3,6,6,4,1,0,6,2,6,4,1,5,5,4,3,
    3,4,6,3,5,2,4,1,1,7,5,3,7,1,6,5,4,6,6,2,3,4,2,3,3,7,4,1,4,4,5,4,6,1,3,4,6,1,7,4,1,3,5,2,6,5,5,4,2,1,5,1,6,1,2,7,1,4,4,2,3,4,7,3};
constexpr uint8_t kPn[256] = {
    0,2,4,3,3,6,4,5,7,6,7,0,5,5,4,3,5,4,3,7,0,7,6,2,6,2,4,6,7,2,4,7,5,5,7,0,7,3,3,3,7,3,3,1,4,2,3,7,0,2,7,7,3,5,1,0,1,4,0,5,0,0,0,0,
    6,5,0,1,2,7,6,5,5,2,7,3,3,3,2,1,2,5,6,1,3,4,2,1,0,1,2,3,6,4,7,5,2,2,6,2,7,6,5,2,4,6,5,4,7,2,5,1,0,0,7,7,3,5,4,2,1,4,2,7,0,3,4,0,
    0,0,7,7,3,5,4,2,1,4,2,7,0,3,4,0,1,0,5,2,6,0,3,5,1,0,5,1,5,2,5,6,3,2,3,7,1,2,2,0,7,1,3,6,4,2,6,2,7,4,3,7,6,7,2,3,1,7,4,1,5,1,5,4,
    7,1,1,2,3,6,7,7,6,6,1,2,2,4,1,7,7,5,5,4,7,7,5,0,7,3,7,5,7,7,5,0,6,6,6,1,3,4,4,4,0,3,3,2,1,4,5,4,5,3,1,1,1,2,5,1,7,1,5,7,2,0,0,6};

cf psk8(int n) { const double a = (n & 7) * kPi / 4; return cf((float)std::cos(a), (float)std::sin(a)); }
inline int par(uint32_t v) { return __builtin_parity(v); }
int walshChip(int n, int j) { return par((uint32_t)(n & j)) ? 4 : 0; }   // table C-VIII: row n, chip j (0 or 4)

// C.5.1.2 with the BW0 parameters of table C-VII: for each fetch position, the stuffed (coded) bit index
const std::vector<int>& bw0Order() {
    static const std::vector<int> o = [] {
        const int R = 4, C = 13, irs = 0, ics = 1, dirs = 1, dics = 0, irf = 1, icf = 0, dirf = 0, dicf = 1;
        std::vector<int> m(R * C, -1), out;
        int rs = 0, cs = 0;
        for (int s = 0; s < R * C;) {
            m[rs * C + cs] = s;
            s++;
            cs = (s % R == 0) ? (cs + ics + dics) % C : (cs + ics) % C;
            rs = (s % C == 0) ? (rs + irs + dirs) % R : (rs + irs) % R;
        }
        int rf = 0, cfp = 0;
        for (int f = 0; f < R * C;) {
            out.push_back(m[rf * C + cfp]);
            f++;
            cfp = (f % R == 0) ? (cfp + icf + dicf) % C : (cfp + icf) % C;
            rf = (f % C == 0) ? (rf + irf + dirf) % R : (rf + irf) % R;
        }
        return out;
    }();
    return o;
}

std::vector<float> rrc() {
    const int half = 60;
    const double b = 0.35;
    std::vector<float> h(2 * half + 1);
    double e = 0;
    for (int i = -half; i <= half; i++) {
        const double t = (double)i / kSps;
        double v;
        if (i == 0) v = 1 - b + 4 * b / kPi;
        else if (std::fabs(std::fabs(4 * b * t) - 1) < 1e-9) v = b / std::sqrt(2.0) * ((1 + 2 / kPi) * std::sin(kPi / (4 * b)) + (1 - 2 / kPi) * std::cos(kPi / (4 * b)));
        else v = (std::sin(kPi * t * (1 - b)) + 4 * b * t * std::cos(kPi * t * (1 + b))) / (kPi * t * (1 - 16 * b * b * t * t));
        h[(size_t)(i + half)] = (float)v;
        e += v * v;
    }
    for (auto& v : h) v = (float)(v / std::sqrt(e));
    return h;
}

uint32_t field(uint32_t b, int hi, int n) { return (b >> (hi - n + 1)) & ((1u << n) - 1); }

const char* callType(int t) {
    static const char* n[8] = {"3G ARQ packet data", "HF modem circuit", "analog voice circuit", "high-quality circuit", "unicast ARQ packet", "unicast circuit", "multicast circuit", "control"};
    return n[t & 7];
}
}  // namespace

uint8_t ale3gCrc(uint32_t bits, int nBits, int crcBits) {
    const uint32_t poly = crcBits == 4 ? 0x0Bu : 0x9Bu, top = 1u << (crcBits - 1), mask = (1u << crcBits) - 1;
    uint32_t r = 0;
    for (int i = nBits - 1; i >= 0; i--) {
        const uint32_t u = (bits >> i) & 1u;
        const uint32_t fb = u ^ ((r & top) ? 1u : 0u);
        r = (r << 1) & mask;
        if (fb) r ^= poly;
    }
    return (uint8_t)r;
}

int ale3gLinkId(int caller, int responder) {
    const uint32_t t1 = (uint32_t)caller * 0x13C6EFu, t2 = (uint32_t)responder * 0x13C6EFu;
    return (int)(((t1 >> 4) + (t2 >> 15)) & 0x3F);
}

uint32_t ale3gCall(int calledMember, int ct, int callerMember, int callerGroup) {
    const uint32_t b = ((uint32_t)(calledMember & 63) << 14) | ((uint32_t)(ct & 7) << 11) | ((uint32_t)(callerMember & 63) << 5) | (uint32_t)(callerGroup & 31);   // 22 bits: 00 + 6 + 3 + 6 + 5
    return (b << 4) | ale3gCrc(b, 22, 4);
}
uint32_t ale3gNotification(int status, int callerMember, int callerGroup) { return ale3gCall(63, status, callerMember, callerGroup); }
uint32_t ale3gHandshake(int linkId, int command, int argument) {
    const uint32_t b = (2u << 16) | ((uint32_t)(linkId & 63) << 10) | ((uint32_t)(command & 7) << 7) | (uint32_t)(argument & 127);   // 18 bits: 10 + 6 + 3 + 7
    return (b << 8) | ale3gCrc(b, 18, 8);
}
uint32_t ale3gBroadcast(int countdown, int ct, int channel) {
    const uint32_t b = (0x0Eu << 13) | ((uint32_t)(countdown & 7) << 10) | ((uint32_t)(ct & 7) << 7) | (uint32_t)(channel & 127);   // 01110 + 3 + 3 + 7
    return (b << 8) | ale3gCrc(b, 18, 8);
}

Ale3gPdu ale3gParse(uint32_t b) {
    Ale3gPdu p;
    p.bits = b & 0x3FFFFFFu;
    b = p.bits;
    char t[200];
    const int pre = (int)field(b, 25, 2);
    if (pre == 0) {
        p.crcOk = ale3gCrc(b >> 4, 22, 4) == (b & 15u);
        const int called = (int)field(b, 23, 6), ct = (int)field(b, 17, 3), mem = (int)field(b, 14, 6), grp = (int)field(b, 8, 5);
        if (called == 63) {
            static const char* st[8] = {"nominal", "time server", "status 2", "status 3", "status 4", "status 5", "commencing EMCON", "leaving network"};
            p.type = "LE_Notification";
            snprintf(t, sizeof t, "station group %d member %d (address %d): %s", grp, mem, (mem << 5) | grp, st[ct]);
        } else {
            p.type = "LE_Call";
            snprintf(t, sizeof t, "group %d member %d (address %d) calls member %d of group %d: %s", grp, mem, (mem << 5) | grp, called, grp, callType(ct));
        }
    } else if (pre == 2) {
        p.crcOk = ale3gCrc(b >> 8, 18, 8) == (b & 255u);
        static const char* cmd[8] = {"continue handshake", "commence traffic setup", "voice traffic", "link release", "sync check", "command 5", "data", "abort handshake"};
        const int lid = (int)field(b, 23, 6), c = (int)field(b, 17, 3), arg = (int)field(b, 14, 7);
        p.type = "LE_Handshake";
        if (c == 1 || c == 2 || c == 3) snprintf(t, sizeof t, "link %02X: %s, channel %d", lid, cmd[c], arg);
        else if (c == 0 || c == 7) {
            static const char* why[4] = {"no response", "rejected", "no traffic channel", "low quality"};
            snprintf(t, sizeof t, "link %02X: %s, reason %s", lid, cmd[c], arg < 4 ? why[arg] : "reserved");
        } else snprintf(t, sizeof t, "link %02X: %s, argument %d", lid, cmd[c], arg);
    } else if (field(b, 25, 5) == 0x0E) {
        p.crcOk = ale3gCrc(b >> 8, 18, 8) == (b & 255u);
        p.type = "LE_Broadcast";
        snprintf(t, sizeof t, "countdown %u, %s, channel %u", field(b, 20, 3), callType((int)field(b, 17, 3)), field(b, 14, 7));
    } else {
        p.crcOk = ale3gCrc(b >> 8, 18, 8) == (b & 255u);   // the other PDUs of figure C-17 carry an 8-bit CRC
        p.type = "3G-ALE PDU";
        snprintf(t, sizeof t, "bits %07X (fields not read)", b);
    }
    p.text = t;
    return p;
}

std::vector<uint8_t> bw0Encode(uint32_t bits26) {
    uint8_t p[26];
    for (int i = 0; i < 26; i++) p[i] = (uint8_t)((bits26 >> (25 - i)) & 1u);
    // C.5.1.3.3: cells x1 .. x6 hold p20 .. p25 (x6 the most recent), then p0 .. p25 are shifted in
    uint32_t r = 0;
    for (int i = 20; i < 26; i++) r = (r >> 1) | ((uint32_t)p[i] << 6);
    std::vector<uint8_t> coded;
    for (int i = 0; i < 26; i++) {
        r = (r >> 1) | ((uint32_t)p[i] << 6);
        coded.push_back((uint8_t)par(r & 0b1011011u));
        coded.push_back((uint8_t)par(r & 0b1111001u));
    }
    std::vector<uint8_t> out(52);
    const auto& o = bw0Order();
    for (int f = 0; f < 52; f++) out[(size_t)f] = coded[(size_t)o[(size_t)f]];
    return out;
}

uint32_t bw0Decode(const std::vector<float>& soft) {
    if (soft.size() != 52) return 0;
    std::vector<float> c(52);
    const auto& o = bw0Order();
    for (int f = 0; f < 52; f++) c[(size_t)o[(size_t)f]] = soft[(size_t)f];
    // tail-biting: three times round, keep the middle
    Viterbi v;
    for (int rep = 0; rep < 3; rep++) for (int k = 0; k < 26; k++) v.push(c[(size_t)(2 * k)], c[(size_t)(2 * k + 1)]);
    std::vector<uint8_t> d;
    v.flush(d);
    if (d.size() < 78) return 0;
    uint32_t b = 0;
    for (int k = 0; k < 26; k++) b = (b << 1) | d[(size_t)(26 + k)];
    return b;
}

std::vector<cf32> genBw0(uint32_t bits26, double leadSec, double tailSec) {
    std::vector<int> sy;
    for (int i = 0; i < kNTlc; i++) sy.push_back(kTlc[i]);
    for (int i = 0; i < kNPre; i++) sy.push_back(kPre[i]);
    const auto coded = bw0Encode(bits26);
    for (int g = 0; g < 13; g++) {
        const int n = (coded[(size_t)(4 * g)] << 3) | (coded[(size_t)(4 * g + 1)] << 2) | (coded[(size_t)(4 * g + 2)] << 1) | coded[(size_t)(4 * g + 3)];
        for (int i = 0; i < 64; i++) sy.push_back((walshChip(n, i & 15) + kPn[(64 * g + i) & 255]) & 7);
    }
    const std::vector<float> h = rrc();
    const size_t lead = (size_t)(leadSec * kFs), tail = (size_t)(tailSec * kFs);
    std::vector<cf32> out(lead + sy.size() * kSps + h.size() + tail, cf32(0, 0));
    for (size_t s = 0; s < sy.size(); s++) {
        const cf v = psk8(sy[s]);
        for (size_t k = 0; k < h.size(); k++) out[lead + s * kSps + k] += v * h[k];
    }
    double ph = 0;
    const double w = 2 * kPi * kCarrier / kFs;
    for (auto& v : out) { v *= cf32((float)std::cos(ph), (float)std::sin(ph)) * 3.16f; ph += w; if (ph > kPi) ph -= 2 * kPi; }
    return out;
}

struct Ale3gRx::Impl {
    std::vector<float> h = rrc();
    std::vector<cf> hist;
    size_t hpos = 0;
    double nco = 0;
    std::vector<cf> y;
    int64_t y0 = 0, nY = 0, scan = 0;
    std::vector<cf> te;                // the preamble's differential products, conjugated
    double candM = 0; int64_t candT = -1; cf candC;
    uint64_t nBursts = 0, nGood = 0;
    double cfoLast = 0, snrLast = 0;
    std::function<void(const Ale3gPdu&)> cb;

    Impl() {
        hist.assign(h.size(), cf(0, 0));
        for (int k = 1; k < kNPre; k++) te.push_back(std::conj(psk8(kPre[k]) * std::conj(psk8(kPre[k - 1]))));
    }

    cf at(double pos, double cfo) const {
        const int64_t i = (int64_t)std::floor(pos);
        const float fr = (float)(pos - (double)i);
        const int64_t a = i - y0;
        if (a < 0 || a + 1 >= (int64_t)y.size()) return cf(0, 0);
        const cf v = y[(size_t)a] * (1 - fr) + y[(size_t)a + 1] * fr;
        const double ph = -2 * kPi * cfo * pos / kFs;
        return v * cf((float)std::cos(ph), (float)std::sin(ph));
    }
    cf fit(double t, double cfo, int lo, int hi) const {
        cf a(0, 0);
        for (int k = lo; k < hi; k++) a += at(t + (double)k * kSps, cfo) * std::conj(psk8(kPre[k]));
        return a;
    }

    void push(const cf32* x, size_t n) {
        const double w = -2 * kPi * kCarrier / kFs;
        const size_t L = h.size();
        for (size_t i = 0; i < n; i++) {
            hist[hpos] = cf(x[i]) * cf((float)std::cos(nco), (float)std::sin(nco));
            nco += w;
            if (nco < -kPi) nco += 2 * kPi;
            cf acc(0, 0);
            size_t j = hpos;
            for (size_t k = 0; k < L; k++) { acc += hist[j] * h[k]; j = j == 0 ? L - 1 : j - 1; }
            hpos = (hpos + 1) % L;
            y.push_back(acc);
            nY++;
        }
        const int64_t need = (int64_t)(kNPre + kNData) * kSps + 8;   // a whole burst after the preamble start
        if (scan < y0 + kSps) scan = y0 + kSps;
        while (scan + need < nY) {
            cf c(0, 0);
            float nrm = 0;
            const int64_t base = scan - y0;
            for (int k = 1; k < kNPre; k++) {
                const int64_t a = base + (int64_t)k * kSps;
                const cf p = y[(size_t)a] * std::conj(y[(size_t)(a - kSps)]);
                c += p * te[(size_t)k - 1];
                nrm += std::abs(p);
            }
            const double m = nrm > 0 ? std::abs(c) / nrm : 0;
            if (m > 0.35 && m > candM) { candM = m; candT = scan; candC = c; }
            if (candT >= 0 && scan > candT + 2 * kSps) {
                const int64_t t = candT;
                const double f0 = std::arg(candC) * kBaud / (2 * kPi);
                candM = 0; candT = -1;
                burst(t, f0);
                scan = t + (int64_t)(kNPre + kNData) * kSps;
                continue;
            }
            scan++;
        }
        const int64_t keepFrom = std::min<int64_t>(nY - 40000, scan - 2000);
        if (keepFrom > y0 + 40000) { y.erase(y.begin(), y.begin() + (long)(keepFrom - y0)); y0 = keepFrom; }
    }

    void burst(int64_t t, double f0) {
        nBursts++;
        double bestT = (double)t, bestE = -1;
        for (double dt = -1.0; dt <= 1.0001; dt += 0.1) {
            const double e = std::abs(fit((double)t + dt, f0, 0, kNPre));
            if (e > bestE) { bestE = e; bestT = (double)t + dt; }
        }
        double f = f0;
        for (int span : {96, 192}) {   // the phase slope across the preamble, first in quarters then in halves
            const cf a = fit(bestT, f, 0, span), b = fit(bestT, f, span, 2 * span);
            f += std::arg(b * std::conj(a)) * kBaud / (2 * kPi * (double)span);
        }
        cf hh = fit(bestT, f, 0, kNPre) / (float)kNPre;
        double er = 0;
        for (int k = 0; k < kNPre; k++) er += std::norm(at(bestT + (double)k * kSps, f) - hh * psk8(kPre[k]));
        const double noise = std::max(er / kNPre, 1e-12);
        snrLast = 10 * std::log10(std::norm(hh) / noise);
        cfoLast = f;
        // 13 Walsh symbols, each 16 chips repeated four times; coherent with the channel followed from symbol to symbol
        std::vector<float> soft;
        for (int g = 0; g < 13; g++) {
            cf z[64];
            for (int i = 0; i < 64; i++) {
                const int m = kNPre + 64 * g + i;
                z[i] = at(bestT + (double)m * kSps, f) * std::conj(psk8(kPn[(64 * g + i) & 255]));
            }
            float met[16];
            cf cc[16];
            int best = 0;
            for (int n = 0; n < 16; n++) {
                cf a(0, 0);
                for (int i = 0; i < 64; i++) a += walshChip(n, i & 15) ? -z[i] : z[i];
                cc[n] = a;
                met[n] = (a * std::conj(hh)).real();
                if (met[n] > met[best]) best = n;
            }
            const float scale = (float)(1.0 / (32.0 * noise));
            for (int k = 0; k < 4; k++) {
                float m0 = -1e30f, m1 = -1e30f;
                for (int n = 0; n < 16; n++) { if ((n >> (3 - k)) & 1) m1 = std::max(m1, met[n]); else m0 = std::max(m0, met[n]); }
                soft.push_back(std::clamp((m0 - m1) * scale, -50.f, 50.f));
            }
            hh = 0.5f * hh + 0.5f * cc[best] / 64.0f;
        }
        Ale3gPdu p = ale3gParse(bw0Decode(soft));
        p.snrDb = snrLast; p.cfoHz = cfoLast;
        if (p.crcOk) nGood++;
        if (cb) cb(p);
    }
};

Ale3gRx::Ale3gRx() : p_(new Impl) {}
Ale3gRx::~Ale3gRx() = default;
void Ale3gRx::reset() { auto c = std::move(p_->cb); p_.reset(new Impl); p_->cb = std::move(c); }
void Ale3gRx::push(const cf32* x, size_t n) { p_->push(x, n); }
void Ale3gRx::setPduSink(std::function<void(const Ale3gPdu&)> cb) { p_->cb = std::move(cb); }
uint64_t Ale3gRx::bursts() const { return p_->nBursts; }
uint64_t Ale3gRx::good() const { return p_->nGood; }
double Ale3gRx::lastCfoHz() const { return p_->cfoLast; }
double Ale3gRx::lastSnrDb() const { return p_->snrLast; }

}  // namespace dect2::hfu
