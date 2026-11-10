#include "dect2/isdbt/isdbt.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <mutex>
#include "dect2/common/bits.h"
#include "dect2/common/mathconst.h"

namespace dect2 {
namespace isdbt {

const int kSegmentAtPosition[kSegments] = {11, 9, 7, 5, 3, 1, 0, 2, 4, 6, 8, 10, 12};

int positionOfSegment(int seg) {
    for (int p = 0; p < kSegments; p++) if (kSegmentAtPosition[p] == seg) return p;
    return 0;
}

namespace {
const int kRateNum[5] = {1, 2, 3, 5, 7};
const int kRateDen[5] = {2, 3, 4, 6, 8};
}

int interleavingLength(int mode, int ti) {
    static const int lens[3][4] = {{0, 4, 8, 16}, {0, 2, 4, 8}, {0, 1, 2, 4}};
    if (mode < 1 || mode > 3 || ti < 0 || ti > 3) return 0;
    return lens[mode - 1][ti];
}

int timeInterleaveAdjust(int mode, int ti) {
    const int total = 95 * interleavingLength(mode, ti);
    return total == 0 ? 0 : (kSymbolsPerFrame - total % kSymbolsPerFrame) % kSymbolsPerFrame;
}

const char* guardName(int g) { static const char* n[4] = {"1/32", "1/16", "1/8", "1/4"}; return n[g & 3]; }
const char* modName(int m) { static const char* n[4] = {"DQPSK", "QPSK", "16QAM", "64QAM"}; return n[m & 3]; }
const char* rateName(int r) { static const char* n[5] = {"1/2", "2/3", "3/4", "5/6", "7/8"}; return n[r < 0 ? 0 : r > 4 ? 4 : r]; }

int packetsPerFrame(int mode, const Layer& l) {
    if (!l.used()) return 0;
    const long cells = (long)kSymbolsPerFrame * dataPerSegment(mode) * l.segments;
    const long info = cells * bitsPerCell(l.mod) * kRateNum[l.rate] / kRateDen[l.rate];
    return (int)(info / (204 * 8));   // a packet is 204 bytes in the coded stream, 1632 bits
}

double layerBitrate(const Params& p, int i) {
    return (double)packetsPerFrame(p.mode, p.layer[i]) * 188 * 8 / frameSeconds(p.mode, p.guard, p.bwMhz);
}
double totalBitrate(const Params& p) { return layerBitrate(p, 0) + layerBitrate(p, 1) + layerBitrate(p, 2); }

bool Params::valid(std::string* why) const {
    auto fail = [&](const char* m) { if (why) *why = m; return false; };
    if (mode < 1 || mode > 3) return fail("mode must be 1, 2 or 3");
    if (guard < 0 || guard > 3) return fail("guard interval");
    int n = 0;
    for (int i = 0; i < 3; i++) {
        const Layer& l = layer[i];
        if (l.segments < 0 || l.segments > kSegments) return fail("segments per layer");
        if (!l.used()) continue;
        if (l.mod < 0 || l.mod > 3 || l.rate < 0 || l.rate > 4 || l.ti < 0 || l.ti > 3) return fail("layer parameters");
        n += l.segments;
    }
    if (n < 1 || n > kSegments) return fail("the layers must use between 1 and 13 segments");
    if (tsb != 0 && tsb != 1 && tsb != 3) return fail("ISDB-TSB has one or three segments");
    if (tsb && n != tsb) return fail("the layers must fill the ISDB-TSB band");
    // layers must be consecutive: no unused layer between used ones
    if ((!layer[0].used() && (layer[1].used() || layer[2].used())) || (!layer[1].used() && layer[2].used())) return fail("unused layer before a used one");
    if (partial && layer[0].segments != 1) return fail("partial reception needs layer A with one segment");
    SegmentInfo info[kSegments];
    if (!segmentLayout(*this, info)) return fail("differential segments must come before the synchronous ones");
    return true;
}

bool segmentLayout(const Params& p, SegmentInfo info[kSegments]) {
    for (int s = 0; s < kSegments; s++) info[s] = SegmentInfo();
    int s = 0;
    bool sawSync = false;
    for (int li = 0; li < 3; li++) {
        for (int k = 0; k < p.layer[li].segments; k++, s++) {
            if (s >= kSegments) return false;
            info[s].layer = li;
            info[s].diff = p.layer[li].mod == kDqpsk;
            if (s == 0 && p.partial) { info[s].partial = true; info[s].group = 0; continue; }
            if (info[s].diff && sawSync) return false;
            if (!info[s].diff) sawSync = true;
            info[s].group = info[s].diff ? 1 : 2;
        }
    }
    int cnt[3] = {0, 0, 0}, idx[3] = {0, 0, 0};
    for (int t = 0; t < kSegments; t++) if (info[t].layer >= 0) cnt[info[t].group]++;
    for (int t = 0; t < kSegments; t++) if (info[t].layer >= 0) { info[t].index = idx[info[t].group]++; info[t].groupSize = cnt[info[t].group]; }
    return true;
}

// ---------------------------------------------------------------- carrier roles and pilots

namespace {
template <size_t N>
void mark(uint8_t* roles, const uint16_t (&t)[N][13], int col, uint8_t role, int cps) {
    for (size_t r = 0; r < N; r++) if (t[r][col] < cps) roles[t[r][col]] = role;
}
}

void segmentRoles(int mode, int seg, bool diff, int symIdx, uint8_t* roles) {
    const int cps = carriersPerSegment(mode);
    const int col = positionOfSegment(seg);
    std::memset(roles, kData, (size_t)cps);
    using namespace tables;
    if (diff) {
        roles[0] = kCP;
        if (mode == 1) { mark(roles, kDiffAc11, col, kAC1, cps); mark(roles, kDiffAc21, col, kAC2, cps); mark(roles, kDiffTmcc1, col, kTMCC, cps); }
        else if (mode == 2) { mark(roles, kDiffAc12, col, kAC1, cps); mark(roles, kDiffAc22, col, kAC2, cps); mark(roles, kDiffTmcc2, col, kTMCC, cps); }
        else { mark(roles, kDiffAc13, col, kAC1, cps); mark(roles, kDiffAc23, col, kAC2, cps); mark(roles, kDiffTmcc3, col, kTMCC, cps); }
    } else {
        const int ph = 3 * (symIdx % 4);
        for (int i = ph; i < cps; i += 12) roles[i] = kSP;
        if (mode == 1) { mark(roles, kSyncAc11, col, kAC1, cps); mark(roles, kSyncTmcc1, col, kTMCC, cps); }
        else if (mode == 2) { mark(roles, kSyncAc12, col, kAC1, cps); mark(roles, kSyncTmcc2, col, kTMCC, cps); }
        else { mark(roles, kSyncAc13, col, kAC1, cps); mark(roles, kSyncTmcc3, col, kTMCC, cps); }
    }
}

const std::vector<uint8_t>& prbsW(int mode, int seg) {
    static std::vector<uint8_t> cache[3][kSegments];
    static std::mutex mu;
    std::lock_guard<std::mutex> lk(mu);
    auto& v = cache[mode - 1][seg];
    if (v.empty()) {
        const uint8_t (*init)[11] = mode == 1 ? tables::kPrbsInit1 : mode == 2 ? tables::kPrbsInit2 : tables::kPrbsInit3;
        const uint8_t* s0 = init[positionOfSegment(seg)];
        unsigned r[11];
        for (int i = 0; i < 11; i++) r[i] = s0[i];
        const int cps = carriersPerSegment(mode);
        v.resize((size_t)cps);
        for (int i = 0; i < cps; i++) {
            v[(size_t)i] = (uint8_t)r[10];
            const unsigned fb = r[8] ^ r[10];
            for (int k = 10; k > 0; k--) r[k] = r[k - 1];
            r[0] = fb;
        }
    }
    return v;
}

// ---------------------------------------------------------------- TMCC

namespace {
// generator polynomial exponents of the (273,191) difference cyclic code
const int kGenExp[] = {82, 77, 76, 71, 67, 66, 56, 52, 48, 40, 36, 34, 24, 22, 18, 10, 4, 0};

struct Poly82 { uint64_t lo = 0, hi = 0; };   // bits 0..63 in lo, 64..81 in hi
inline void flipBit(Poly82& p, int i) { if (i < 64) p.lo ^= 1ull << i; else p.hi ^= 1ull << (i - 64); }
inline bool getBit(const Poly82& p, int i) { return i < 64 ? (p.lo >> i) & 1 : (p.hi >> (i - 64)) & 1; }

// remainder of x^e mod g(x), e >= 0
Poly82 powerMod(int e) {
    Poly82 g;
    for (int x : kGenExp) if (x < 82) flipBit(g, x);   // g(x) without its x^82 term
    Poly82 r;
    flipBit(r, 0);
    for (int k = 0; k < e; k++) {   // r *= x
        const bool top = getBit(r, 81);
        r.hi = (r.hi << 1) | (r.lo >> 63);
        r.lo <<= 1;
        r.hi &= (1ull << 18) - 1;
        if (top) { r.lo ^= g.lo; r.hi ^= g.hi; }
    }
    return r;
}

// syndrome contribution of bit position j (0 = B20 ... 183 = B203) of the 184-bit word: x^(183 - j) mod g
const Poly82* unitSyndromes() {
    static Poly82 tab[184];
    static const bool done = [] {   // built once, thread safe, and no lock on the calls after it
        Poly82 cur = powerMod(0);
        Poly82 g;
        for (int x : kGenExp) if (x < 82) flipBit(g, x);
        for (int e = 0; e < 184; e++) {   // cur = x^e mod g, stored for the bit of power e
            tab[183 - e] = cur;
            const bool top = getBit(cur, 81);
            cur.hi = (cur.hi << 1) | (cur.lo >> 63);
            cur.lo <<= 1;
            cur.hi &= (1ull << 18) - 1;
            if (top) { cur.lo ^= g.lo; cur.hi ^= g.hi; }
        }
        return true;
    }();
    (void)done;
    return tab;
}

Poly82 syndromeOf(const uint8_t* bits184) {
    const Poly82* t = unitSyndromes();
    Poly82 s;
    for (int j = 0; j < 184; j++) if (bits184[j]) { s.lo ^= t[j].lo; s.hi ^= t[j].hi; }
    return s;
}
inline bool zero(const Poly82& p) { return p.lo == 0 && p.hi == 0; }
}

void tmccParity(const uint8_t info[kTmccInfoBits], uint8_t parity[82]) {
    // the parity makes the word a multiple of g: with the parity bits zero, the syndrome of the information part is what must be cancelled
    uint8_t w[184] = {0};
    std::memcpy(w, info, kTmccInfoBits);
    Poly82 s = syndromeOf(w);
    // parity bit at power e (e = 81 .. 0) contributes x^e (e < 82, so its own remainder is a single bit): parity bit p_e = bit e of the syndrome
    for (int e = 0; e < 82; e++) parity[81 - e] = getBit(s, e) ? 1 : 0;
}

bool tmccValid(const uint8_t bits184[184]) { return zero(syndromeOf(bits184)); }

bool tmccCorrect(uint8_t bits[184], const float* reliability) {
    const Poly82 s = syndromeOf(bits);
    if (zero(s)) return true;
    const Poly82* t = unitSyndromes();
    // the least reliable positions
    constexpr int kN = 18;
    int idx[184];
    for (int i = 0; i < 184; i++) idx[i] = i;
    std::partial_sort(idx, idx + kN, idx + 184, [&](int a, int b) { return reliability[a] < reliability[b]; });
    // Up to four flips among the kN least reliable bits (about 4000 combinations), found by meeting in the middle: the syndromes of all the sets of
    // at most two of them go in a small hash table, and for each set B the table is asked for the set A whose syndrome is s ^ B. The receiver calls
    // this for every candidate while it looks for the frame, so it has to be cheap.
    struct Sub { Poly82 syn; int8_t a, b; };   // the positions in idx (-1: none)
    constexpr int kSubs = 1 + kN + kN * (kN - 1) / 2;
    Sub sub[kSubs];
    int ns = 0;
    sub[ns++] = Sub{Poly82(), -1, -1};
    for (int i = 0; i < kN; i++) sub[ns++] = Sub{t[idx[i]], (int8_t)i, -1};
    for (int i = 0; i < kN; i++)
        for (int j = i + 1; j < kN; j++) {
            Poly82 p = t[idx[i]];
            p.lo ^= t[idx[j]].lo; p.hi ^= t[idx[j]].hi;
            sub[ns++] = Sub{p, (int8_t)i, (int8_t)j};
        }
    constexpr int kH = 512;
    int16_t slot[kH];
    for (int i = 0; i < kH; i++) slot[i] = -1;
    auto hash = [](const Poly82& p) { return (unsigned)(((p.lo ^ (p.hi * 0x9E3779B97F4A7C15ull)) * 0xD6E8FEB86659FD93ull) >> 55); };
    for (int i = 0; i < ns; i++) {   // the smaller sets come first and stay when two have the same syndrome
        unsigned h = hash(sub[i].syn);
        bool dup = false;
        while (slot[h] >= 0) { if (sub[slot[h]].syn.lo == sub[i].syn.lo && sub[slot[h]].syn.hi == sub[i].syn.hi) { dup = true; break; } h = (h + 1) & (kH - 1); }
        if (!dup) slot[h] = (int16_t)i;
    }
    auto count = [](const Sub& x) { return (x.a >= 0) + (x.b >= 0); };
    int best = -1, bestA = -1, bestN = 99;
    for (int i = 0; i < ns; i++) {
        Poly82 key = s;
        key.lo ^= sub[i].syn.lo; key.hi ^= sub[i].syn.hi;
        unsigned h = hash(key);
        while (slot[h] >= 0 && !(sub[slot[h]].syn.lo == key.lo && sub[slot[h]].syn.hi == key.hi)) h = (h + 1) & (kH - 1);
        if (slot[h] < 0) continue;
        const Sub& A = sub[slot[h]];
        const Sub& B = sub[i];
        const bool overlap = (A.a >= 0 && (A.a == B.a || A.a == B.b)) || (A.b >= 0 && (A.b == B.a || A.b == B.b));
        const int n = count(A) + count(B);
        if (!overlap && n > 0 && n < bestN) { bestN = n; best = i; bestA = slot[h]; }
    }
    if (best < 0) return false;
    for (int k : {sub[best].a, sub[best].b, sub[bestA].a, sub[bestA].b}) if (k >= 0) bits[idx[k]] ^= 1;
    return true;
}

namespace {
void putBits(uint8_t* dst, int& pos, unsigned v, int n) { bits::putMsbAdv(dst, pos, v, n); }
unsigned getBits(const uint8_t* src, int& pos, int n) { return (unsigned)bits::getMsbAdvRaw(src, pos, n); }
}

void tmccPack(const Tmcc& t, uint8_t bits[kTmccInfoBits]) {
    int pos = 0;
    putBits(bits, pos, t.sysId, 2);
    putBits(bits, pos, t.switching, 4);
    putBits(bits, pos, t.emergency ? 1 : 0, 1);
    putBits(bits, pos, t.partial ? 1 : 0, 1);
    for (int i = 0; i < 3; i++) { putBits(bits, pos, t.cur[i].mod, 3); putBits(bits, pos, t.cur[i].rate, 3); putBits(bits, pos, t.cur[i].ti, 3); putBits(bits, pos, t.cur[i].segments, 4); }
    putBits(bits, pos, t.nextPartial ? 1 : 0, 1);
    for (int i = 0; i < 3; i++) { putBits(bits, pos, t.next[i].mod, 3); putBits(bits, pos, t.next[i].rate, 3); putBits(bits, pos, t.next[i].ti, 3); putBits(bits, pos, t.next[i].segments, 4); }
    putBits(bits, pos, 7, 3);       // phase shift correction for connected segment transmission
    putBits(bits, pos, 0xFFF, 12);  // reserved
}

bool tmccUnpack(const uint8_t bits[kTmccInfoBits], Tmcc& t) {
    int pos = 0;
    t.sysId = (int)getBits(bits, pos, 2);
    t.switching = (int)getBits(bits, pos, 4);
    t.emergency = getBits(bits, pos, 1) != 0;
    t.partial = getBits(bits, pos, 1) != 0;
    for (int i = 0; i < 3; i++) { t.cur[i].mod = (int)getBits(bits, pos, 3); t.cur[i].rate = (int)getBits(bits, pos, 3); t.cur[i].ti = (int)getBits(bits, pos, 3); t.cur[i].segments = (int)getBits(bits, pos, 4); }
    t.nextPartial = getBits(bits, pos, 1) != 0;
    for (int i = 0; i < 3; i++) { t.next[i].mod = (int)getBits(bits, pos, 3); t.next[i].rate = (int)getBits(bits, pos, 3); t.next[i].ti = (int)getBits(bits, pos, 3); t.next[i].segments = (int)getBits(bits, pos, 4); }
    return true;
}

void tmccFrameBits(const uint8_t info[kTmccInfoBits], bool evenFrame, bool diffSegment, uint8_t bits[kSymbolsPerFrame]) {
    std::memset(bits, 0, kSymbolsPerFrame);
    for (int i = 0; i < 16; i++) bits[1 + i] = evenFrame ? kTmccSync0[i] : (uint8_t)(kTmccSync0[i] ^ 1);
    for (int i = 0; i < 3; i++) bits[17 + i] = diffSegment ? 1 : 0;
    std::memcpy(bits + 20, info, kTmccInfoBits);
    uint8_t par[82];
    tmccParity(info, par);
    std::memcpy(bits + 122, par, 82);
}

std::vector<int> tmccCarrierList(int mode, int pos, bool diff) {
    std::vector<int> v;
    const int cps = carriersPerSegment(mode);
    using namespace tables;
    auto add = [&](const uint16_t (*t)[13], int rows) { for (int r = 0; r < rows; r++) v.push_back(pos * cps + t[r][pos]); };
    if (diff) {
        if (mode == 1) add(kDiffTmcc1, 5); else if (mode == 2) add(kDiffTmcc2, 10); else add(kDiffTmcc3, 20);
    } else {
        if (mode == 1) add(kSyncTmcc1, 1); else if (mode == 2) add(kSyncTmcc2, 2); else add(kSyncTmcc3, 4);
    }
    return v;
}

LayerInfo toLayerInfo(const Layer& l) {
    LayerInfo li;
    if (!l.used()) return li;
    li.mod = l.mod; li.rate = l.rate; li.ti = l.ti; li.segments = l.segments;
    return li;
}

bool fromLayerInfo(const LayerInfo& li, Layer& l) {
    l = Layer();
    if (li.segments == 15 && li.mod == 7) return true;     // unused
    if (li.mod > 3 || li.rate > 4 || li.ti > 3 || li.segments < 1 || li.segments > 13) return false;
    l.segments = li.segments; l.mod = li.mod; l.rate = li.rate; l.ti = li.ti;
    return true;
}

Tmcc tmccFromParams(const Params& p) {
    Tmcc t;
    t.sysId = p.tsb ? 1 : 0;
    t.partial = p.partial;
    for (int i = 0; i < 3; i++) t.cur[i] = toLayerInfo(p.layer[i]);
    return t;
}

bool paramsFromTmcc(const Tmcc& t, Params& p) {
    Params q = p;
    for (int i = 0; i < 3; i++) if (!fromLayerInfo(t.cur[i], q.layer[i])) return false;
    q.partial = t.partial;
    if (q.tsb && q.totalSegments() != q.tsb) {
        // a narrow receiver inside a wider signal: only the one-segment layer at the centre can be taken out on its own
        if (q.tsb != 1 || !q.partial || q.layer[0].segments != 1) return false;
        q.layer[1] = Layer(); q.layer[2] = Layer();
    }
    if (!q.valid()) return false;
    p = q;
    return true;
}

float edgeCarrierValue(int mode, int tsb) {
    if (!tsb) return lastCarrierValue(mode);
    return pilotValue(prbsW(mode, kSegmentAtPosition[lastPosition(tsb) + 1])[0]);
}

// ---------------------------------------------------------------- constellations

namespace {
// level of an axis for the label bits (first bit most significant), as in Figures 3-15, 3-17 and 3-19
int axisLevel(int mod, unsigned bits) {
    if (mod == kQpsk || mod == kDqpsk) return bits ? -1 : 1;
    if (mod == k16Qam) { static const int lv[4] = {3, 1, -3, -1}; return lv[bits & 3]; }        // 00 +3, 01 +1, 10 -3, 11 -1
    static const int lv[8] = {7, 5, 1, 3, -7, -5, -1, -3};                                       // 000 +7, 001 +5, 010 +1, 011 +3, 100 -7, 101 -5, 110 -1, 111 -3
    return lv[bits & 7];
}
float norm(int mod) { return mod == k64Qam ? 1.f / std::sqrt(42.f) : mod == k16Qam ? 1.f / std::sqrt(10.f) : 1.f / std::sqrt(2.f); }
}

cf32 mapLabel(int mod, unsigned label) {
    const int m = bitsPerCell(mod);
    // b0 b1 b2 ...: I takes b0, b2, b4; Q takes b1, b3, b5
    unsigned ib = 0, qb = 0;
    for (int i = 0; i < m; i++) {
        const unsigned bit = (label >> (m - 1 - i)) & 1;
        if (i % 2 == 0) ib = (ib << 1) | bit; else qb = (qb << 1) | bit;
    }
    const float n = norm(mod);
    return cf32((float)axisLevel(mod, ib) * n, (float)axisLevel(mod, qb) * n);
}

namespace {
inline float sq(float v) { return v * v; }
inline float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
}

void demapCell(int mod, cf32 z, float n0, float* llr) {
    // closed forms of the max-log bit decisions per axis (levels in units of the constellation spacing; see axisLevel for the labelling)
    const float inv = 1.f / std::max(n0, 1e-12f);
    if (mod == kQpsk || mod == kDqpsk) {
        const float n = 1.f / std::sqrt(2.f);
        // levels +-n: d(bit 0 at +) - d(bit 1 at -) ... (x - n)^2 - (x + n)^2 = -4 n x, so llr = (d1 - d0) / n0 = 4 n x / n0
        llr[0] = 4.f * n * z.real() * inv;
        llr[1] = 4.f * n * z.imag() * inv;
        return;
    }
    if (mod == k16Qam) {
        const float n = 1.f / std::sqrt(10.f), n2 = n * n;
        for (int axis = 0; axis < 2; axis++) {
            const float u = (axis == 0 ? z.real() : z.imag()) / n;
            // first bit: 0 for the positive levels {1, 3}
            const float dpos = std::min(sq(u - 1.f), sq(u - 3.f)), dneg = std::min(sq(u + 1.f), sq(u + 3.f));
            // second bit: 0 for the outer levels +-3, 1 for the inner levels +-1
            const float a = std::fabs(u);
            const float dout = sq(a - 3.f), din = sq(a - 1.f);
            llr[axis] = (dneg - dpos) * n2 * inv;
            llr[2 + axis] = (din - dout) * n2 * inv;
        }
        return;
    }
    const float n = 1.f / std::sqrt(42.f), n2 = n * n;
    for (int axis = 0; axis < 2; axis++) {
        const float u = (axis == 0 ? z.real() : z.imag()) / n;
        const float a = std::fabs(u);
        // first bit: 0 for the positive levels {1, 3, 5, 7}
        const float lp = clampf(2.f * std::floor((u - 1.f) * 0.5f + 0.5f) + 1.f, 1.f, 7.f), ln = clampf(2.f * std::floor((-u - 1.f) * 0.5f + 0.5f) + 1.f, 1.f, 7.f);
        const float dpos = sq(u - lp), dneg = sq(u + ln);
        // second bit: 0 for |level| in {5, 7}, 1 for {1, 3}
        const float d0b = std::min(sq(a - 5.f), sq(a - 7.f)), d1b = std::min(sq(a - 1.f), sq(a - 3.f));
        // third bit: 0 for |level| in {1, 7}, 1 for {3, 5}
        const float d0c = std::min(sq(a - 1.f), sq(a - 7.f)), d1c = std::min(sq(a - 3.f), sq(a - 5.f));
        llr[axis] = (dneg - dpos) * n2 * inv;
        llr[2 + axis] = (d1b - d0b) * n2 * inv;
        llr[4 + axis] = (d1c - d0c) * n2 * inv;
    }
}

namespace {
// The same max-log decisions as demapCell, with the constants folded and no libm call (the level of the 64-QAM axis is a clamped integer cast).
template <int Mod>
inline void demapAxis(float x, float inv, float* llr, int stride) {
    if constexpr (Mod == kQpsk) {
        llr[0] = 4.f * dect2::math::k1OverSqrt2f * x * inv;
    } else if constexpr (Mod == k16Qam) {
        constexpr float n = 0.31622777f, n2 = 0.1f, rn = 1.f / n;
        const float u = x * rn;
        const float dpos = std::min(sq(u - 1.f), sq(u - 3.f)), dneg = std::min(sq(u + 1.f), sq(u + 3.f));
        const float a = std::fabs(u);
        llr[0] = (dneg - dpos) * n2 * inv;
        llr[stride] = (sq(a - 1.f) - sq(a - 3.f)) * n2 * inv;
    } else {
        constexpr float n = 0.15430335f, n2 = 1.f / 42.f, rn = 1.f / n;
        const float u = x * rn, a = std::fabs(u);
        // nearest positive level of {1, 3, 5, 7} on each side: 2 * floor(v / 2) + 1 clamped, and for v < 0 the clamp gives 1 as a truncation does
        auto lvl = [](float v) { v *= 0.5f; v = !(v >= 0.f) ? 0.f : v > 3.f ? 3.f : v; return 2.f * (float)(int)v + 1.f; };
        const float dpos = sq(u - lvl(u)), dneg = sq(u + lvl(-u));
        const float d0b = std::min(sq(a - 5.f), sq(a - 7.f)), d1b = std::min(sq(a - 1.f), sq(a - 3.f));
        const float d0c = std::min(sq(a - 1.f), sq(a - 7.f)), d1c = std::min(sq(a - 3.f), sq(a - 5.f));
        llr[0] = (dneg - dpos) * n2 * inv;
        llr[stride] = (d1b - d0b) * n2 * inv;
        llr[2 * stride] = (d1c - d0c) * n2 * inv;
    }
}
template <int Mod>
void demapRun(const cf32* z, const float* n0, int nc, float* llr) {
    constexpr int m = Mod == kQpsk ? 2 : Mod == k16Qam ? 4 : 6;
    for (int c = 0; c < nc; c++) {
        const float inv = 1.f / std::max(n0[c], 1e-12f);
        float* o = llr + (size_t)c * m;
        demapAxis<Mod>(z[c].real(), inv, o, 2);
        demapAxis<Mod>(z[c].imag(), inv, o + 1, 2);
    }
}
}

void demapCells(int mod, const cf32* z, const float* n0, int nc, float* llr) {
    if (mod == kQpsk || mod == kDqpsk) demapRun<kQpsk>(z, n0, nc, llr);
    else if (mod == k16Qam) demapRun<k16Qam>(z, n0, nc, llr);
    else demapRun<k64Qam>(z, n0, nc, llr);
}

void demapCellGeneric(int mod, cf32 z, float n0, float* llr) {
    const int m = bitsPerCell(mod);
    const int half = m / 2;
    const float n = norm(mod);
    const float inv = 1.f / std::max(n0, 1e-12f);
    for (int axis = 0; axis < 2; axis++) {
        const float x = axis == 0 ? z.real() : z.imag();
        float d0[3], d1[3];
        for (int b = 0; b < half; b++) d0[b] = d1[b] = 1e30f;
        for (unsigned bits = 0; bits < (1u << half); bits++) {
            const float lvl = (float)axisLevel(mod, bits) * n;
            const float d = (x - lvl) * (x - lvl);
            for (int b = 0; b < half; b++) {
                const unsigned bit = (bits >> (half - 1 - b)) & 1;
                if (bit) d1[b] = std::min(d1[b], d); else d0[b] = std::min(d0[b], d);
            }
        }
        for (int b = 0; b < half; b++) llr[2 * b + axis] = (d1[b] - d0[b]) * inv;
    }
}

} // namespace isdbt
} // namespace dect2
