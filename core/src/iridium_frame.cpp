// Iridium frame layer: classification, BCH, de-interleaving and field decoding of one burst, pager message assembly, and the
// builders for the test signal. Written from the field layouts of iridium-toolkit (bitsparser.py, bch.py, FORMAT.md) and the
// symbol mapping of gr-iridium (iridium_qpsk_demod_impl.cc); the code is our own.
#include "dect2/iridium_frame.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <map>
#include <random>

namespace dect2 {
namespace {

typedef std::vector<uint8_t> Bits;

// ---- bits as numbers ----
uint32_t rd(const Bits& b, size_t off, int n) {
    uint32_t v = 0;
    for (int i = 0; i < n; i++) v = (v << 1) | (off + i < b.size() ? (b[off + i] & 1u) : 0u);
    return v;
}
void wr(Bits& b, uint32_t v, int n) {
    for (int i = n - 1; i >= 0; i--) b.push_back((v >> i) & 1u);
}
Bits fromStr(const char* s) {
    Bits b;
    for (; *s; s++) b.push_back(*s == '1');
    return b;
}
int popc(uint32_t v) {
    int n = 0;
    for (; v; v &= v - 1) n++;
    return n;
}
int bitDiff(const Bits& a, size_t off, const Bits& pat) {
    int n = 0;
    for (size_t i = 0; i < pat.size(); i++) n += ((off + i < a.size() ? a[off + i] : 0) != pat[i]);
    return n;
}
// Both bits of each symbol swapped: RAW order <-> the toolkit's own order (its symbol_reverse).
Bits swapPairs(const Bits& b) {
    Bits o(b.begin(), b.end() - (b.size() & 1));
    for (size_t i = 0; i + 1 < o.size(); i += 2) std::swap(o[i], o[i + 1]);
    return o;
}
std::string hexOf(const std::vector<uint8_t>& v, size_t n = std::string::npos) {
    std::string s;
    char t[4];
    for (size_t i = 0; i < v.size() && i < n; i++) { snprintf(t, sizeof t, "%02x", v[i]); s += t; }
    return s;
}

// ---- BCH, GF(2) ----
int bitLen(uint32_t v) { int n = 0; while (v) { n++; v >>= 1; } return n; }
uint32_t gfMod(uint32_t poly, uint32_t num) {
    int pl = bitLen(poly);
    for (int s = bitLen(num) - pl; s >= 0; s--)
        if ((num >> (s + pl - 1)) & 1) num ^= poly << s;
    return num;
}
// bch.py: codewords of n bits, up to maxErr errors corrected by a syndrome table (poly 1897 and 1207: 31 bits, 21 data; 3545: 31 bits,
// 20 data; 465: 14 bits, 6 data; 41: 26 bits, 21 data; 29: 7 bits, 3 data).
struct Bch {
    uint32_t poly;
    int n, deg;
    std::vector<int32_t> pat;
    std::vector<uint8_t> cnt;
    Bch(uint32_t p, int nbits, int maxErr) : poly(p), n(nbits), deg(bitLen(p) - 1), pat(size_t(1) << (bitLen(p) - 1), -1), cnt(pat.size(), 0) {
        for (int a = 0; a < n; a++) {
            uint32_t r = gfMod(poly, 1u << a);
            pat[r] = int32_t(1u << a); cnt[r] = 1;
        }
        if (maxErr >= 2)
            for (int a = 0; a < n; a++)
                for (int b = a + 1; b < n; b++) {
                    uint32_t v = (1u << a) | (1u << b), r = gfMod(poly, v);
                    if (pat[r] < 0) { pat[r] = int32_t(v); cnt[r] = 2; }
                }
    }
    uint32_t encode(uint32_t data) const { uint32_t s = data << deg; return s | gfMod(poly, s); }
    // Corrects cw (nbits wide) in place; returns the number of corrected bits, -1 when it cannot.
    int repair(uint32_t& cw, int nbits) const {
        uint32_t r = gfMod(poly, cw);
        if (!r) return 0;
        if (pat[r] < 0 || (uint32_t(pat[r]) >> nbits)) return -1;
        cw ^= uint32_t(pat[r]);
        return cnt[r];
    }
};
const Bch& bch29() { static const Bch b(29, 7, 1); return b; }
const Bch& bch465() { static const Bch b(465, 14, 2); return b; }
const Bch& bch41() { static const Bch b(41, 26, 1); return b; }
const Bch& bch1897() { static const Bch b(1897, 31, 2); return b; }   // messaging
const Bch& bch1207() { static const Bch b(1207, 31, 2); return b; }   // ring alert, broadcast
const Bch& bch3545() { static const Bch b(3545, 31, 2); return b; }   // data (ACCH)

// One 32-bit block (31 BCH bits and an even-parity bit): corrects cw, returns the number of wrong bits found (-1: hopeless).
// The toolkit drops a block that needed a correction and then shows wrong parity. Here one correction with a wrong parity bit is
// taken as two errors (the parity bit itself is the second); a wrong parity after two corrections is three errors, which the code
// cannot fix, and is refused.
int block32(uint32_t& cw, int parityBit, const Bch& b) {
    int e = b.repair(cw, 31);
    if (e < 0) return -1;
    if (((popc(cw) + parityBit) & 1) == 0) return e;
    return e >= 2 ? -1 : e + 1;
}

// ---- de-interleaving (bitsparser.py de_interleave*, working on the toolkit's order) ----
void deInt2(const uint8_t* g, size_t len, Bits& odd, Bits& even) {
    long S = long(len / 2);
    odd.clear(); even.clear();
    for (long z = S - 2; z >= 0; z -= 2) { even.push_back(g[2 * z + 1]); even.push_back(g[2 * z]); }
    for (long z = S - 1; z >= 0; z -= 2) { odd.push_back(g[2 * z + 1]); odd.push_back(g[2 * z]); }
}
void deInt3(const uint8_t* g, size_t len, Bits& first, Bits& second, Bits& third) {
    long S = long(len / 2);
    first.clear(); second.clear(); third.clear();
    for (long z = S - 3; z >= 0; z -= 3) { third.push_back(g[2 * z + 1]); third.push_back(g[2 * z]); }
    for (long z = S - 2; z >= 0; z -= 3) { second.push_back(g[2 * z + 1]); second.push_back(g[2 * z]); }
    for (long z = S - 1; z >= 0; z -= 3) { first.push_back(g[2 * z + 1]); first.push_back(g[2 * z]); }
}
void inInt2(const Bits& odd, const Bits& even, Bits& out) {
    long S = long((odd.size() + even.size()) / 2);
    out.assign(size_t(2 * S), 0);
    size_t k = 0;
    for (long z = S - 2; z >= 0; z -= 2, k += 2) { out[2 * z + 1] = even[k]; out[2 * z] = even[k + 1]; }
    k = 0;
    for (long z = S - 1; z >= 0; z -= 2, k += 2) { out[2 * z + 1] = odd[k]; out[2 * z] = odd[k + 1]; }
}
void inInt3(const Bits& first, const Bits& second, const Bits& third, Bits& out) {
    long S = long((first.size() + second.size() + third.size()) / 2);
    out.assign(size_t(2 * S), 0);
    size_t k = 0;
    for (long z = S - 3; z >= 0; z -= 3, k += 2) { out[2 * z + 1] = third[k]; out[2 * z] = third[k + 1]; }
    k = 0;
    for (long z = S - 2; z >= 0; z -= 3, k += 2) { out[2 * z + 1] = second[k]; out[2 * z] = second[k + 1]; }
    k = 0;
    for (long z = S - 1; z >= 0; z -= 3, k += 2) { out[2 * z + 1] = first[k]; out[2 * z] = first[k + 1]; }
}
// de_interleave_lcw: 1-based source positions of the 46 LCW bits
const int kLcwTbl[46] = { 40, 39, 36, 35, 32, 31, 28, 27, 24, 23, 20, 19, 16, 15, 12, 11, 8, 7, 4, 3,
                          41, 38, 37, 34, 33, 30, 29, 26, 25, 22, 21, 18, 17, 14, 13, 10, 9, 6, 5, 2,
                          1, 46, 45, 44, 43, 42 };
Bits lcwDeInt(const Bits& d) {
    Bits o(46);
    for (int i = 0; i < 46; i++) o[i] = d[kLcwTbl[i] - 1];
    return o;
}
Bits lcwInt(const Bits& seq) {
    Bits o(46);
    for (int i = 0; i < 46; i++) o[kLcwTbl[i] - 1] = seq[i];
    return o;
}

const Bits& kHdrMsg() { static const Bits b = fromStr("00110011111100110011001111110011"); return b; }   // 0x9669 in BPSK
const Bits& kHdrTl() { static const Bits b = [] { Bits x(96, 0); x[0] = x[1] = 1; return x; }(); return b; }
const Bits& kFillA() { static const Bits b = fromStr("10100010011100111011111101101101"); return b; }
const Bits& kFillB() { static const Bits b = fromStr("01010100010001011100001011100110"); return b; }

// One 32-bit block: 31 BCH bits and an even-parity bit.
void putBlock32(Bits& out, uint32_t data21, const Bch& b) {
    uint32_t cw = b.encode(data21);
    wr(out, cw, 31);
    out.push_back(popc(cw) & 1);
}

// ---- time ----
const double kEra[3] = { 1173325821.0, 1399818235.0, 1739556857.0 };
const double kEraStart2 = 1425405600.0, kEraStart3 = 1768414080.0;   // 2015-03-03 18:00 and 2026-01-14 18:08 UTC
double nowUnix() {
    return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
}

// ---- checksums ----
uint16_t crc16ccitt(const std::vector<uint8_t>& v) {   // crc-ccitt-false: init 0xffff, no reflection, no xor
    uint16_t c = 0xffff;
    for (uint8_t x : v) {
        c ^= uint16_t(x) << 8;
        for (int k = 0; k < 8; k++) c = (c & 0x8000) ? uint16_t((c << 1) ^ 0x1021) : uint16_t(c << 1);
    }
    return c;
}
// iip_crc24 of bitsparser.py: crcmod poly 0x1BBA1B5, reflected, register start 0xffffff, final xor 0x0c91b6 (zero when the frame is good).
uint32_t crc24ip(const std::vector<uint8_t>& v) {
    uint32_t reg = 0xffffff, rp = 0;
    for (int i = 0; i < 24; i++) if ((0xBBA1B5u >> i) & 1) rp |= 1u << (23 - i);
    for (uint8_t x : v) {
        reg ^= x;
        for (int k = 0; k < 8; k++) reg = (reg & 1) ? (reg >> 1) ^ rp : reg >> 1;
    }
    return reg ^ 0x0c91b6;
}
int msgChecksum7(const std::string& s) {   // inverted 7-bit sum (messagechecksum)
    int c = 0;
    for (unsigned char x : s) c = (c + x) % 128;
    return (~c) & 127;
}

// ---- the classification of a burst ----
enum class Kind { None, MS, TL, BC, LW, RA, AQ };

struct Lcw {
    int ft = 0, err = 0;
    uint32_t l2 = 0, l3 = 0;
    bool ok = false;
};
Lcw readLcw(const Bits& d) {
    Lcw l;
    Bits s = lcwDeInt(d);
    uint32_t c1 = rd(s, 0, 7), c2 = rd(s, 7, 13), c3 = rd(s, 20, 26);
    int e1 = bch29().repair(c1, 7);
    uint32_t a = c2 << 1, b = (c2 << 1) | 1;
    int e2a = bch465().repair(a, 14), e2b = bch465().repair(b, 14);
    int e2 = e2a;
    uint32_t w2 = a;
    if ((e2b >= 0 && e2b <= e2a) || e2a < 0) { e2 = e2b; w2 = b; }
    int e3 = bch41().repair(c3, 26);
    l.ft = int(c1 >> 4);
    l.l2 = w2 >> 8;
    l.l3 = c3 >> 5;
    l.ok = e1 >= 0 && e2 >= 0 && e3 >= 0;
    l.err = l.ok ? e1 + e2 + e3 : -1;
    return l;
}

Kind classify(const Bits& d, bool uplink, int& ecLcw) {
    size_t N = d.size();
    ecLcw = 0;
    // strict pass: no correction needed in the first code words
    if (N >= 32 && bitDiff(d, 0, kHdrMsg()) == 0) return Kind::MS;
    if (N >= 96 && bitDiff(d, 0, kHdrTl()) == 0) return Kind::TL;
    if (N > 70 && gfMod(29, rd(d, 0, 6)) == 0) {
        Bits o, e;
        deInt2(d.data() + 6, 64, o, e);
        if (gfMod(1207, rd(o, 0, 31)) == 0 && gfMod(1207, rd(e, 0, 31)) == 0) return Kind::BC;
    }
    if (N > 64) {
        Bits s = lcwDeInt(d);
        if (gfMod(29, rd(s, 0, 7)) == 0 && gfMod(41, rd(s, 20, 26)) == 0) {
            uint32_t c2 = rd(s, 7, 13);
            if (gfMod(465, c2 << 1) == 0 || gfMod(465, (c2 << 1) | 1) == 0) return Kind::LW;
        }
    }
    if (N >= 96) {
        Bits a, b, c;
        deInt3(d.data(), 96, a, b, c);
        if (gfMod(1207, rd(a, 0, 31)) == 0 && gfMod(1207, rd(b, 0, 31)) == 0 && gfMod(1207, rd(c, 0, 31)) == 0) return Kind::RA;
    }
    if (uplink && N >= 52 && N < 100) return Kind::AQ;
    // second pass with corrections (the toolkit's --harder). Every frame type is tried; each match is scored by the check bits
    // it satisfied minus about 5 bits per correction (what a chance match would be worth), and the best score wins if it is
    // high enough, so noise is not taken for a frame.
    int bestCost = 1000, bestScore = 9;
    Kind best = Kind::None;
    auto offer = [&](Kind k, int evidence, int cost) {
        int score = evidence - 5 * cost;
        if (score > bestScore) { bestScore = score; bestCost = cost; best = k; }
    };
    if (N >= 70 && !uplink) {
        uint32_t h = rd(d, 0, 6);
        int e1 = bch29().repair(h, 6);
        Bits o, e;
        deInt2(d.data() + 6, 64, o, e);
        uint32_t c1 = rd(o, 0, 31), c2 = rd(e, 0, 31);
        int e2 = block32(c1, o[31], bch1207()), e3 = block32(c2, e[31], bch1207());
        if (e1 >= 0 && e2 >= 0 && e3 >= 0 && e1 + e2 + e3 <= 3) offer(Kind::BC, 4 + 22, e1 + e2 + e3);
    }
    if (N >= 96 && !uplink) {
        Bits a, b, c;
        deInt3(d.data(), 96, a, b, c);
        uint32_t x[3] = { rd(a, 0, 31), rd(b, 0, 31), rd(c, 0, 31) };
        int par[3] = { a[31], b[31], c[31] };
        int tot = 0;
        bool good = true;
        for (int i = 0; i < 3 && good; i++) {
            int e = block32(x[i], par[i], bch1207());
            if (e < 0) good = false; else tot += e;
        }
        if (good && tot <= 4) offer(Kind::RA, 33, tot);
    }
    if (N >= 64) {
        Lcw l = readLcw(d);
        if (l.ok && l.err <= 2) offer(Kind::LW, 17, l.err);
    }
    if (N >= 96 + 8 * 8 * 12 && !uplink && bitDiff(d, 0, kHdrTl()) < 4) offer(Kind::TL, 96, bitDiff(d, 0, kHdrTl()));
    if (N >= 32 && !uplink && bitDiff(d, 0, kHdrMsg()) < 2) offer(Kind::MS, 32, bitDiff(d, 0, kHdrMsg()));
    ecLcw = bestCost < 1000 ? bestCost : 0;
    return best;
}

// ---- error correction of 32-bit blocks ----
struct Ecc {
    Bits data;
    int fixed = 0;
    bool cut = false, err = false;
};
Ecc eccBlocks(const std::vector<Bits>& blocks, const Bch& bch) {
    Ecc r;
    for (const Bits& b : blocks) {
        uint32_t cw = rd(b, 0, 31);
        int e = block32(cw, b[31], bch);
        if (e < 0) { r.cut = true; break; }
        r.fixed += e;
        wr(r.data, cw >> 10, 21);
    }
    return r;
}
bool isFill(const Bits& a, const Bits& b) {
    int x = bitDiff(a, 0, kFillA()), y = bitDiff(b, 0, kFillB());
    return x <= 2 && y <= 2;
}
std::string printable(const std::string& s) {
    std::string o;
    for (unsigned char c : s) o += (c == '\n' || (c >= 32 && c < 127)) ? char(c) : '?';
    return o;
}

// ---- IRA ----
void decodeRa(const Bits& d, IridiumFrame& f, bool guessed) {
    f.type = IridiumType::IRA; f.typeName = "IRA";
    if (d.size() < 96) return;
    std::vector<Bits> blk(3);
    deInt3(d.data(), 96, blk[0], blk[1], blk[2]);
    for (size_t p = 96; p + 64 <= d.size(); p += 64) {
        Bits o, e;
        deInt2(d.data() + p, 64, o, e);
        blk.push_back(o); blk.push_back(e);
    }
    while (blk.size() >= 5 && isFill(blk[blk.size() - 2], blk.back())) { blk.pop_back(); blk.pop_back(); }
    Ecc ec = eccBlocks(blk, bch1207());
    f.corrected = ec.fixed;
    if (ec.err || ec.data.size() < 63) return;
    const Bits& b = ec.data;
    f.satId = int(rd(b, 0, 7));
    f.beamId = int(rd(b, 7, 6));
    int pos[3];
    for (int i = 0; i < 3; i++) { pos[i] = int(rd(b, 13 + 12 * i, 12)); if (pos[i] >= 2048) pos[i] -= 4096; }
    double x = pos[0], y = pos[1], z = pos[2];
    f.hasPosition = true;
    f.lat = std::atan2(z, std::sqrt(x * x + y * y)) * 180 / M_PI;
    f.lon = std::atan2(y, x) * 180 / M_PI;
    f.altKm = std::sqrt(x * x + y * y + z * z) * 4 - 6378 + 23;
    size_t pages = (b.size() - 63) / 42;
    int n = 0;
    for (size_t i = 0; i < pages; i++) {
        bool allOnes = true;
        for (int k = 0; k < 42; k++) if (!b[63 + 42 * i + k]) { allOnes = false; break; }
        if (allOnes) break;
        n++;
    }
    f.paged = n;
    f.ok = true;
    // a frame that only matched after corrections must also look like a ring alert: interval 48, a position near the Earth
    if (guessed) {
        double r = std::sqrt(x * x + y * y + z * z) * 4;
        if (rd(b, 49, 7) != 48 || r < 6300 || r > 8200) f.ok = false;
    }
}

// ---- IBC ----
void decodeBc(const Bits& d, IridiumFrame& f, double ref, bool guessed) {
    f.type = IridiumType::IBC; f.typeName = "IBC";
    uint32_t h = rd(d, 0, 6);
    int e = bch29().repair(h, 6);
    if (e < 0) return;
    int bcType = int(h >> 4);
    std::vector<Bits> blk;
    for (size_t p = 6; p + 64 <= d.size() && p < 262; p += 64) {
        Bits o, ev;
        deInt2(d.data() + p, 64, o, ev);
        blk.push_back(o); blk.push_back(ev);
    }
    Ecc ec = eccBlocks(blk, bch1207());
    f.corrected = ec.fixed + e;
    if (ec.err || ec.data.size() < 84) return;   // two 42-bit blocks at least
    const Bits& b = ec.data;
    f.ok = true;
    if (bcType != 0) return;
    f.satId = int(rd(b, 0, 7));
    f.beamId = int(rd(b, 7, 6));
    if (guessed && (f.beamId > 47 || rd(b, 42, 6) > 2)) f.ok = false;   // 48 spot beams; block types 0 to 2
    if (rd(b, 42, 6) == 1) {   // type 1: the Iridium time
        uint32_t t = rd(b, 52, 32);
        f.lbfc = int(t);
        f.hasTime = true;
        f.unixTime = iridiumTimeFromLbfc(t, ref);
    }
}

// ---- pager (IMS / MSG) ----
void decodeMs(const Bits& d, IridiumFrame& f) {
    f.type = IridiumType::IMS; f.typeName = "IMS";
    std::vector<Bits> blk;
    for (size_t p = 32; p + 64 <= d.size(); p += 64) {
        Bits o, e;
        deInt2(d.data() + p, 64, o, e);
        blk.push_back(o); blk.push_back(e);
    }
    while (blk.size() >= 3 && isFill(blk[blk.size() - 2], blk.back())) { blk.pop_back(); blk.pop_back(); }
    if (blk.empty()) return;
    Ecc ec = eccBlocks(blk, bch1897());
    f.corrected = ec.fixed;
    if (ec.err || ec.data.empty()) return;
    std::vector<Bits> b21;
    for (size_t i = 0; i + 21 <= ec.data.size(); i += 21) b21.emplace_back(ec.data.begin() + i, ec.data.begin() + i + 21);
    const Bits& h = b21[0];
    bool acq = h[0];
    if (rd(h, 1, 4) != 0) return;
    int bchBlocks = int(rd(h, 15, 4));
    if (bchBlocks < 2 || size_t(bchBlocks) * 2 > b21.size()) return;
    b21.resize(size_t(bchBlocks) * 2);
    b21.erase(b21.begin());
    if (acq) {
        if (b21.size() < 2) return;
        b21.erase(b21.begin(), b21.begin() + (b21.size() >= 4 ? 4 : 2));
    }
    for (int t = 0; t < 2 && !b21.empty() && b21.back()[0]; t++) {
        for (uint8_t x : b21.back()) if (!x) return;   // a trailer that is not all ones: the toolkit calls this an error
        b21.pop_back();
    }
    f.ok = true;   // the frame itself is fine; the body decides below
    if (b21.size() < 3) return;
    Bits rest;
    for (const Bits& x : b21) rest.insert(rest.end(), x.begin() + 1, x.end());
    if (rest.size() <= 27) { f.ok = false; return; }
    uint32_t ricRev = rd(rest, 0, 22), ric = 0;
    for (int i = 0; i < 22; i++) ric |= ((ricRev >> i) & 1u) << (21 - i);   // rest[0:22][::-1]
    int fmt = int(rd(rest, 22, 5));
    if (rest.size() - 27 <= 16) { f.ok = false; return; }
    int seq = int(rd(rest, 27, 6));
    if (rd(rest, 33, 4) != 0) { f.ok = false; return; }
    f.ric = int(ric);
    f.msgSeq = seq;
    f.msgFmt = fmt;
    // 10-bit packet checksum over the blocks after the first (msg_checksum)
    {
        std::vector<Bits> bl(b21.begin() + 1, b21.end());
        uint32_t v = 0;
        for (int i = 0; i < 3; i++) v |= uint32_t(bl[0][18 + i]) << i;
        for (int i = 0; i < 7; i++) v |= uint32_t(bl[1][1 + i]) << (3 + i);
        // cs string is bl[0][-3:] + bl[1][1:8] reversed: first transmitted bit is the lowest value bit
        uint32_t sum = 0;
        for (size_t i = 0; i < bl.size(); i++) {
            if (i != 1) sum += rd(bl[i], 0, 8);
            sum += rd(bl[i], 8, 8);
            if (i != 0) sum += rd(bl[i], 16, 5);
        }
        f.crcOk = ((v + sum) % 1024) == 1023;
    }
    Bits data(rest.begin() + 43, rest.end());   // msg_data
    if (data.size() < 5) { f.ok = false; return; }
    if (fmt == 5) {
        size_t o = 5;
        int ctr = 0, ctrMax = 0;
        if (data[4]) {
            int lfl = int(rd(data, 5, 4));
            if (lfl == 0 || lfl > 2) { f.ok = false; return; }
            if (data.size() < size_t(9 + 2 * lfl)) { f.ok = false; return; }
            ctr = int(rd(data, 9, lfl));
            ctrMax = int(rd(data, 9 + lfl, lfl));
            o = size_t(9 + 2 * lfl);
        }
        if (data.size() < o + 8) { f.ok = false; return; }
        if (data[o]) { f.ok = false; return; }
        f.msgChecksum = int(rd(data, o + 1, 7));
        o += 8;
        std::string raw;
        bool etx = false;
        size_t i = o;
        for (; i + 7 <= data.size(); i += 7) {
            char c = char(rd(data, i, 7));
            if (c == 3) etx = true;
            else if (etx) { f.ok = false; }
            raw += c;
        }
        f.msgRaw = raw;
        std::string txt = raw;
        while (!txt.empty() && txt.back() == 3) txt.pop_back();
        f.msgText = printable(txt);
        f.block = ctr;
        f.blocks = ctrMax + 1;
        Bits msgdata(data.begin() + o, data.end());
        std::vector<uint8_t> bytes;
        for (size_t k = 0; k + 8 <= msgdata.size(); k += 8) bytes.push_back(uint8_t(rd(msgdata, k, 8)));
        f.hex = hexOf(bytes) + ".";
        for (size_t k = bytes.size() * 8; k < msgdata.size(); k++) f.hex += char('0' + msgdata[k]);
    } else if (fmt == 3) {
        std::string digits;
        for (size_t k = 1; k + 4 <= data.size(); k += 4) digits += "0123456789abcdef"[rd(data, k, 4)];
        f.msgRaw = digits;
        while (!digits.empty() && digits.back() == 'c') digits.pop_back();
        f.msgText = digits;
        f.block = 0; f.blocks = 1; f.msgChecksum = -1;
    } else {
        return;   // unknown format: stays an IMS frame
    }
    f.type = IridiumType::MSG;
    f.typeName = fmt == 5 ? "MSG" : "MS3";
    if (fmt == 5) f.ok = f.ok && f.crcOk;   // the toolkit checks the packet sum of ASCII messages only
}

// ---- link control word frames: voice, IP, data, sync ----
std::vector<uint8_t> payloadBytes(const Bits& d, size_t off, size_t nbytes, bool reversed) {
    std::vector<uint8_t> v;
    for (size_t k = 0; k < nbytes; k++) {
        uint32_t x = rd(d, off + 8 * k, 8), r = 0;
        for (int i = 0; i < 8; i++) r |= ((x >> i) & 1u) << (7 - i);
        v.push_back(uint8_t(reversed ? r : x));
    }
    return v;
}

void decodeIda(const Bits& d, IridiumFrame& f) {
    f.type = IridiumType::IDA; f.typeName = "IDA";
    size_t off = 46;
    if (d.size() < off + 312) return;
    std::vector<Bits> desc;
    for (int g = 0; g < 2; g++) {
        Bits o, e;
        deInt2(d.data() + off + 124 * g, 124, o, e);
        Bits all(o);
        all.insert(all.end(), e.begin(), e.end());
        Bits b[4];
        for (int k = 0; k < 4; k++) b[k].assign(all.begin() + 31 * k, all.begin() + 31 * (k + 1));
        desc.push_back(b[3]); desc.push_back(b[1]); desc.push_back(b[2]); desc.push_back(b[0]);
    }
    {
        Bits o, e;
        deInt2(d.data() + off + 248, 64, o, e);
        desc.emplace_back(e.begin() + 1, e.end());
        desc.emplace_back(o.begin() + 1, o.end());
    }
    Bits bs;
    for (const Bits& b : desc) {
        uint32_t cw = rd(b, 0, 31);
        int e = bch3545().repair(cw, 31);
        if (e < 0) break;
        f.corrected += e;
        wr(bs, cw >> 11, 20);
    }
    if (bs.size() < 200) return;   // toolkit: not enough data
    int len = int(rd(bs, 11, 5));
    f.idaCont = bs[3];
    f.idaCtr = int(rd(bs, 5, 3));
    f.idaLen = len;
    bool zeros = rd(bs, 17, 3) == 0 && rd(bs, 196, 4) == 0;
    std::vector<uint8_t> da;
    for (int k = 0; k < 20; k++) da.push_back(uint8_t(rd(bs, 20 + 8 * k, 8)));
    if (len > 0) {
        std::vector<uint8_t> cs;
        Bits t(bs.begin(), bs.begin() + 20);
        for (int i = 0; i < 12; i++) t.push_back(0);
        t.insert(t.end(), bs.begin() + 20, bs.begin() + 196);
        for (size_t k = 0; k + 8 <= t.size(); k += 8) cs.push_back(uint8_t(rd(t, k, 8)));
        f.crcOk = crc16ccitt(cs) == 0;
    }
    f.hex = hexOf(da, size_t(std::min(len, 20)));
    f.ok = zeros && f.crcOk;
}

void decodeLw(const Bits& d, IridiumFrame& f) {
    Lcw l = readLcw(d);
    f.lcwFt = l.ft;
    f.corrected = l.err < 0 ? 0 : l.err;
    f.ok = l.ok;
    bool full = d.size() >= 46 + 312;
    switch (l.ft) {
    case 0: case 1: {
        if (!full) { f.type = IridiumType::Unknown; f.typeName = "U" + std::to_string(l.ft); f.ok = false; return; }
        auto pr = payloadBytes(d, 46, 39, true);
        bool crc = crc24ip(pr) == 0;
        if (crc) {   // IP data frame (also when it came in the voice slot type, the toolkit's VDA)
            f.type = IridiumType::IIP; f.typeName = "IIP"; f.crcOk = true;
            f.hex = hexOf(pr, 35);
            f.ok = true;
        } else if (l.ft == 0) {
            f.type = IridiumType::Voice; f.typeName = "VOC";   // counted, never decoded
        } else {
            f.type = IridiumType::IIU; f.typeName = "IIU";    // IIQ/IIR need the toolkit's Reed-Solomon code (not implemented)
            f.hex = hexOf(payloadBytes(d, 46, 39, false));
        }
        break;
    }
    case 2:
        f.ok = false;
        decodeIda(d, f);
        f.lcwFt = 2;
        break;
    case 7: {
        f.type = IridiumType::ISY; f.typeName = "ISY";
        size_t n = d.size() - 46, errs = 0;
        for (size_t i = 0; i < n; i++) errs += (d[46 + i] != ((i & 1) ? 0 : 1));
        int sym = int((1 + errs) / 2);
        f.corrected = sym;
        f.ok = l.ok && sym == 0 && n >= 2;
        break;
    }
    default:
        f.type = IridiumType::Unknown; f.typeName = "U" + std::to_string(l.ft);
        if (full) f.hex = hexOf(payloadBytes(d, 46, 39, false));
        break;
    }
}

} // namespace

// ================= public: decoder =================
IridiumFrame decodeIridiumBurst(const IridiumBurstBits& b) {
    IridiumFrame f;
    f.uplink = !b.downlink;
    Bits d = swapPairs(b.bits);
    if (d.size() < 64) { f.typeName = "short"; return f; }
    int ec = 0;
    Kind k = classify(d, f.uplink, ec);
    switch (k) {
    case Kind::MS: decodeMs(d, f); if (f.type == IridiumType::IMS) f.corrected += ec; break;
    case Kind::TL:
        f.type = IridiumType::ITL; f.typeName = "ITL";
        f.ok = d.size() >= 96 + 768; f.corrected = ec;
        break;
    case Kind::BC: decodeBc(d, f, b.refUnixTime, ec > 0); break;
    case Kind::LW: decodeLw(d, f); break;
    case Kind::RA: decodeRa(d, f, ec > 0); break;
    case Kind::AQ: f.type = IridiumType::Unknown; f.typeName = "IAQ"; f.ok = false; break;
    case Kind::None: f.typeName = "unknown"; break;
    }
    return f;
}

// ================= public: assembler =================
struct IridiumMsgAssembler::State {
    struct Msg {
        int ric = 0, seq = 0, fmt = 5, csum = -1;
        double time = 0;
        std::vector<std::string> parts;
        std::vector<bool> have;
        bool sent = false;
    };
    std::map<std::string, Msg> buf;
};
IridiumMsgAssembler::IridiumMsgAssembler() : s_(std::make_unique<State>()) {}
IridiumMsgAssembler::~IridiumMsgAssembler() = default;
void IridiumMsgAssembler::reset() { s_->buf.clear(); }

namespace {
std::string joinMsg(const IridiumMsgAssembler*, int fmt, const std::vector<std::string>& parts, const std::vector<bool>& have, bool& all) {
    std::string t;
    all = true;
    for (size_t i = 0; i < parts.size(); i++) {
        if (have[i]) t += parts[i];
        else { all = false; t += "[missing]"; }
    }
    if (fmt == 5) while (!t.empty() && t.back() == 3) t.pop_back();
    else while (!t.empty() && t.back() == 'c') t.pop_back();
    return t;
}
}

std::vector<IridiumPagerMessage> IridiumMsgAssembler::expire(double timeSec) {
    std::vector<IridiumPagerMessage> out;
    for (auto it = s_->buf.begin(); it != s_->buf.end();) {
        auto& m = it->second;
        if (m.time + 2000 <= timeSec) {
            if (!m.sent) {
                bool all;
                IridiumPagerMessage p;
                p.ric = m.ric; p.seq = m.seq; p.timeSec = m.time; p.complete = false;
                p.text = printable(joinMsg(this, m.fmt, m.parts, m.have, all));
                out.push_back(p);
            }
            it = s_->buf.erase(it);
        } else ++it;
    }
    return out;
}

std::vector<IridiumPagerMessage> IridiumMsgAssembler::feed(const IridiumFrame& f, double timeSec) {
    std::vector<IridiumPagerMessage> out = expire(timeSec);
    if (f.type != IridiumType::MSG || !f.ok || (f.msgFmt != 5 && f.msgFmt != 3) || f.blocks < 1 || f.block < 0 || f.block >= f.blocks) return out;
    char key[64];
    snprintf(key, sizeof key, "%07d %04d %d", f.ric, f.msgSeq, f.msgFmt);
    auto it = s_->buf.find(key);
    if (it != s_->buf.end() && it->second.csum != f.msgChecksum) {
        double tdiff = timeSec - it->second.time;
        if (tdiff > 600 || size_t(f.block) >= it->second.parts.size()) { s_->buf.erase(it); it = s_->buf.end(); }
    }
    if (it == s_->buf.end()) {
        State::Msg m;
        m.ric = f.ric; m.seq = f.msgSeq; m.fmt = f.msgFmt; m.csum = f.msgChecksum; m.time = timeSec;
        m.parts.assign(size_t(f.blocks), std::string());
        m.have.assign(size_t(f.blocks), false);
        it = s_->buf.emplace(key, m).first;
    }
    auto& m = it->second;
    if (size_t(f.block) < m.parts.size()) { m.parts[size_t(f.block)] = f.msgRaw; m.have[size_t(f.block)] = true; }
    bool all;
    std::string text = joinMsg(this, m.fmt, m.parts, m.have, all);
    if (all && !m.sent) {
        bool good;
        if (m.fmt == 5) good = msgChecksum7(text) == m.csum;
        else { good = !text.empty(); for (char c : text) if (c < '0' || c > '9') good = false; }
        if (good) {
            m.sent = true;
            IridiumPagerMessage p;
            p.ric = m.ric; p.seq = m.seq; p.timeSec = timeSec; p.complete = true;
            p.text = printable(text);
            out.push_back(p);
        }
    }
    return out;
}

// ================= public: time and symbol helpers =================
double iridiumTimeFromLbfc(uint32_t lbfc, double ref) {
    if (ref <= 0) ref = nowUnix();
    double best = 0, bd = 1e30;
    for (int i = 2; i >= 0; i--) {
        double t = kEra[i] + lbfc * 0.09;
        if (std::fabs(t - ref) < bd) { bd = std::fabs(t - ref); best = t; }
    }
    return best;
}
uint32_t iridiumLbfcFromTime(double t) {
    double era = t >= kEraStart3 ? kEra[2] : t >= kEraStart2 ? kEra[1] : kEra[0];
    return uint32_t(uint64_t(std::llround((t - era) / 0.09)) & 0xffffffffu);
}
void iridiumStepsToBits(const std::vector<uint8_t>& steps, std::vector<uint8_t>& bits) {
    static const uint8_t code[4] = { 0, 2, 3, 1 };
    bits.clear();
    for (uint8_t s : steps) { uint8_t c = code[s & 3]; bits.push_back((c >> 1) & 1); bits.push_back(c & 1); }
}
void iridiumBitsToSteps(const std::vector<uint8_t>& bits, std::vector<uint8_t>& steps) {
    static const uint8_t inv[4] = { 0, 3, 1, 2 };   // code -> step
    steps.clear();
    for (size_t i = 0; i + 1 < bits.size(); i += 2) steps.push_back(inv[((bits[i] & 1) << 1) | (bits[i + 1] & 1)]);
}
const std::vector<uint8_t>& iridiumUniqueWordBits(bool downlink) {
    static const Bits dl = fromStr("001100000011000011110011"), ul = fromStr("110011000011110011111100");
    return downlink ? dl : ul;
}

// ================= public: builders =================
namespace {

Bits encLcw(int ft, uint32_t l2, uint32_t l3) {
    Bits seq;
    wr(seq, bch29().encode(uint32_t(ft) & 7), 7);
    wr(seq, bch465().encode(l2 & 63) >> 1, 13);   // the last bit of the 14-bit word is not sent
    wr(seq, bch41().encode(l3 & 0x1fffff), 26);
    return lcwInt(seq);
}
// 42 data bits -> a 64-bit group (two code words, interleaved)
Bits group64(const Bits& d42, size_t off, const Bch& b) {
    Bits o, e, g;
    putBlock32(o, rd(d42, off, 21), b);
    putBlock32(e, rd(d42, off + 21, 21), b);
    inInt2(o, e, g);
    return g;
}
Bits fillGroup() {
    Bits a = kFillA(), b = kFillB(), g;
    inInt2(a, b, g);
    return g;
}
}

std::vector<uint8_t> iridiumBuildLcwFrame(int ft, int lcw2, int lcw3, const std::vector<uint8_t>& payload312) {
    Bits d = encLcw(ft, uint32_t(lcw2), uint32_t(lcw3));
    Bits p = payload312;
    p.resize(312, 0);
    d.insert(d.end(), p.begin(), p.end());
    return swapPairs(d);
}

std::vector<uint8_t> iridiumBuildIsy() {
    Bits p;
    for (int i = 0; i < 156; i++) { p.push_back(1); p.push_back(0); }
    // lcw2/lcw3 as in the real frame of iridium-toolkit's tests/test_parser.py
    return iridiumBuildLcwFrame(7, 0x11, 0x0e0, p);
}

std::vector<uint8_t> iridiumBuildVoiceLike(uint32_t seed) {
    std::mt19937 g(seed ? seed : 1u);
    Bits p(312);
    for (auto& x : p) x = g() & 1;
    return iridiumBuildLcwFrame(0, int(g() & 63), int(g() & 0x1fffff), p);
}

std::vector<uint8_t> iridiumBuildIda(const std::vector<uint8_t>& payload, int ctr, bool cont) {
    int len = int(std::min<size_t>(payload.size(), 20));
    Bits bs;
    wr(bs, 0, 3); bs.push_back(cont); bs.push_back(0); wr(bs, uint32_t(ctr) & 7, 3); wr(bs, 0, 3);
    wr(bs, uint32_t(len), 5); wr(bs, 0, 1); wr(bs, 0, 3);
    for (int k = 0; k < 20; k++) wr(bs, k < len ? payload[size_t(k)] : 0, 8);
    Bits t(bs.begin(), bs.begin() + 20);
    for (int i = 0; i < 12; i++) t.push_back(0);
    t.insert(t.end(), bs.begin() + 20, bs.end());
    std::vector<uint8_t> cs;
    for (size_t k = 0; k + 8 <= t.size(); k += 8) cs.push_back(uint8_t(rd(t, k, 8)));
    wr(bs, crc16ccitt(cs), 16);
    wr(bs, 0, 4);
    Bits cw[10];
    for (int k = 0; k < 10; k++) { cw[k].clear(); wr(cw[k], bch3545().encode(rd(bs, size_t(20 * k), 20)), 31); }
    Bits payload312;
    for (int g = 0; g < 2; g++) {
        Bits odd(cw[4 * g + 3]), even(cw[4 * g + 2]), out;
        odd.insert(odd.end(), cw[4 * g + 1].begin(), cw[4 * g + 1].end());
        even.insert(even.end(), cw[4 * g].begin(), cw[4 * g].end());
        inInt2(odd, even, out);
        payload312.insert(payload312.end(), out.begin(), out.end());
    }
    Bits odd(1, 0), even(1, 0), out;
    odd.insert(odd.end(), cw[9].begin(), cw[9].end());
    even.insert(even.end(), cw[8].begin(), cw[8].end());
    inInt2(odd, even, out);
    payload312.insert(payload312.end(), out.begin(), out.end());
    return iridiumBuildLcwFrame(2, 0x03, 0, payload312);   // LCW: maintenance word 3, all fields zero
}

std::vector<uint8_t> iridiumBuildIraRaw(int satId, int beamId, int x, int y, int z, int interval, int ts, int eip, int bcSubband,
                                        const std::vector<IridiumPage>& pages) {
    Bits b;
    wr(b, uint32_t(satId) & 127, 7);
    wr(b, uint32_t(beamId) & 63, 6);
    wr(b, uint32_t(x) & 0xfff, 12); wr(b, uint32_t(y) & 0xfff, 12); wr(b, uint32_t(z) & 0xfff, 12);
    wr(b, uint32_t(interval) & 127, 7);
    b.push_back(ts & 1); b.push_back(eip & 1);
    wr(b, uint32_t(bcSubband) & 31, 5);
    size_t np = std::min<size_t>(pages.size(), 12);
    for (size_t i = 0; i < np; i++) {
        wr(b, pages[i].tmsi, 32); wr(b, 0, 2); wr(b, uint32_t(pages[i].mscId) & 31, 5); wr(b, 0, 3);
    }
    size_t groups = np;
    if (np < 12) { wr(b, 0xffffffffu, 32); wr(b, 0x3ff, 10); groups++; }
    Bits d;
    Bits blk[3];
    for (int i = 0; i < 3; i++) putBlock32(blk[i], rd(b, size_t(21 * i), 21), bch1207());
    inInt3(blk[0], blk[1], blk[2], d);
    for (size_t g = 0; g < groups; g++) {
        Bits gr = group64(b, 63 + 42 * g, bch1207());
        d.insert(d.end(), gr.begin(), gr.end());
    }
    for (size_t g = groups; g < 12; g++) {
        Bits gr = fillGroup();
        d.insert(d.end(), gr.begin(), gr.end());
    }
    return swapPairs(d);
}

std::vector<uint8_t> iridiumBuildIra(int satId, int beamId, double lat, double lon, double altKm, const std::vector<uint32_t>& tmsis) {
    double r = (altKm + 6378 - 23) / 4, la = lat * M_PI / 180, lo = lon * M_PI / 180;
    auto q = [](double v) { return int(std::lround(std::max(-2047.0, std::min(2047.0, v)))); };
    std::vector<IridiumPage> pages;
    for (uint32_t t : tmsis) { IridiumPage p; p.tmsi = t; pages.push_back(p); }
    return iridiumBuildIraRaw(satId, beamId, q(r * std::cos(la) * std::cos(lo)), q(r * std::cos(la) * std::sin(lo)), q(r * std::sin(la)),
                              48, 1, 0, 7, pages);
}

std::vector<uint8_t> iridiumBuildIbc(int satId, int beamId, double unixTime) {
    Bits b;
    wr(b, uint32_t(satId) & 127, 7); wr(b, uint32_t(beamId) & 63, 6);
    wr(b, 0, 1); wr(b, 0, 1); wr(b, 0, 1);       // unknown, slot, blocking
    wr(b, 0xffff, 16);                           // acquisition classes
    wr(b, 22, 5); wr(b, 2, 3); wr(b, 0, 2);      // acquisition sub-band and channels, unknown
    wr(b, 1, 6); wr(b, 0, 4); wr(b, iridiumLbfcFromTime(unixTime), 32);   // type 1: Iridium time
    for (int i = 0; i < 2; i++) { wr(b, 7, 3); wr(b, 0, 16); wr(b, 0, 16); wr(b, 0, 7); }   // empty assignments
    Bits d;
    wr(d, 0, 6);   // header: type 0, its BCH is zero
    for (int g = 0; g < 4; g++) {
        Bits gr = group64(b, size_t(42 * g), bch1207());
        d.insert(d.end(), gr.begin(), gr.end());
    }
    return swapPairs(d);
}

namespace {
// R: the 20-bit data blocks of the message; wraps them into header, trailer, code words and interleaving. sumRule: put the 10-bit
// packet checksum (ASCII messages) into R[37..46].
std::vector<uint8_t> wrapMsg(Bits r, int ric, int seq, int part, bool sumRule) {
    size_t nd = r.size() / 20;
    if (sumRule) {
        uint32_t sum = 0;
        for (size_t i = 1; i < nd; i++) {
            Bits bl(1, 0);
            bl.insert(bl.end(), r.begin() + 20 * i, r.begin() + 20 * (i + 1));
            size_t k = i - 1;   // the sum rule of msg_checksum, over the blocks after the first
            if (k != 1) sum += rd(bl, 0, 8);
            sum += rd(bl, 8, 8);
            if (k != 0) sum += rd(bl, 16, 5);
        }
        uint32_t cv = (1023 - sum % 1024) % 1024;
        for (int i = 0; i < 10; i++) r[37 + i] = (cv >> i) & 1;   // first bit sent is the lowest
    }
    size_t total = 1 + nd + 1;
    size_t trailers = (total & 1) ? 2 : 1;   // an even number of 21-bit blocks: pairs of 42
    total = 1 + nd + trailers;
    Bits blocks;
    wr(blocks, 0, 1); wr(blocks, 0, 4);
    wr(blocks, uint32_t(1 + (seq + part) % 14), 4);
    wr(blocks, uint32_t((ric ^ seq) % 48), 6);
    wr(blocks, uint32_t(total / 2) & 15, 4);
    wr(blocks, 0, 2);
    for (size_t i = 0; i < nd; i++) {
        blocks.push_back(0);
        blocks.insert(blocks.end(), r.begin() + 20 * i, r.begin() + 20 * (i + 1));
    }
    for (size_t t = 0; t < trailers; t++) for (int i = 0; i < 21; i++) blocks.push_back(1);
    Bits d = kHdrMsg();
    for (size_t g = 0; g * 42 < blocks.size(); g++) {
        Bits gr = group64(blocks, 42 * g, bch1897());
        d.insert(d.end(), gr.begin(), gr.end());
    }
    return swapPairs(d);
}
Bits msgHead(int ric, int seq, int fmt) {
    Bits r;
    uint32_t rr = uint32_t(ric) & 0x3fffff;
    for (int i = 0; i < 22; i++) r.push_back((rr >> i) & 1);   // reversed
    wr(r, uint32_t(fmt), 5);
    wr(r, uint32_t(seq) & 63, 6);
    wr(r, 0, 4);
    wr(r, 0, 6);   // checksum bits, filled in later
    return r;
}
std::vector<uint8_t> buildMsgBits(int ric, int seq, int part, int parts, const std::string& text, int csum7) {
    Bits r = msgHead(ric, seq, 5);
    wr(r, 0, 4);
    if (parts > 1) {
        r.push_back(1); wr(r, 2, 4); wr(r, uint32_t(part) & 3, 2); wr(r, uint32_t(parts - 1) & 3, 2);
    } else r.push_back(0);
    r.push_back(0);
    wr(r, uint32_t(csum7) & 127, 7);
    for (unsigned char c : text) wr(r, c & 127, 7);
    size_t pad = (20 - r.size() % 20) % 20;
    for (size_t i = 0; i < pad / 7; i++) wr(r, 3, 7);      // ETX fills whole characters
    for (size_t i = 0; i < pad % 7; i++) r.push_back(1);
    return wrapMsg(r, ric, seq, part, true);
}
}

std::vector<uint8_t> iridiumBuildMsgBcd(int ric, int seq, const std::string& digits) {
    Bits r = msgHead(ric, seq, 3);
    r.push_back(0);
    for (char c : digits) wr(r, c >= '0' && c <= '9' ? uint32_t(c - '0') : 0u, 4);
    size_t pad = (20 - r.size() % 20) % 20;
    for (size_t i = 0; i < pad / 4; i++) wr(r, 0xc, 4);
    for (size_t i = 0; i < pad % 4; i++) r.push_back(1);
    return wrapMsg(r, ric, seq, 0, false);
}

std::vector<uint8_t> iridiumBuildMsg(int ric, int seq, int block, int blocks, const std::string& textPart) {
    return buildMsgBits(ric, seq, block, blocks, textPart, msgChecksum7(textPart));
}

std::vector<std::vector<uint8_t>> iridiumBuildMsgParts(int ric, int seq, const std::string& text0) {
    std::string text = text0.substr(0, 177);
    for (auto& c : text) if (c < 0 || c > 126) c = '?';
    int cs = msgChecksum7(text);
    std::vector<std::vector<uint8_t>> out;
    if (text.size() <= 60) {
        out.push_back(buildMsgBits(ric, seq, 0, 1, text, cs));
        return out;
    }
    // Parts that are not the last must fill their blocks without ETX padding (the toolkit joins the parts as they are): 59 characters do.
    size_t n = (text.size() + 58) / 59;
    for (size_t i = 0; i < n; i++) out.push_back(buildMsgBits(ric, seq, int(i), int(n), text.substr(i * 59, 59), cs));
    return out;
}

} // namespace dect2
