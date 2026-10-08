#include "dect2/aero_phy.h"
#include <algorithm>
#include <cstring>

namespace dect2 {

const AeroFrameFormat* aeroFrameFormat(int bitRate) {
    // aerol.cpp setSettings: leaver.setSize / block.resize / AERO_SPEC_NumberOfBits / BitsInHeader / TotalNumberOfBits
    static const AeroFrameFormat f600{600, 32, 16, 0, 1152, 6, 3};
    static const AeroFrameFormat f1200{1200, 32, 16, 0, 1152, 9, 2};
    static const AeroFrameFormat f10500{10500, 64, 16, 178, 4992, 78, 1};
    switch (bitRate) {
    case 600: return &f600;
    case 1200: return &f1200;
    case 10500: return &f10500;
    default: return nullptr;
    }
}

// ---- scrambler ----
const std::vector<uint8_t>& AeroScrambler::sequence() {
    static const std::vector<uint8_t> seq = [] {
        // aerol.h AeroLScrambler: state {1,1,0,1,0,0,1,0,1,0,1,1,0,0,1}; v = s[0]^s[14]; shift s[i] = s[i-1]; s[0] = v; output v
        int s[15] = {1, 1, 0, 1, 0, 0, 1, 0, 1, 0, 1, 1, 0, 0, 1};
        std::vector<uint8_t> v(5000);
        for (int a = 0; a < 5000; a++) {
            const int b = s[0] ^ s[14];
            v[a] = (uint8_t)b;
            for (int i = 14; i > 0; i--) s[i] = s[i - 1];
            s[0] = b;
        }
        return v;
    }();
    return seq;
}
AeroScrambler::AeroScrambler() : seq_(sequence()) {}

// ---- interleaver ----
namespace {
struct RowPerm {
    uint8_t perm[64], deperm[64];
    RowPerm() {
        for (int i = 0; i < 64; i++) { perm[(i * 27) % 64] = (uint8_t)i; deperm[i] = (uint8_t)((i * 27) % 64); }
    }
};
const RowPerm& rowPerm() { static const RowPerm r; return r; }
inline int parity7(unsigned v) { return __builtin_popcount(v & 0x7F) & 1; }
} // namespace

void aeroInterleave(const uint8_t* coded, uint8_t* tx, int cols) {
    const RowPerm& r = rowPerm();
    for (int i = 0; i < 64; i++)
        for (int j = 0; j < cols; j++) tx[i * cols + j] = coded[r.perm[i] + 64 * j];
}
void aeroDeinterleave(const float* rx, float* out, int cols) {
    const RowPerm& r = rowPerm();
    for (int j = 0; j < cols; j++)
        for (int i = 0; i < 64; i++) out[j * 64 + i] = rx[r.deperm[i] * cols + j];
}

// ---- convolutional code ----
void AeroConvEncoder::encode(int bit, uint8_t& c0, uint8_t& c1) {
    sr_ = ((sr_ << 1) | (unsigned)(bit & 1)) & 0x7F;
    c0 = (uint8_t)parity7(sr_ & kAeroPoly0);
    c1 = (uint8_t)parity7(sr_ & kAeroPoly1);
}

AeroViterbi::AeroViterbi() : pm_(64, 0.f), pmNew_(64, 0.f) {
    for (unsigned sr = 0; sr < 128; sr++) { out0_[sr] = (uint8_t)parity7(sr & kAeroPoly0); out1_[sr] = (uint8_t)parity7(sr & kAeroPoly1); }
}
void AeroViterbi::reset() { std::fill(pm_.begin(), pm_.end(), 0.f); }

void AeroViterbi::decode(const float* soft, int n, uint8_t* bits) {
    if ((int)dec_.size() < n) dec_.resize(n);
    for (int t = 0; t < n; t++) {
        const float s0 = soft[2 * t], s1 = soft[2 * t + 1];
        const float bm[4] = {-s0 - s1, -s0 + s1, s0 - s1, s0 + s1};   // index c0*2 + c1
        uint64_t d = 0;
        float best = -1e30f;
        for (int ns = 0; ns < 64; ns++) {
            // register (x << 6) | ns: the oldest bit x leaves, the predecessor state is (ns >> 1) | (x << 5)
            const unsigned a = (unsigned)ns, b = 64u | (unsigned)ns;
            const float m0 = pm_[ns >> 1] + bm[out0_[a] * 2 + out1_[a]];
            const float m1 = pm_[(ns >> 1) | 32] + bm[out0_[b] * 2 + out1_[b]];
            float m;
            if (m1 > m0) { m = m1; d |= 1ull << ns; } else m = m0;
            pmNew_[ns] = m;
            best = std::max(best, m);
        }
        for (int s = 0; s < 64; s++) pm_[s] = pmNew_[s] - best;      // keep the numbers small
        dec_[t] = d;
    }
    int s = (int)(std::max_element(pm_.begin(), pm_.end()) - pm_.begin());
    for (int t = n - 1; t >= 0; t--) {
        bits[t] = (uint8_t)(s & 1);
        const int x = (int)((dec_[t] >> s) & 1);
        s = (s >> 1) | (x << 5);
    }
}

// ---- CRC ----
uint16_t aeroCrc16(const uint8_t* p, size_t n) {
    uint16_t c = 0xFFFF;
    for (size_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++) c = (c & 1) ? (uint16_t)((c >> 1) ^ 0x8408) : (uint16_t)(c >> 1);
    }
    return (uint16_t)~c;
}
bool aeroSuCrcOk(const uint8_t* su) {
    const uint16_t rec = (uint16_t)((su[11] << 8) | su[10]);
    if (aeroCrc16(su, 10) == rec) return true;
    if (rec) return false;
    for (int i = 0; i < 10; i++) if (su[i]) return false;       // JAERO takes an all-zero SU as good (aerol.cpp: if((!crc_rec)&&...))
    return true;
}
void aeroSuSetCrc(uint8_t* su) {
    const uint16_t c = aeroCrc16(su, 10);
    su[10] = (uint8_t)(c & 0xFF);
    su[11] = (uint8_t)(c >> 8);
}

