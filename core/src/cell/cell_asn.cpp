// Cell towers: UPER of the LTE broadcast system information (36.331). One description of each message serves both directions (the
// Coder reads or writes), so the test signal's encoder and the receiver's parser cannot drift apart; the real cells check both.
#include "dect2/cell/cell_asn.h"
#include <algorithm>
#include <cstdio>

namespace dect2::cell {

int bitsFor(long n) { int b = 0; while ((1L << b) < n) b++; return b; }

uint32_t BitReader::u(int n) {
    uint32_t v = 0;
    for (int i = 0; i < n; i++) {
        if (p_ >= b_.size()) { err_ = true; v <<= 1; continue; }
        v = (v << 1) | b_[p_++];
    }
    return v;
}

int BitReader::range(int lo, int hi) { return lo + (int)u(bitsFor((long)hi - lo + 1)); }

bool BitReader::skipExtensions() {
    // normally small length of the extension bitmap, the bitmap, then one open type per present addition
    if (bit()) { err_ = true; return false; }
    const int n = (int)u(6) + 1;
    std::vector<bool> pres((size_t)n);
    for (int i = 0; i < n; i++) pres[(size_t)i] = bit();
    for (int i = 0; i < n; i++) {
        if (!pres[(size_t)i]) continue;
        size_t len = u(8);
        if (len & 0x80) {
            if (len & 0x40) { err_ = true; return false; }   // fragmented: not in system information
            len = ((len & 0x3f) << 8) | u(8);
        }
        skip(len * 8);
    }
    return ok();
}

void BitWriter::range(int v, int lo, int hi) { u((uint32_t)(v - lo), bitsFor((long)hi - lo + 1)); }

std::string plmnText(const Plmn& p) {
    char b[24];
    if (p.mncDigits == 3) snprintf(b, sizeof b, "%03d-%03d", p.mcc, p.mnc);
    else snprintf(b, sizeof b, "%03d-%02d", p.mcc, p.mnc);
    return b;
}

static const int kQOff[31] = {-24, -22, -20, -18, -16, -14, -12, -10, -8, -6, -5, -4, -3, -2, -1, 0, 1, 2, 3, 4, 5, 6, 8, 10, 12, 14, 16, 18, 20, 22, 24};
int qOffsetDb(int i) { return (i >= 0 && i < 31) ? kQOff[i] : 0; }
int qOffsetIdx(int db) { for (int i = 0; i < 31; i++) if (kQOff[i] == db) return i; return 15; }

namespace {

struct Coder {
    BitReader* r = nullptr;
    BitWriter* w = nullptr;
    bool rd() const { return r != nullptr; }
    bool ok() const { return !r || r->ok(); }
    void u(uint32_t& v, int n) { if (r) v = r->u(n); else w->u(v, n); }
    void i(int& v, int lo, int hi) { if (r) v = r->range(lo, hi); else w->range(std::max(lo, std::min(hi, v)), lo, hi); }
    void b(bool& v) { if (r) v = r->bit(); else w->bit(v); }
    void skipI(int lo, int hi, int def = 0) { int t = def; i(t, lo, hi); }
    void skipB() { bool t = false; b(t); }
    void ev(int& val, const int* tab, int n) {      // ENUMERATED stored as its value from tab
        int idx = 0;
        if (!r) { for (int k = 0; k < n; k++) if (tab[k] == val) idx = k; }
        i(idx, 0, n - 1);
        if (r) val = tab[idx];
    }
    void ext(bool& e) { if (!r) e = false; b(e); }
    void tail(bool e) { if (r && e) r->skipExtensions(); }
};

const int kPreambles[16] = {4, 8, 12, 16, 20, 24, 28, 32, 36, 40, 44, 48, 52, 56, 60, 64};
const int kTransMax[11] = {3, 4, 5, 6, 7, 8, 10, 20, 50, 100, 200};
const int kRaWin[8] = {2, 3, 4, 5, 6, 7, 8, 10};
const int kModCoeff[4] = {2, 4, 8, 16};
const int kPagCycle[4] = {32, 64, 128, 256};
const int kT300[8] = {100, 200, 300, 400, 600, 1000, 1500, 2000};
const int kT310[7] = {0, 50, 100, 200, 500, 1000, 2000};
const int kN310[8] = {1, 2, 3, 4, 6, 8, 10, 20};
const int kT311[7] = {1000, 3000, 5000, 10000, 15000, 20000, 30000};
const int kN311[8] = {1, 2, 3, 4, 5, 6, 8, 10};
const int kTa[8] = {500, 750, 1280, 1920, 2560, 5120, 10240, -1};
const int kRb[6] = {6, 15, 25, 50, 75, 100};
const int kSiPer[7] = {8, 16, 32, 64, 128, 256, 512};
const int kSiWin[7] = {1, 2, 5, 10, 15, 20, 40};
const int kQHyst[16] = {0, 1, 2, 3, 4, 5, 6, 8, 10, 12, 14, 16, 18, 20, 22, 24};

void plmnId(Coder& c, Plmn& p, Plmn& prev) {
    bool hasMcc = !c.rd() ? true : false;
    c.b(hasMcc);
    if (hasMcc) {
        int d[3] = {p.mcc / 100, p.mcc / 10 % 10, p.mcc % 10};
        for (int& x : d) c.i(x, 0, 9);
        if (c.rd()) p.mcc = d[0] * 100 + d[1] * 10 + d[2];
    } else if (c.rd()) p.mcc = prev.mcc;
    bool three = p.mncDigits == 3;
    c.b(three);   // SIZE (2..3)
    if (c.rd()) p.mncDigits = three ? 3 : 2;
    if (three) { int d[3] = {p.mnc / 100, p.mnc / 10 % 10, p.mnc % 10}; for (int& x : d) c.i(x, 0, 9); if (c.rd()) p.mnc = d[0] * 100 + d[1] * 10 + d[2]; }
    else { int d[2] = {p.mnc / 10, p.mnc % 10}; for (int& x : d) c.i(x, 0, 9); if (c.rd()) p.mnc = d[0] * 10 + d[1]; }
    prev = p;
}

// SIB1's non-critical extensions (36.331): v890 { lateNonCriticalExtension OCTET STRING (CONTAINING v8h0), nonCriticalExtension v920 },
// v8h0 { multiBandInfoList SEQUENCE (SIZE (1..8)) OF FreqBandIndicator (1..64), nonCriticalExtension v9e0 },
// v9e0 { freqBandIndicator-v9e0 (65..256), multiBandInfoList-v9e0 SEQUENCE (SIZE (1..8)) OF SEQUENCE { freqBandIndicator-v9e0 OPTIONAL }, ... },
// v920 { ims-EmergencySupport-r9 ENUMERATED {true} OPTIONAL, ... }. Read as far as these; later additions are left alone.
bool needsExt(const LteSib1& s) { return s.band > 64 || !s.multiBands.empty() || s.imsEmergency; }

void sib1V8h0(Coder& c, LteSib1& s) {
    std::vector<int> root, hi;   // the root list (1..64) and, for each entry, its v9e0 band (0: none)
    for (int b : s.multiBands) { root.push_back(b > 64 ? 64 : b); hi.push_back(b > 64 ? b : 0); }
    bool hasList = !root.empty();
    bool hasV9e0 = s.band > 64 || std::any_of(hi.begin(), hi.end(), [](int v) { return v > 0; });
    c.b(hasList); c.b(hasV9e0);
    if (hasList) {
        int n = (int)root.size(); c.i(n, 1, 8);
        if (c.rd()) { root.assign((size_t)n, 0); hi.assign((size_t)n, 0); }
        for (int& b : root) c.i(b, 1, 64);
    }
    if (hasV9e0 && c.ok()) {
        bool hasFbi = s.band > 64, hasMl = std::any_of(hi.begin(), hi.end(), [](int v) { return v > 0; }), hasNce = false;
        c.b(hasFbi); c.b(hasMl); c.b(hasNce);
        int fbi = s.band > 64 ? s.band : 65;
        if (hasFbi) { c.i(fbi, 65, 256); if (c.rd() && c.ok()) s.band = fbi; }
        if (hasMl) {
            int n = (int)hi.size(); c.i(n, 1, 8);
            if (c.rd()) hi.assign((size_t)n, 0);
            for (int& v : hi) {
                bool has = v > 0; c.b(has);
                int t = v > 0 ? v : 65;
                if (has) { c.i(t, 65, 256); if (c.rd()) v = t; }
            }
        }
    }
    if (c.rd() && c.ok()) {
        s.multiBands.clear();
        for (size_t k = 0; k < root.size(); k++) s.multiBands.push_back(k < hi.size() && hi[k] > 0 ? hi[k] : root[k]);
    }
}

void sib1Ext(Coder& c, LteSib1& s) {
    // v890
    bool hasLate = !s.multiBands.empty() || s.band > 64, hasV920 = s.imsEmergency;
    c.b(hasLate); c.b(hasV920);
    if (hasLate) {
        if (c.rd()) {
            BitReader& r = *c.r;
            size_t len = r.u(8);
            if (len & 0x80) { if (len & 0x40) { r.fail(); return; } len = ((len & 0x3f) << 8) | r.u(8); }
            const size_t start = r.pos();
            if (!r.ok()) return;
            Bits inner;
            for (size_t k = 0; k < len * 8 && r.ok(); k++) inner.push_back((uint8_t)r.bit());
            if (!r.ok()) return;
            BitReader ir(inner);
            Coder ic; ic.r = &ir;
            sib1V8h0(ic, s);
            (void)start;
        } else {
            BitWriter iw;
            Coder ic; ic.w = &iw;
            sib1V8h0(ic, s);
            iw.padOctet();
            const size_t len = iw.bits().size() / 8;
            if (len < 128) c.w->u((uint32_t)len, 8); else c.w->u((uint32_t)(0x8000 | len), 16);
            for (uint8_t v : iw.bits()) c.w->bit(v != 0);
        }
    }
    if (hasV920 && c.ok()) {
        bool hasIms = s.imsEmergency, hasCsi = false, hasNce = false;
        c.b(hasIms); c.b(hasCsi); c.b(hasNce);
        if (hasIms) { int t = 0; c.i(t, 0, 0); }   // ENUMERATED {true}: no bits
        if (c.rd() && c.ok()) s.imsEmergency = hasIms;
    }
}

void sib1(Coder& c, LteSib1& s) {
    bool hasPmax = s.pMax != -99, hasTdd = s.tdd, hasNce = !c.rd() && needsExt(s);
    c.b(hasPmax); c.b(hasTdd); c.b(hasNce);
    // cellAccessRelatedInfo
    bool hasCsgId = false;
    c.b(hasCsgId);
    int n = (int)s.plmns.size();
    c.i(n, 1, 6);
    if (c.rd()) s.plmns.assign((size_t)n, Plmn());
    Plmn prev;
    for (auto& p : s.plmns) {
        plmnId(c, p, prev);
        int res = p.reserved ? 0 : 1;   // reserved, notReserved
        c.i(res, 0, 1);
        if (c.rd()) p.reserved = res == 0;
    }
    c.u(s.tac, 16);
    c.u(s.cellId, 28);
    int barred = s.barred ? 0 : 1; c.i(barred, 0, 1); if (c.rd()) s.barred = barred == 0;
    int ifr = s.intraFreqReselAllowed ? 0 : 1; c.i(ifr, 0, 1); if (c.rd()) s.intraFreqReselAllowed = ifr == 0;
    c.b(s.csgIndication);
    if (hasCsgId) { uint32_t id = 0; c.u(id, 27); }
    // cellSelectionInfo
    bool hasOff = s.qRxLevMinOffset != 0;
    c.b(hasOff);
    int q = s.qRxLevMinDbm / 2; c.i(q, -70, -22); if (c.rd()) s.qRxLevMinDbm = q * 2;
    if (hasOff) c.i(s.qRxLevMinOffset, 1, 8);
    if (hasPmax) c.i(s.pMax, -30, 33);
    int rootBand = s.band > 64 ? 64 : s.band;
    c.i(rootBand, 1, 64);
    if (c.rd()) s.band = rootBand;
    int ns = (int)s.sched.size();
    c.i(ns, 1, 32);
    if (c.rd()) s.sched.assign((size_t)ns, LteSib1::Sched());
    for (auto& e : s.sched) {
        c.ev(e.periodRf, kSiPer, 7);
        int m = (int)e.sibs.size();
        c.i(m, 0, 31);
        if (c.rd()) e.sibs.assign((size_t)m, 0);
        for (int& t : e.sibs) {
            bool ex = t >= 12; c.b(ex);   // SIB-Type is extensible: sibType12-v920 and later sit in the extension
            if (ex) {                     // a normally small non-negative whole number
                bool big = false; c.b(big);
                if (big) { if (c.r) c.r->fail(); return; }
                int idx = t - 12; c.i(idx, 0, 63);
                if (c.rd()) t = idx + 12;
                continue;
            }
            int idx = t - 3; c.i(idx, 0, 15);
            if (c.rd()) t = idx + 3;
        }
        if (!c.ok()) return;
    }
    if (hasTdd) { c.i(s.sfAssign, 0, 6); c.i(s.specialSf, 0, 8); }
    c.ev(s.siWindowMs, kSiWin, 7);
    c.i(s.valueTag, 0, 31);
    if (hasNce) sib1Ext(c, s);
}

void acConfig(Coder& c, bool& barred) {
    int factor = barred ? 0 : 15; c.i(factor, 0, 15);
    if (c.rd()) barred = factor < 15;
    c.skipI(0, 7); uint32_t sp = 0; c.u(sp, 5);
}

void sib2(Coder& c, LteSib2& s) {
    bool e; c.ext(e);
    bool hasAc = s.acBarring, hasMbsfn = s.mbsfnConfigs > 0;
    c.b(hasAc); c.b(hasMbsfn);
    if (hasAc) {
        bool hs = s.acSignallingBarred, hd = s.acDataBarred;
        c.b(hs); c.b(hd);   // the preamble comes before the first component
        c.b(s.acEmergencyBarred);
        if (hs) acConfig(c, s.acSignallingBarred);
        if (hd) acConfig(c, s.acDataBarred);
        if (c.rd()) s.acBarring = true;
    }
    // radioResourceConfigCommon
    bool eR; c.ext(eR);
    {   // rach-ConfigCommon
        bool eRach; c.ext(eRach);
        bool hasGA = false; c.b(hasGA);
        c.ev(s.numRaPreambles, kPreambles, 16);
        if (hasGA) { bool eg; c.ext(eg); c.skipI(0, 14); c.skipI(0, 3); c.skipI(0, 7); c.tail(eg); }
        c.skipI(0, 3, 1); c.skipI(0, 15, 10);
        c.ev(s.preambleTransMax, kTransMax, 11);
        c.ev(s.raRespWindow, kRaWin, 8);
        c.skipI(0, 7, 3);
        c.skipI(1, 8, 4);
        c.tail(eRach);
    }
    c.ev(s.modPeriodCoeff, kModCoeff, 4);
    c.ev(s.pagingCycleRf, kPagCycle, 4); c.skipI(0, 7, 2);
    c.i(s.rootSeqIndex, 0, 837);
    c.i(s.prachConfigIndex, 0, 63); c.b(s.highSpeed); c.skipI(0, 15, 12); c.i(s.prachFreqOffset, 0, 94);
    c.i(s.refSignalPowerDbm, -60, 50); c.i(s.pb, 0, 3);
    c.skipI(1, 4, 1); c.skipI(0, 1); c.skipI(0, 98, 4); c.b(s.enable64qam);
    c.skipB(); c.skipI(0, 29); c.skipB(); c.skipI(0, 7);
    c.skipI(0, 2, 1); c.skipI(0, 98, 2); c.skipI(0, 7); c.i(s.n1PucchAn, 0, 2047);
    {   // soundingRS-UL-ConfigCommon
        int ch = 0; c.i(ch, 0, 1);
        if (ch == 1) { bool mx = false; c.b(mx); c.skipI(0, 7); c.skipI(0, 15); c.skipB(); }
    }
    c.i(s.p0NominalPusch, -126, 24); c.i(s.alpha, 0, 7); c.i(s.p0NominalPucch, -127, -96);
    c.skipI(0, 2, 1); c.skipI(0, 2, 1); c.skipI(0, 3, 1); c.skipI(0, 2, 1); c.skipI(0, 2, 1);
    c.skipI(-1, 6, 4);
    c.skipI(0, 1);
    c.tail(eR);
    // ue-TimersAndConstants
    bool eT; c.ext(eT);
    c.ev(s.t300, kT300, 8); c.ev(s.t301, kT300, 8); c.ev(s.t310, kT310, 7); c.ev(s.n310, kN310, 8); c.ev(s.t311, kT311, 7); c.ev(s.n311, kN311, 8);
    c.tail(eT);
    // freqInfo
    bool hasUl = s.ulEarfcn >= 0, hasBw = s.ulBwRb > 0;
    c.b(hasUl); c.b(hasBw);
    if (hasUl) { uint32_t v = (uint32_t)std::max(0, s.ulEarfcn); c.u(v, 16); if (c.rd()) s.ulEarfcn = (int)v; }
    if (hasBw) c.ev(s.ulBwRb, kRb, 6);
    c.i(s.addSpectrumEmission, 1, 32);
    if (hasMbsfn) {
        int n = std::max(1, s.mbsfnConfigs); c.i(n, 1, 8);
        if (c.rd()) s.mbsfnConfigs = n;
        for (int k = 0; k < n; k++) { c.skipI(0, 5); c.skipI(0, 7); int ch = 0; c.i(ch, 0, 1); uint32_t a = 0; c.u(a, ch ? 24 : 6); }
    }
    c.ev(s.timeAlignMs, kTa, 8);
    if (c.rd()) s.extended = e;
    c.tail(e);
}

void sib3(Coder& c, LteSib3& s) {
    bool e; c.ext(e);
    bool hasSpeed = false; c.b(hasSpeed);
    c.ev(s.qHystDb, kQHyst, 16);
    if (hasSpeed) { c.skipI(0, 7); c.skipI(0, 7); c.skipI(1, 16, 1); c.skipI(1, 16, 1); c.skipI(0, 3); c.skipI(0, 3); }
    bool hasNis = s.sNonIntraSearch >= 0; c.b(hasNis);
    if (hasNis) { int v = s.sNonIntraSearch / 2; c.i(v, 0, 31); if (c.rd()) s.sNonIntraSearch = v * 2; }
    { int v = s.threshServingLow / 2; c.i(v, 0, 31); if (c.rd()) s.threshServingLow = v * 2; }
    c.i(s.priority, 0, 7);
    bool hasPmax = false, hasIs = s.sIntraSearch >= 0, hasMbw = false, hasSf = false;
    c.b(hasPmax); c.b(hasIs); c.b(hasMbw); c.b(hasSf);
    { int q = s.qRxLevMinDbm / 2; c.i(q, -70, -22); if (c.rd()) s.qRxLevMinDbm = q * 2; }
    if (hasPmax) c.skipI(-30, 33);
    if (hasIs) { int v = s.sIntraSearch / 2; c.i(v, 0, 31); if (c.rd()) s.sIntraSearch = v * 2; }
    if (hasMbw) c.skipI(0, 5);
    c.skipB(); uint32_t ncc = 0; c.u(ncc, 2);
    c.i(s.tReselSec, 0, 7);
    if (hasSf) { c.skipI(0, 3); c.skipI(0, 3); }
    c.tail(e);
}

void physRange(Coder& c, std::pair<int, int>& r) {
    static const int kR[16] = {4, 8, 12, 16, 24, 32, 48, 64, 84, 96, 128, 168, 252, 504, 0, 0};
    bool hasR = r.second > 1; c.b(hasR);
    c.i(r.first, 0, 503);
    if (hasR) c.ev(r.second, kR, 16); else if (c.rd()) r.second = 1;
}

void sib4(Coder& c, LteSib4& s) {
    bool e; c.ext(e);
    bool hn = !s.neigh.empty(), hb = !s.black.empty(), hc = false;
    c.b(hn); c.b(hb); c.b(hc);
    if (hn) {
        int n = (int)s.neigh.size(); c.i(n, 1, 16);
        if (c.rd()) s.neigh.assign((size_t)n, LteNeigh());
        for (auto& x : s.neigh) {
            bool ex; c.ext(ex);
            c.i(x.pci, 0, 503);
            int q = qOffsetIdx(x.qOffsetDb); c.i(q, 0, 30); if (c.rd()) x.qOffsetDb = qOffsetDb(q);
            c.tail(ex);
        }
    }
    if (hb) {
        int n = (int)s.black.size(); c.i(n, 1, 16);
        if (c.rd()) s.black.assign((size_t)n, {0, 1});
        for (auto& x : s.black) physRange(c, x);
    }
    if (hc) { std::pair<int, int> t; physRange(c, t); }
    c.tail(e);
}

void sib5(Coder& c, LteSib5& s) {
    bool e; c.ext(e);
    int n = (int)s.freqs.size(); c.i(n, 1, 8);
    if (c.rd()) s.freqs.assign((size_t)n, LteSib5Freq());
    for (auto& f : s.freqs) {
        bool ex; c.ext(ex);
        bool hPmax = false, hSf = false, hPrio = f.priority >= 0, hQo = f.qOffsetFreqDb != 0, hN = !f.neigh.empty(), hB = false;
        c.b(hPmax); c.b(hSf); c.b(hPrio); c.b(hQo); c.b(hN); c.b(hB);
        uint32_t ear = (uint32_t)f.earfcn; c.u(ear, 16); if (c.rd()) f.earfcn = (int)ear;
        { int q = f.qRxLevMinDbm / 2; c.i(q, -70, -22); if (c.rd()) f.qRxLevMinDbm = q * 2; }
        if (hPmax) c.skipI(-30, 33);
        c.skipI(0, 7);
        if (hSf) { c.skipI(0, 3); c.skipI(0, 3); }
        { int v = f.threshHigh / 2; c.i(v, 0, 31); if (c.rd()) f.threshHigh = v * 2; }
        { int v = f.threshLow / 2; c.i(v, 0, 31); if (c.rd()) f.threshLow = v * 2; }
        c.ev(f.bwRb, kRb, 6);
        c.skipB();
        if (hPrio) c.i(f.priority, 0, 7);
        uint32_t ncc = 0; c.u(ncc, 2);
        if (hQo) { int q = qOffsetIdx(f.qOffsetFreqDb); c.i(q, 0, 30); if (c.rd()) f.qOffsetFreqDb = qOffsetDb(q); }
        if (hN) {
            int m = (int)f.neigh.size(); c.i(m, 1, 16);
            if (c.rd()) f.neigh.assign((size_t)m, LteNeigh());
            for (auto& x : f.neigh) { c.i(x.pci, 0, 503); int q = qOffsetIdx(x.qOffsetDb); c.i(q, 0, 30); if (c.rd()) x.qOffsetDb = qOffsetDb(q); }
        }
        if (hB) { int m = 1; c.i(m, 1, 16); for (int k = 0; k < m; k++) { std::pair<int, int> t; physRange(c, t); } }
        c.tail(ex);
        if (!c.ok()) return;
    }
    c.tail(e);
}

} // namespace

bool lteParseBcch(const Bits& msg, LteBcch& out) {
    out = LteBcch();
    BitReader r(msg);
    Coder c; c.r = &r;
    if (r.bit()) return false;            // messageClassExtension
    if (r.bit()) {                         // c1: systemInformationBlockType1
        out.isSib1 = true;
        sib1(c, out.sib1);
        return r.ok() && !out.sib1.plmns.empty();
    }
    if (r.bit()) return false;             // criticalExtensionsFuture
    const bool nce = r.bit(); (void)nce;
    const int n = r.range(1, 32);
    for (int k = 0; k < n && r.ok(); k++) {
        if (r.bit()) { out.sibsSeen.push_back(12); break; }   // a later SIB (open type): stop here
        const int t = (int)r.u(4) + 2;     // 0 = SIB2 .. 9 = SIB11
        out.sibsSeen.push_back(t);
        if (t == 2) { sib2(c, out.sib2); out.has2 = r.ok(); }
        else if (t == 3) { sib3(c, out.sib3); out.has3 = r.ok(); }
        else if (t == 4) { sib4(c, out.sib4); out.has4 = r.ok(); }
        else if (t == 5) { sib5(c, out.sib5); out.has5 = r.ok(); }
        else break;                        // SIB6 and up are not read (the rest of the message cannot be found without them)
    }
    return !out.sibsSeen.empty();
}

Bits lteEncodeSib1(const LteSib1& s0) {
    LteSib1 s = s0;
    BitWriter w;
    Coder c; c.w = &w;
    w.bit(0); w.bit(1);
    sib1(c, s);
    w.padOctet();
    return w.bits();
}

Bits lteEncodeSi(const LteSib2* s2, const LteSib3* s3, const LteSib4* s4, const LteSib5* s5) {
    BitWriter w;
    Coder c; c.w = &w;
    w.bit(0); w.bit(0); w.bit(0); w.bit(0);
    const int n = (s2 ? 1 : 0) + (s3 ? 1 : 0) + (s4 ? 1 : 0) + (s5 ? 1 : 0);
    w.range(std::max(1, n), 1, 32);
    if (s2) { w.bit(0); w.u(0, 4); LteSib2 t = *s2; sib2(c, t); }
    if (s3) { w.bit(0); w.u(1, 4); LteSib3 t = *s3; sib3(c, t); }
    if (s4) { w.bit(0); w.u(2, 4); LteSib4 t = *s4; sib4(c, t); }
    if (s5) { w.bit(0); w.u(3, 4); LteSib5 t = *s5; sib5(c, t); }
    w.padOctet();
    return w.bits();
}

} // namespace dect2::cell
