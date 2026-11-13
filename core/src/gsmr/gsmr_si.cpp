// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// GSM-R System Information messages (gsmr_si.h).
#include "dect2/gsmr/gsmr_si.h"
#include <algorithm>

namespace dect2::gsmr {

namespace {

void putLai(Frame23& f, int at, const Lai& l) {
    const int m1 = (l.mcc / 100) % 10, m2 = (l.mcc / 10) % 10, m3 = l.mcc % 10;
    int n1, n2, n3 = 0xF;
    if (l.mncDigits == 3) { n1 = (l.mnc / 100) % 10; n2 = (l.mnc / 10) % 10; n3 = l.mnc % 10; }
    else { n1 = (l.mnc / 10) % 10; n2 = l.mnc % 10; }
    f[(size_t)at] = (uint8_t)((m2 << 4) | m1);
    f[(size_t)at + 1] = (uint8_t)((n3 << 4) | m3);
    f[(size_t)at + 2] = (uint8_t)((n2 << 4) | n1);
    f[(size_t)at + 3] = (uint8_t)(l.lac >> 8);
    f[(size_t)at + 4] = (uint8_t)l.lac;
}

bool getLai(const uint8_t* m, Lai& l) {
    const int m1 = m[0] & 15, m2 = m[0] >> 4, m3 = m[1] & 15, n3 = m[1] >> 4, n1 = m[2] & 15, n2 = m[2] >> 4;
    if (m1 > 9 || m2 > 9 || m3 > 9 || n1 > 9 || n2 > 9 || (n3 > 9 && n3 != 15)) return false;
    l.mcc = m1 * 100 + m2 * 10 + m3;
    if (n3 == 15) { l.mnc = n1 * 10 + n2; l.mncDigits = 2; } else { l.mnc = n1 * 100 + n2 * 10 + n3; l.mncDigits = 3; }
    l.lac = ((uint32_t)m[3] << 8) | m[4];
    return true;
}

void putSelection(Frame23& f, int at, const Si3& s) {
    f[(size_t)at] = (uint8_t)(((s.reselHystDb / 2 & 7) << 5) | (s.msTxPwrMax & 31));
    f[(size_t)at + 1] = (uint8_t)((s.acs ? 0x80 : 0) | (s.neci ? 0x40 : 0) | (s.rxLevAccessMin & 63));
}

void putRach(Frame23& f, int at, const Si3& s) {
    f[(size_t)at] = (uint8_t)(((s.maxRetrans & 3) << 6) | ((s.txInteger & 15) << 2) | (s.cellBar ? 2 : 0));
    f[(size_t)at + 1] = (uint8_t)(s.acBarred >> 8);
    f[(size_t)at + 2] = (uint8_t)s.acBarred;
}

void putList(Frame23& f, int at, const std::vector<int>& arfcns) {
    uint8_t cd[16];
    encodeFreqList(arfcns, cd);
    for (int i = 0; i < 16; i++) f[(size_t)(at + i)] = cd[i];
}

Frame23 base(int pseudoLen, int type) {
    Frame23 f;
    f.fill(0x2B);
    f[0] = (uint8_t)((pseudoLen << 2) | 1);
    f[1] = 0x06;
    f[2] = (uint8_t)type;
    return f;
}

} // namespace

Frame23 idleFrame() { Frame23 f; f.fill(0x2B); return f; }

Frame23 encodeSi1(const std::vector<int>& alloc, const Si3& s) {
    Frame23 f = base(21, kSi1);
    putList(f, 3, alloc);
    putRach(f, 19, s);
    return f;
}

Frame23 encodeSi2(const std::vector<int>& neigh, int ncc, const Si3& s) {
    Frame23 f = base(22, kSi2);
    putList(f, 3, neigh);
    f[19] = (uint8_t)ncc;
    putRach(f, 20, s);
    return f;
}

Frame23 encodeSi3(const Si3& s) {
    Frame23 f = base(18, kSi3);
    f[3] = (uint8_t)(s.cellId >> 8); f[4] = (uint8_t)s.cellId;
    putLai(f, 5, s.lai);
    f[10] = (uint8_t)((s.attach ? 0x40 : 0) | ((s.agBlks & 7) << 3) | (s.ccchConf & 7));
    f[11] = (uint8_t)((s.pagMfrms - 2) & 7);
    f[12] = (uint8_t)s.t3212;
    f[13] = (uint8_t)(((s.pwrc ? 1 : 0) << 6) | ((s.dtx & 3) << 4) | (s.rlTimeout & 15));
    putSelection(f, 14, s);
    putRach(f, 16, s);
    f[19] = 0x2B; f[20] = 0x2B; f[21] = 0x2B; f[22] = 0x2B;
    return f;
}

Frame23 encodeSi4(const Si3& s) {
    Frame23 f = base(12, kSi4);
    putLai(f, 3, s.lai);
    putSelection(f, 8, s);
    putRach(f, 10, s);
    return f;
}

Frame23 encodeSi2ter() { return base(18, kSi2ter); }
Frame23 encodePagingKind(int type) {   // L2 pseudo length, RR, the type, page mode 0, then zeros / fill: no identities
    Frame23 f; f.fill(0x2B);
    f[0] = (uint8_t)((2 << 2) | 1); f[1] = 0x06; f[2] = (uint8_t)type;
    for (int i = 3; i < 8; i++) f[(size_t)i] = 0;
    return f;
}
CcchKind ccchKind(const uint8_t* m) {   // 44.018 9.1.18 - 9.1.24 (message types 10.4)
    bool fill = true; for (int i = 0; i < 23; i++) if (m[i] != 0x2B) { fill = false; break; }
    if (fill) return CcchKind::Idle;
    if ((m[0] & 3) != 1 || (m[1] & 0x0F) != 0x06) return CcchKind::Other;
    switch (m[2]) {
    case 0x21: return CcchKind::Paging1;
    case 0x22: return CcchKind::Paging2;
    case 0x24: return CcchKind::Paging3;
    case 0x3F: return CcchKind::ImmAssign;
    case 0x39: return CcchKind::ImmAssignExt;
    case 0x3A: return CcchKind::ImmAssignReject;
    default: return CcchKind::Other;
    }
}

namespace {

inline int smod(int n, int m) { return ((n - 1) % m + m) % m + 1; }   // n modulo m in 1 .. m

struct BitReader {
    const uint8_t* d; int pos;
    int get(int n) { int v = 0; for (int i = 0; i < n; i++, pos++) v = (v << 1) | ((d[pos >> 3] >> (7 - (pos & 7))) & 1); return v; }
};

// The tree of Annex J: W(1) is the root, the children of the nodes of one level are the next 2^level numbers; of node j (counted within
// its level) the parent is the (j mod width of the level above)th node there, and the left half of the level sits below the "-half" side.
int rangeValue(const std::vector<int>& w, int k, int rootBits) {
    int res = w[(size_t)k];
    int child = k;
    while (child != 1) {
        int lvl = 0; for (int t = child; t > 1; t >>= 1) lvl++;   // child's level above the root: 1 for k = 2, 3
        const int j = child - (1 << lvl), up = 1 << (lvl - 1);
        const int parent = up + (j % up), left = (j / up) == 0;
        int plvl = 0; for (int t = parent; t > 1; t >>= 1) plvl++;
        const int m = (1 << (rootBits - plvl)) - 1, half = (m + 1) / 2;
        res = smod(w[(size_t)parent] + (left ? -half : 0) + res, m);
        child = parent;
    }
    return res;
}

} // namespace

bool decodeFreqList(const uint8_t* cd, std::vector<int>& out) {
    out.clear();
    std::vector<char> in(1024, 0);
    if ((cd[0] & 0xC0) == 0x00) {
        for (int n = 1; n <= 124; n++) if (cd[15 - ((n - 1) >> 3)] & (1 << ((n - 1) & 7))) in[(size_t)n] = 1;
    } else {
        int rootBits, nW, startBit;
        bool r1024 = false, varMap = false;
        if ((cd[0] & 0xC8) == 0x80) { rootBits = 10; nW = 16; startBit = 6; r1024 = true; }
        else if ((cd[0] & 0xCE) == 0x88) { rootBits = 9; nW = 17; startBit = 7; }
        else if ((cd[0] & 0xCE) == 0x8A) { rootBits = 8; nW = 21; startBit = 7; }
        else if ((cd[0] & 0xCE) == 0x8C) { rootBits = 7; nW = 28; startBit = 7; }
        else if ((cd[0] & 0xCE) == 0x8E) { rootBits = 0; nW = 0; startBit = 7; varMap = true; }
        else return false;
        BitReader br{cd, startBit};
        if (r1024) {
            if (cd[0] & 0x04) in[0] = 1;   // F0: ARFCN 0
            std::vector<int> w((size_t)nW + 1, 0);
            for (int k = 1; k <= nW; k++) { int lg = 0; for (int t = k; t > 1; t >>= 1) lg++; w[(size_t)k] = br.get(rootBits - lg); }
            for (int k = 1; k <= nW; k++) if (w[(size_t)k]) in[(size_t)(k == 1 ? w[1] : rangeValue(w, k, rootBits)) % 1024] = 1;
        } else if (varMap) {
            const int orig = br.get(10);
            in[(size_t)orig] = 1;
            for (int i = 1; br.pos < 128; i++) if (br.get(1)) in[(size_t)((orig + i) % 1024)] = 1;
        } else {
            const int orig = br.get(10);
            in[(size_t)orig] = 1;
            std::vector<int> w((size_t)nW + 1, 0);
            for (int k = 1; k <= nW; k++) { int lg = 0; for (int t = k; t > 1; t >>= 1) lg++; w[(size_t)k] = br.get(rootBits - lg); }
            for (int k = 1; k <= nW; k++) if (w[(size_t)k]) in[(size_t)((orig + (k == 1 ? w[1] : rangeValue(w, k, rootBits))) % 1024)] = 1;
        }
    }
    for (int n = 0; n < 1024; n++) if (in[(size_t)n]) out.push_back(n);
    return true;
}

bool encodeFreqList(const std::vector<int>& list, uint8_t* cd) {
    for (int i = 0; i < 16; i++) cd[i] = 0;
    std::vector<int> t = list;
    std::sort(t.begin(), t.end());
    t.erase(std::unique(t.begin(), t.end()), t.end());
    bool low = true;
    for (int n : t) if (n < 1 || n > 124) low = false;
    if (low) {
        for (int n : t) cd[15 - ((n - 1) >> 3)] |= (uint8_t)(1 << ((n - 1) & 7));
        return true;
    }
    cd[0] = 0x80;
    if (!t.empty() && t[0] == 0) { cd[0] |= 0x04; t.erase(t.begin()); }
    if (t.empty()) return true;
    for (int n : t) if (n > 1023) return false;
    // Search the tree: the first node is one of the channels; each further node takes one of the channels left, or none
    std::vector<int> w(17, 0);
    std::vector<char> done(t.size(), 0);
    struct Search {
        const std::vector<int>& t; std::vector<int>& w; std::vector<char>& done;
        bool go(int k, size_t left) {
            if (left == 0) return true;
            if (k > 16 || (size_t)(17 - k) < left) return false;
            int lg = 0; for (int x = k; x > 1; x >>= 1) lg++;
            const int maxW = (1 << (10 - lg)) - 1;
            for (size_t i = 0; i < t.size(); i++) {
                if (done[i]) continue;
                for (int x = 1; x <= maxW; x++) {
                    w[(size_t)k] = x;
                    if (rangeValue(w, k, 10) % 1024 != t[i]) continue;
                    done[i] = 1;
                    if (go(k + 1, left - 1)) return true;
                    done[i] = 0;
                    break;
                }
                w[(size_t)k] = 0;
            }
            w[(size_t)k] = 0;
            return go(k + 1, left);
        }
    } search{t, w, done};
    bool ok = false;
    for (size_t r = 0; r < t.size() && !ok; r++) {
        std::fill(w.begin(), w.end(), 0);
        std::fill(done.begin(), done.end(), 0);
        w[1] = t[r]; done[r] = 1;
        ok = search.go(2, t.size() - 1);
    }
    if (!ok) return false;
    int pos = 6;
    for (int k = 1; k <= 16; k++) {
        int lg = 0; for (int x = k; x > 1; x >>= 1) lg++;
        for (int b = 10 - lg - 1; b >= 0; b--, pos++) if ((w[(size_t)k] >> b) & 1) cd[pos >> 3] |= (uint8_t)(0x80 >> (pos & 7));
    }
    return true;
}

bool parseBcch(const uint8_t* m, Bcch& o) {
    o = Bcch();
    if ((m[0] & 3) != 1 || m[1] != 0x06) return false;
    o.type = m[2];
    auto list = [&](int at) {
        o.hasList = true;
        if (!decodeFreqList(m + at, o.arfcns)) o.listUnparsed = true;
    };
    auto rach = [&](int at) {
        o.si3.maxRetrans = m[at] >> 6; o.si3.txInteger = (m[at] >> 2) & 15; o.si3.cellBar = (m[at] & 2) != 0;
        o.si3.acBarred = (uint16_t)((m[at + 1] << 8) | m[at + 2]);
    };
    auto selection = [&](int at) {
        o.si3.reselHystDb = ((m[at] >> 5) & 7) * 2; o.si3.msTxPwrMax = m[at] & 31;
        o.si3.acs = (m[at + 1] & 0x80) != 0; o.si3.neci = (m[at + 1] & 0x40) != 0; o.si3.rxLevAccessMin = m[at + 1] & 63;
    };
    switch (o.type) {
    case kSi1: list(3); rach(19); o.ok = true; break;
    case kSi2: list(3); o.nccPermitted = m[19]; rach(20); o.ok = true; break;
    case kSi2bis: list(3); rach(19); o.ok = true; break;   // the neighbour cell description of SI2bis has the SI2 format (44.018 9.1.33)
    case kSi2ter: o.ok = true; break;
    case kSi2quater: case kSi13: o.ok = true; break;
    case kSi3:
        if (!getLai(m + 5, o.si3.lai)) return false;
        o.hasLai = o.hasSi3 = true;
        o.si3.cellId = ((uint32_t)m[3] << 8) | m[4];
        o.si3.attach = (m[10] & 0x40) != 0; o.si3.agBlks = (m[10] >> 3) & 7; o.si3.ccchConf = m[10] & 7;
        o.si3.pagMfrms = (m[11] & 7) + 2; o.si3.t3212 = m[12];
        o.si3.pwrc = (m[13] >> 6) & 1; o.si3.dtx = (m[13] >> 4) & 3; o.si3.rlTimeout = m[13] & 15;
        selection(14); rach(16);
        o.ok = true;
        break;
    case kSi4:
        if (!getLai(m + 3, o.si3.lai)) return false;
        o.hasLai = true;
        selection(8); rach(10);
        o.ok = true;
        break;
    default: return false;
    }
    return o.ok;
}

const char* siName(int t) {
    switch (t) {
    case kSi1: return "SI1"; case kSi2: return "SI2"; case kSi2bis: return "SI2bis"; case kSi2ter: return "SI2ter"; case kSi2quater: return "SI2quater";
    case kSi3: return "SI3"; case kSi4: return "SI4"; case kSi13: return "SI13"; default: return "?";
    }
}

} // namespace dect2::gsmr
