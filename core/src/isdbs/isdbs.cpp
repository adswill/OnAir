// ISDB-S: modulation table, TMCC bits and their RS(64,48) code, the fixed parts of the frame. See isdbs.h and docs/ISDBS.md for the sources.
#include "dect2/isdbs/isdbs.h"
#include "dect2/common/mathconst.h"
#include "dect2/fec/fec_proto.h"
#include "isdbs_fec.h"
#include <array>
#include <cmath>
#include <cstring>

namespace dect2 {
namespace isdbs {

const char* modName(int mod) {
    switch (mod) {
    case kBpsk12: return "BPSK 1/2";
    case kQpsk12: return "QPSK 1/2";
    case kQpsk23: return "QPSK 2/3";
    case kQpsk34: return "QPSK 3/4";
    case kQpsk56: return "QPSK 5/6";
    case kQpsk78: return "QPSK 7/8";
    case kTc8psk23: return "TC8PSK 2/3";
    default: return "unused";
    }
}

bool isMod(int mod) { return mod >= kBpsk12 && mod <= kTc8psk23; }
int efficiencyRank(int mod) { return isMod(mod) ? kTc8psk23 - mod : 99; }   // the words of Table 1 rise with the efficiency

int slotUnit(int mod) {
    switch (mod) {
    case kBpsk12: return 4;
    case kQpsk12: return 2;
    case kQpsk23: return 3;
    case kQpsk34: return 4;
    case kQpsk56: return 6;
    case kQpsk78: return 8;
    default: return 1;
    }
}

int slotActive(int mod) {
    switch (mod) {
    case kBpsk12: return 1;
    case kQpsk12: return 1;
    case kQpsk23: return 2;
    case kQpsk34: return 3;
    case kQpsk56: return 5;
    case kQpsk78: return 7;
    default: return 1;
    }
}

int groupSteps(int mod, int count) {
    if (!isMod(mod)) return 0;
    const int packets = count / slotUnit(mod) * slotActive(mod);
    return mod == kTc8psk23 ? packets * kSlotSyms : packets * kSlotBytes * 8;
}

double slotTsBitrate(int mod) {
    if (!isMod(mod)) return 0;
    return (double)slotActive(mod) / slotUnit(mod) * 188.0 * 8.0 / kFrameSeconds;
}

bool Tmcc::operator==(const Tmcc& o) const {
    if (valid != o.valid || change != o.change || slotTs != o.slotTs || tsId != o.tsId || ews != o.ews || uplink != o.uplink) return false;
    for (int i = 0; i < kMaxGroups; i++) if (mode[i] != o.mode[i] || count[i] != o.count[i]) return false;
    return true;
}

int Tmcc::slotMod(int slot) const {
    int first = 0;
    for (int g = 0; g < kMaxGroups; g++) {
        if (!isMod(mode[g])) continue;
        if (slot >= first && slot < first + count[g]) return mode[g];
        first += count[g];
    }
    return kUnused;
}

bool Tmcc::slotCarries(int slot) const {
    int first = 0;
    for (int g = 0; g < kMaxGroups; g++) {
        if (!isMod(mode[g])) continue;
        if (slot >= first && slot < first + count[g]) return (slot - first) % slotUnit(mode[g]) < slotActive(mode[g]);
        first += count[g];
    }
    return false;
}

int Tmcc::slotsOf(int rel) const {
    int n = 0;
    for (int s = 0; s < kSlots; s++) if (slotTs[(size_t)s] == rel && slotMod(s) != kUnused) n++;
    return n;
}

int Tmcc::packetsOf(int rel) const {
    int n = 0;
    for (int s = 0; s < kSlots; s++) if (slotTs[(size_t)s] == rel && slotCarries(s)) n++;
    return n;
}

int Tmcc::modOf(int rel) const {
    for (int s = 0; s < kSlots; s++) if (slotTs[(size_t)s] == rel && slotMod(s) != kUnused) return slotMod(s);
    return -1;
}

namespace {
void putBits(uint8_t*& o, unsigned val, int n) { for (int i = n - 1; i >= 0; i--) *o++ = (uint8_t)((val >> i) & 1); }
unsigned getBits(const uint8_t*& b, int n) { unsigned v = 0; for (int i = 0; i < n; i++) v = (v << 1) | (*b++ & 1); return v; }
}  // namespace

std::array<uint8_t, kTmccBits> tmccPack(const Tmcc& t) {
    std::array<uint8_t, kTmccBits> out{};
    uint8_t* o = out.data();
    putBits(o, (unsigned)t.change & 31, 5);
    for (int g = 0; g < kMaxGroups; g++) {
        const bool used = isMod(t.mode[g]) && t.count[g] > 0;
        putBits(o, used ? (unsigned)t.mode[g] : (unsigned)kDummyWord, 4);
        putBits(o, used ? (unsigned)t.count[g] : 0u, 6);
    }
    for (int s = 0; s < kSlots; s++) putBits(o, t.slotTs[(size_t)s] & 7u, 3);
    for (int r = 0; r < kMaxStreams; r++) putBits(o, t.tsId[(size_t)r], 16);
    putBits(o, t.ews ? 1u : 0u, 1);
    putBits(o, (unsigned)t.uplink & 15, 4);
    // 62 reserved bits: left at 0 here (the transmitter's value is not known)
    return out;
}

bool tmccUnpack(const uint8_t* bits, Tmcc& t) {
    Tmcc r;
    const uint8_t* b = bits;
    r.change = (int)getBits(b, 5);
    int total = 0;
    for (int g = 0; g < kMaxGroups; g++) {
        const int w = (int)getBits(b, 4), c = (int)getBits(b, 6);
        if (w == kDummyWord) { if (c != 0) return false; r.mode[g] = kUnused; r.count[g] = 0; continue; }
        if (!isMod(w) || c % slotUnit(w)) return false;
        r.mode[g] = w; r.count[g] = c; total += c;
    }
    if (total > kSlots) return false;
    for (int s = 0; s < kSlots; s++) r.slotTs[(size_t)s] = (uint8_t)getBits(b, 3);
    for (int q = 0; q < kMaxStreams; q++) r.tsId[(size_t)q] = (uint16_t)getBits(b, 16);
    r.ews = getBits(b, 1) != 0;
    r.uplink = (int)getBits(b, 4);
    r.valid = true;
    t = r;
    return true;
}

// RS(64,48): the RS(204,188) code is RS(255,239) shortened by 51 bytes; 140 more zero bytes in front shorten it to (64,48)
void tmccEncode(const uint8_t* bits, uint8_t* out) {
    uint8_t in[188] = {}, cw[204];
    for (int i = 0; i < 48; i++) { unsigned v = 0; for (int k = 0; k < 8; k++) v = (v << 1) | (bits[i * 8 + k] & 1); in[140 + i] = (uint8_t)v; }
    fecp::rs204Encode(in, cw);
    for (int i = 0; i < 64; i++) for (int k = 0; k < 8; k++) out[i * 8 + k] = (cw[140 + i] >> (7 - k)) & 1;
}

int tmccDecode(const uint8_t* bits, uint8_t* out) {
    uint8_t cw[204] = {};
    for (int i = 0; i < 64; i++) { unsigned v = 0; for (int k = 0; k < 8; k++) v = (v << 1) | (bits[i * 8 + k] & 1); cw[140 + i] = (uint8_t)v; }
    const int r = fecp::rs204Decode(cw);
    if (r < 0) return -1;
    for (int i = 0; i < 140; i++) if (cw[i]) return -1;   // a "correction" in the bytes that are zero by definition: too many errors
    for (int i = 0; i < 48; i++) for (int k = 0; k < 8; k++) out[i * 8 + k] = (cw[140 + i] >> (7 - k)) & 1;
    return r;
}

const std::vector<float>& burstSequence() {
    static const std::vector<float> b = [] {
        std::vector<float> v((size_t)kBlocks * kBurstSyms);
        unsigned reg = 0x1FF;   // 1 + x^5 + x^9, all ones at the start of every frame: a choice of this code, see isdbs.h
        for (auto& x : v) {
            const unsigned o = ((reg >> 8) ^ (reg >> 4)) & 1;
            reg = ((reg << 1) | o) & 0x1FF;
            x = o ? -1.f : 1.f;
        }
        return v;
    }();
    return b;
}

namespace {
std::array<float, 20> wordTail(uint16_t w) {
    unsigned s = 0;
    for (int i = 0; i < 6; i++) s = convNext(s, (w >> (15 - i)) & 1);
    std::array<float, 20> a{};
    for (int i = 6; i < 16; i++) {
        const int in = (w >> (15 - i)) & 1;
        const unsigned o = convOut(s, in);
        s = convNext(s, in);
        a[(size_t)(2 * (i - 6))] = (o >> 1) ? -1.f : 1.f;
        a[(size_t)(2 * (i - 6) + 1)] = (o & 1) ? -1.f : 1.f;
    }
    return a;
}
}  // namespace

const std::array<float, 20>& w1Tail() { static const auto a = wordTail(kW1); return a; }
const std::array<float, 20>& w2Tail() { static const auto a = wordTail(kW2); return a; }

cf32 psk8Point(int label) {
    // (W, Y, X): 000 at 0 degrees, then 001, 011, 010 every 45 degrees; W = 1 is the opposite point
    // worked out once: the receiver asks for a point per TC8PSK symbol
    static const auto tab = [] {
        static const int kGray[4] = {0, 1, 3, 2};   // (Y, X) = 00, 01, 10, 11 -> 0, 1, 3, 2
        std::array<cf32, 8> t{};
        for (int l = 0; l < 8; l++) {
            const int idx = ((l >> 2) & 1) * 4 + kGray[l & 3];
            const double a = dect2::math::kPi * 0.25 * idx;
            t[(size_t)l] = cf32((float)std::cos(a), (float)std::sin(a));
        }
        return t;
    }();
    return tab[(size_t)(label & 7)];
}

}  // namespace isdbs
}  // namespace dect2
