// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// NB-IoT physical layer (nbiot_phy.h): messages, synchronisation signals, reference signals, OFDM, channel estimate and soft demapping.
#include "dect2/nbiot/nbiot_phy.h"
#include "dect2/dsp/fftutil.h"
#include "../cell/cell_lte_int.h"
#include <algorithm>
#include <cmath>

namespace dect2::nbiot {

int symCp(int l) { return l % 7 == 0 ? 10 : 9; }
int symUseful(int l) {
    const int slot = l / 7, ll = l % 7;
    const int start = ll == 0 ? 0 : 138 + (ll - 1) * 137;   // first sample of the cyclic prefix in the slot
    return slot * 960 + start + symCp(l);
}

const char* opModeName(int m) {
    switch (m) {
    case kInbandSame: return "in-band (same PCI)";
    case kInbandDiff: return "in-band (other PCI)";
    case kGuardband: return "guard band";
    default: return "standalone";
    }
}
double rasterOffsetKhz(int idx) { static const double t[4] = {-7.5, -2.5, 2.5, 7.5}; return t[idx & 3]; }

// ---------------------------------------------------------------- MIB-NB (36.331 6.7.3.1), 34 bits
Bits mibEncode(const Mib& m) {
    cell::BitWriter w;
    w.u((uint32_t)m.sfnMsb & 15u, 4);
    w.u((uint32_t)m.hsfnLsb & 3u, 2);
    w.u((uint32_t)m.sched & 15u, 4);
    w.u((uint32_t)m.valueTag & 31u, 5);
    w.bit(m.ab);
    w.u((uint32_t)m.mode & 3u, 2);
    switch (m.mode) {
    case kInbandSame: w.u((uint32_t)m.crsSeq & 31u, 5); break;
    case kInbandDiff: w.u((uint32_t)m.numCrs & 1u, 1); w.u((uint32_t)m.raster & 3u, 2); w.u(0, 2); break;
    case kGuardband: w.u((uint32_t)m.raster & 3u, 2); w.u(0, 3); break;
    default: w.u(0, 5); break;
    }
    w.u(0, 11);   // spare
    return w.bits();
}

bool mibParse(const Bits& b, Mib& m) {
    if (b.size() < 34) return false;
    cell::BitReader r(b);
    m.sfnMsb = (int)r.u(4);
    m.hsfnLsb = (int)r.u(2);
    m.sched = (int)r.u(4);
    m.valueTag = (int)r.u(5);
    m.ab = r.bit();
    m.mode = (int)r.u(2);
    switch (m.mode) {
    case kInbandSame: m.crsSeq = (int)r.u(5); break;
    case kInbandDiff: m.numCrs = (int)r.u(1); m.raster = (int)r.u(2); r.skip(2); break;
    case kGuardband: m.raster = (int)r.u(2); r.skip(3); break;
    default: r.skip(5); break;
    }
    return r.ok();
}

// ---------------------------------------------------------------- SIB1-NB (36.331 6.7.3.1)
Bits sib1Encode(const Sib1& s, size_t padTo) {
    cell::BitWriter w;
    w.bit(false);   // BCCH-DL-SCH-MessageType-NB: c1 (not messageClassExtension)
    w.bit(true);    // c1: systemInformationBlockType1-r13
    const bool hasPmax = s.pMax != -99;
    w.bit(hasPmax);                      // optional fields of the SEQUENCE, in order: p-Max,
    for (int i = 0; i < 9; i++) w.bit(false);   // freqBandInfo, multiBandInfoList, downlinkBitmap, eutraControlRegionSize, nrs-CRS-PowerOffset, si-RadioFrameOffset, systemInfoValueTagList, lateNonCriticalExtension, nonCriticalExtension
    w.u((uint32_t)s.hsfnMsb & 255u, 8);
    // cellAccessRelatedInfo
    w.range((int)s.plmns.size(), 1, 6);
    for (const cell::Plmn& p : s.plmns) {
        w.bit(false);                    // attachWithoutPDN-Connectivity absent
        w.bit(true);                     // mcc present
        const int d[3] = {p.mcc / 100 % 10, p.mcc / 10 % 10, p.mcc % 10};
        for (int x : d) w.u((uint32_t)x, 4);
        const bool three = p.mncDigits == 3;
        w.bit(three);                    // MNC SIZE (2..3)
        if (three) { const int e[3] = {p.mnc / 100 % 10, p.mnc / 10 % 10, p.mnc % 10}; for (int x : e) w.u((uint32_t)x, 4); }
        else { const int e[2] = {p.mnc / 10 % 10, p.mnc % 10}; for (int x : e) w.u((uint32_t)x, 4); }
        w.bit(!p.reserved);              // cellReservedForOperatorUse: reserved, notReserved
    }
    w.u(s.tac & 0xFFFFu, 16);
    w.u(s.cellId & 0xFFFFFFFu, 28);
    w.bit(!s.barred);                    // barred, notBarred
    w.bit(!s.intraFreqReselAllowed);     // allowed, notAllowed
    // cellSelectionInfo
    w.range(std::max(-70, std::min(-22, s.qRxLevMinDbm / 2)), -70, -22);
    w.range(std::max(-34, std::min(-3, s.qQualMinDb)), -34, -3);
    if (hasPmax) w.range(std::max(-30, std::min(33, s.pMax)), -30, 33);
    w.range(std::max(1, std::min(256, s.band)), 1, 256);
    // schedulingInfoList: one SI message with SIB3-NB; the receiver stops reading before this
    w.range(1, 1, 8);
    w.u(2, 3);     // si-Periodicity rf256
    w.u(1, 2);     // si-RepetitionPattern every4thRF
    w.range(1, 0, 31); w.u(0, 3);   // sib-MappingInfo: sibType3-NB
    w.u(4, 3);     // si-TB b328
    w.u(0, 3);     // si-WindowLength ms160
    Bits b = w.bits();
    while (b.size() < padTo) b.push_back(0);
    return b;
}

bool sib1Parse(const Bits& b, Sib1& s) {
    cell::BitReader r(b);
    if (r.bit()) return false;       // messageClassExtension
    if (!r.bit()) return false;      // systemInformation: not SIB1-NB
    const bool hasPmax = r.bit();
    for (int i = 0; i < 9; i++) r.bit();
    s.hsfnMsb = (int)r.u(8);
    const int n = r.range(1, 6);
    s.plmns.clear();
    cell::Plmn prev;
    for (int i = 0; i < n && r.ok(); i++) {
        cell::Plmn p;
        const bool attach = r.bit();
        (void)attach;
        const bool hasMcc = r.bit();
        if (hasMcc) { int m = 0; for (int k = 0; k < 3; k++) { const int d = (int)r.u(4); if (d > 9) r.fail(); m = m * 10 + d; } p.mcc = m; }
        else p.mcc = prev.mcc;
        const bool three = r.bit();
        int mn = 0;
        for (int k = 0; k < (three ? 3 : 2); k++) { const int d = (int)r.u(4); if (d > 9) r.fail(); mn = mn * 10 + d; }
        p.mnc = mn; p.mncDigits = three ? 3 : 2;
        p.reserved = !r.bit();
        s.plmns.push_back(p);
        prev = p;
    }
    s.tac = r.u(16);
    s.cellId = r.u(28);
    s.barred = !r.bit();
    s.intraFreqReselAllowed = !r.bit();
    s.qRxLevMinDbm = 2 * r.range(-70, -22);
    s.qQualMinDb = r.range(-34, -3);
    s.pMax = hasPmax ? r.range(-30, 33) : -99;
    s.band = r.range(1, 256);
    return r.ok() && !s.plmns.empty();
}

int sib1Reps(int sched) { static const int t[12] = {4, 8, 16, 4, 8, 16, 4, 8, 16, 4, 8, 16}; return sched >= 0 && sched < 12 ? t[sched] : 0; }
int sib1Tbs(int sched) { static const int t[12] = {208, 208, 208, 328, 328, 328, 440, 440, 440, 680, 680, 680}; return sched >= 0 && sched < 12 ? t[sched] : 0; }
int sib1StartFrame(int reps, int pci) {
    if (reps == 4) return 16 * (pci % 4);
    if (reps == 8) return 16 * (pci % 2);
    return pci % 2;
}

// ---------------------------------------------------------------- NPSS / NSSS
int npssCover(int l) {
    static const int s[11] = {1, 1, 1, 1, -1, -1, 1, 1, 1, -1, 1};
    return l >= 3 && l <= 13 ? s[l - 3] : 1;
}

void npssSeq(cf32 d[11]) {
    for (int n = 0; n < 11; n++) {
        const double ph = -M_PI * 5.0 * n * (n + 1) / 11.0;
        d[n] = cf32((float)std::cos(ph), (float)std::sin(ph));
    }
}

void nsssSeq(int pci, int mfi, cf32 d[132]) {
    static int bq[4][128];
    static bool init = false;
    if (!init) {
        static const int rows[4] = {0, 31, 63, 127};
        for (int q = 0; q < 4; q++) for (int m = 0; m < 128; m++) bq[q][m] = (__builtin_popcount((unsigned)(rows[q] & m)) & 1) ? -1 : 1;
        init = true;
    }
    const int u = pci % 126 + 3, q = pci / 126;
    for (int n = 0; n < 132; n++) {
        const int np = n % 131, m = n % 128;
        const double ph = -2.0 * M_PI * mfi * n / 4.0 - M_PI * u * np * (np + 1) / 131.0;
        d[n] = (float)bq[q][m] * cf32((float)std::cos(ph), (float)std::sin(ph));
    }
}

void npssSymbol(int n, cf32* out) {
    cf32 d[11];
    npssSeq(d);
    for (int t = 0; t < n; t++) {
        std::complex<double> a(0, 0);
        for (int k = 0; k < 11; k++) {
            const double ph = 2 * M_PI * (k - 6) * t / n;
            a += std::complex<double>(d[k]) * std::complex<double>(std::cos(ph), std::sin(ph));
        }
        const double r = M_PI * t / n;
        a *= std::complex<double>(std::cos(r), std::sin(r));
        out[t] = cf32((float)a.real(), (float)a.imag());
    }
}

// ---------------------------------------------------------------- NRS and CRS
int nrsBase(int port, int l, int pci) {
    const int ll = l % 7;
    if (ll != 5 && ll != 6) return -1;
    int v;
    if (port == 0) v = ll == 5 ? 0 : 3;
    else v = ll == 5 ? 3 : 0;
    return (v + pci % 6) % 6;
}

namespace {
struct NrsTable {
    int pci = -1;
    cf32 v[20][2][2];   // slot of the frame, symbol 5 / 6 of the slot, m
};
const NrsTable& nrsTable(int pci) {
    thread_local NrsTable t;
    if (t.pci != pci) {
        t.pci = pci;
        cf32 r[2 * cell::kMaxRb];
        for (int ns = 0; ns < 20; ns++) for (int li = 0; li < 2; li++) {
            cell::lteCrsSeq(pci, ns, 5 + li, false, r);
            for (int m = 0; m < 2; m++) t.v[ns][li][m] = r[m + cell::kMaxRb - 1];   // m' = m + N_RB^max,DL - 1
        }
    }
    return t;
}
} // namespace

cf32 nrsValue(int pci, int sf, int l, int m) { return nrsTable(pci).v[2 * sf + l / 7][l % 7 - 5][m]; }

int crsBase(int port, int l, int vshift) {
    const int ll = l % 7, slot = l / 7;
    int v;
    if (port <= 1) {
        if (ll != 0 && ll != 4) return -1;
        v = port == 0 ? (ll == 0 ? 0 : 3) : (ll == 0 ? 3 : 0);
    } else {
        if (ll != 1) return -1;
        v = port == 2 ? 3 * slot : 3 + 3 * slot;
    }
    return (v + vshift) % 6;
}

std::vector<std::pair<int, int>> npbchRes(int pci) {
    std::vector<std::pair<int, int>> out;
    for (int l = 3; l < kSyms; l++) for (int k = 0; k < kSc; k++) {
        bool used = false;
        for (int p = 0; p < 2 && !used; p++) { const int b = nrsBase(p, l, pci); if (b >= 0 && k % 6 == b) used = true; }
        for (int p = 0; p < 4 && !used; p++) { const int b = crsBase(p, l, pci % 6); if (b >= 0 && k % 6 == b) used = true; }
        if (!used) out.push_back({l, k});
    }
    return out;
}

std::vector<std::pair<int, int>> npdschRes(int pci, int nrsPorts, bool inband, int crsPorts, int crsShift) {
    std::vector<std::pair<int, int>> out;
    const int start = inband ? 3 : 0;
    for (int l = start; l < kSyms; l++) for (int k = 0; k < kSc; k++) {
        bool used = false;
        for (int p = 0; p < nrsPorts && !used; p++) { const int b = nrsBase(p, l, pci); if (b >= 0 && k % 6 == b) used = true; }
        if (inband) for (int p = 0; p < crsPorts && !used; p++) { const int b = crsBase(p, l, crsShift); if (b >= 0 && k % 6 == b) used = true; }
        if (!used) out.push_back({l, k});
    }
    return out;
}

// ---------------------------------------------------------------- OFDM
namespace {
constexpr int kAdv = 3;   // the demodulator's window starts this many samples inside the cyclic prefix

struct Tw {
    cf32 half[kFft + 16];   // e^{j pi t / 128}, t = -16 .. 127 + ...
    Tw() { for (int i = 0; i < kFft + 16; i++) { const double a = M_PI * (i - 16) / kFft; half[i] = cf32((float)std::cos(a), (float)std::sin(a)); } }
    cf32 at(int t) const { return half[t + 16]; }
};
const Tw& tw() { static const Tw t; return t; }
} // namespace

void modulateSubframe(const Grid& g, cf32* out) {
    thread_local Fft f(kFft);
    cf32 b[kFft];
    const Tw& w = tw();
    for (int l = 0; l < kSyms; l++) {
        for (auto& x : b) x = cf32(0, 0);
        for (int k = 0; k < kSc; k++) b[(k - 6 + kFft) % kFft] = g.at(l, k);
        f.inverse(b);   // B[n] = sum a_m e^{j 2 pi m n / 128}, periodic
        const int cp = symCp(l), u = symUseful(l);
        for (int n = -cp; n < kFft; n++) out[u + n] = b[(n + kFft) % kFft] * w.at(n);
    }
}

void demodSubframe(const cf32* x, Grid& g) {
    thread_local Fft f(kFft);
    cf32 b[kFft];
    const Tw& w = tw();
    for (int l = 0; l < kSyms; l++) {
        const int u = symUseful(l) - kAdv;
        for (int n = 0; n < kFft; n++) b[n] = x[u + n] * std::conj(w.at(n - kAdv));
        f.forward(b);
        for (int k = 0; k < kSc; k++) {
            const int m = k - 6;
            const double a = 2 * M_PI * m * kAdv / kFft;
            g.at(l, k) = b[(m + kFft) % kFft] * cf32((float)std::cos(a), (float)std::sin(a)) * (1.0f / kFft);
        }
    }
}

// ---------------------------------------------------------------- channel and demapping
bool estimateChannel(const Grid& y, int pci, int sf, int ports, Chan& c) {
    c.ports = ports;
    double resid = 0, pw = 0;
    int nres = 0, npw = 0;
    for (int p = 0; p < ports; p++) {
        cf32 hs[2];
        for (int s = 0; s < 2; s++) {
            cf32 z[4];
            int nz = 0;
            cf32 acc(0, 0);
            for (int li = 0; li < 2; li++) {
                const int l = 7 * s + 5 + li;
                const int base = nrsBase(p, l, pci);
                for (int m = 0; m < 2; m++) { z[nz] = y.at(l, base + 6 * m) * std::conj(nrsValue(pci, sf, l, m)); acc += z[nz]; nz++; }
            }
            hs[s] = acc * 0.25f;
            for (int i = 0; i < 4; i++) { resid += std::norm(z[i] - hs[s]); nres++; if (p == 0) { pw += std::norm(z[i]); npw++; } }
        }
        // between the slots: magnitude linear, phase along the line between the two slot estimates
        const float m0 = std::abs(hs[0]), m1 = std::abs(hs[1]);
        const float p0 = std::arg(hs[0]), dp = std::arg(hs[1] * std::conj(hs[0]));
        for (int l = 0; l < kSyms; l++) {
            const float t = ((float)l - 5.5f) / 7.0f;
            c.h[p][l] = std::polar(std::max(0.f, m0 + (m1 - m0) * t), p0 + dp * t);
        }
    }
    c.rsrp = npw ? (float)(pw / npw) : 0.f;
    c.nv = nres ? (float)(resid / nres * 4.0 / 3.0) : 1e-3f;
    c.nv = std::max(c.nv, 1e-4f * std::max(c.rsrp, 1e-12f));
    return std::isfinite(c.rsrp) && c.rsrp > 0;
}

void demapQpsk(const Grid& y, const Chan& c, const std::vector<std::pair<int, int>>& res, int ports, std::vector<float>& llr) {
    const size_t n = res.size();
    std::vector<cf32> r(n);
    std::vector<std::vector<cf32>> h((size_t)ports, std::vector<cf32>(n));
    for (size_t j = 0; j < n; j++) {
        r[j] = y.at(res[j].first, res[j].second);
        for (int p = 0; p < ports; p++) h[(size_t)p][j] = c.h[p][res[j].first];
    }
    std::vector<const cf32*> hp;
    for (int p = 0; p < ports; p++) hp.push_back(h[(size_t)p].data());
    cell::lteTxdDecode(r.data(), hp, n, ports, c.nv, llr);
}

Bits npbchCode(const Mib& m, int ports, int pci) {
    Bits b = mibEncode(m);
    cell::appendCrc(b, cell::kCrc16, 16, ports == 2 ? 0xFFFFu : 0u);
    const Bits d = cell::convEncode(b);
    Bits e = cell::convRateMatch(d, 1600);
    const Bits c = cell::gold((uint32_t)pci, 1600);
    for (size_t i = 0; i < e.size(); i++) e[i] ^= c[i];
    return e;
}

} // namespace dect2::nbiot