void aeroBytesToBits(const uint8_t* bytes, size_t nBytes, uint8_t* bits) {
    for (size_t i = 0; i < nBytes; i++)
        for (int k = 0; k < 8; k++) bits[8 * i + k] = (uint8_t)((bytes[i] >> k) & 1);
}
void aeroBitsToBytes(const uint8_t* bits, size_t nBytes, uint8_t* bytes) {
    for (size_t i = 0; i < nBytes; i++) {
        uint8_t c = 0;
        for (int k = 0; k < 8; k++) c |= (uint8_t)((bits[8 * i + k] & 1) << k);
        bytes[i] = c;
    }
}

// ---- frames ----
AeroFrameEncoder::AeroFrameEncoder(int bitRate) {
    const AeroFrameFormat* f = aeroFrameFormat(bitRate);
    if (f) fmt_ = *f;
}

std::vector<uint8_t> AeroFrameEncoder::frame(const uint8_t* info, uint16_t header) {
    std::vector<uint8_t> out;
    if (!fmt_.bitRate) return out;
    out.reserve(fmt_.totalBits());
    for (int i = 0; i < kAeroUwBits; i++) {
        const uint8_t u = (uint8_t)((kAeroUw >> (kAeroUwBits - 1 - i)) & 1);
        out.push_back(u);
        if (fmt_.oqpsk()) out.push_back(u);          // the same word on both arms (aerol.cpp checks each arm)
    }
    for (int i = 15; i >= 0; i--) out.push_back((uint8_t)((header >> i) & 1));
    for (int i = 0; i < fmt_.skipBits; i++) {         // not data: what a real station puts here is not known, so filler without a line spectrum
        fill_ ^= fill_ << 13; fill_ ^= fill_ >> 17; fill_ ^= fill_ << 5;
        out.push_back((uint8_t)(fill_ & 1));
    }
    const int nInfo = fmt_.infoBits();
    std::vector<uint8_t> bits(nInfo), coded(2 * nInfo), tx(fmt_.blockBits());
    aeroBytesToBits(info, (size_t)fmt_.infoBytes(), bits.data());
    AeroScrambler scr;
    for (int i = 0; i < nInfo; i++) {
        bits[i] ^= (uint8_t)scr.next();
        enc_.encode(bits[i], coded[2 * i], coded[2 * i + 1]);
    }
    for (int b = 0; b < fmt_.blocks; b++) {
        aeroInterleave(coded.data() + (size_t)b * fmt_.blockBits(), tx.data(), fmt_.cols);
        out.insert(out.end(), tx.begin(), tx.end());
    }
    return out;
}

AeroFrameDecoder::AeroFrameDecoder(int bitRate) {
    const AeroFrameFormat* f = aeroFrameFormat(bitRate);
    if (f) fmt_ = *f;
    deint_.resize(fmt_.codedBits);
    bits_.resize(fmt_.infoBits());
}

void AeroFrameDecoder::decode(const float* coded, uint16_t header, AeroDecodedFrame& out) {
    out.bitRate = fmt_.bitRate;
    out.header = header;
    out.bytes.assign(fmt_.infoBytes(), 0);
    out.susOk = out.susBad = 0;
    if (!fmt_.bitRate) return;
    const int bb = fmt_.blockBits();
    for (int b = 0; b < fmt_.blocks; b++)
        aeroDeinterleave(coded + (size_t)b * bb, deint_.data() + (size_t)b * bb, fmt_.cols);
    // the blocks are one continuous code: one trellis over the frame, so the end of a block is decided with the next block in view
    vit_.decode(deint_.data(), fmt_.codedBits / 2, bits_.data());
    int errs = 0;
    for (int i = 0; i < fmt_.infoBits(); i++) {
        uint8_t c0, c1;
        reenc_.encode(bits_[i], c0, c1);
        errs += (c0 != (deint_[2 * i] > 0)) + (c1 != (deint_[2 * i + 1] > 0));
    }
    out.channelBer = (float)errs / (float)fmt_.codedBits;
    AeroScrambler scr;
    for (int i = 0; i < fmt_.infoBits(); i++) bits_[i] ^= (uint8_t)scr.next();
    aeroBitsToBytes(bits_.data(), out.bytes.size(), out.bytes.data());
    for (size_t k = 0; k + 12 <= out.bytes.size(); k += 12) {
        if (aeroSuCrcOk(&out.bytes[k])) out.susOk++; else out.susBad++;
    }
}

} // namespace dect2
