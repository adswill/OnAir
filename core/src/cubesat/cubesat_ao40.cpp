// AO-40 FEC: see cubesat_ao40.h.
#include "dect2/cubesat/cubesat_ao40.h"
#include "dect2/cubesat/cubesat_proto.h"
#include <algorithm>
#include <cstring>

namespace dect2 {
namespace cubesat {

namespace {

// '11111110000111011110010110010010000001000100110001011101011011000' (gr-satellites' ao40_fec_deframer syncword)
const char kSync[] = "11111110000111011110010110010010000001000100110001011101011011000";

inline int par7(unsigned v) { v ^= v >> 4; v ^= v >> 2; v ^= v >> 1; return (int)(v & 1u); }

} // namespace

bool ao40Viterbi(const float* soft, size_t n, std::vector<uint8_t>& bits) {
    const size_t nb = n / 2;
    if (nb < 8) return false;
    constexpr int kStates = 64;
    // branch outputs for (previous state, input bit): the 7 bit register has the newest bit in the LSB
    uint8_t out[kStates][2];
    for (int s = 0; s < kStates; s++)
        for (int b = 0; b < 2; b++) {
            const unsigned reg = ((unsigned)s << 1 | (unsigned)b) & 0x7Fu;
            out[s][b] = (uint8_t)(par7(reg & 79u) | ((par7(reg & 109u) ^ 1) << 1));
        }
    std::vector<float> pm(kStates, 1e30f), npm(kStates);
    pm[0] = 0;
    std::vector<uint8_t> from(nb * kStates);   // the previous state's top bit decides: (state >> 1 of the new state) is shared, so store the choice
    std::vector<uint8_t> prevOf(nb * kStates);
    for (size_t t = 0; t < nb; t++) {
        const float s0 = soft[2 * t], s1 = soft[2 * t + 1];
        // cost of an output pair: -s if the bit is 1, +s if 0 (low = the sign agrees)
        const float c[4] = {s0 + s1, -s0 + s1, s0 - s1, -s0 - s1};
        std::fill(npm.begin(), npm.end(), 1e30f);
        for (int s = 0; s < kStates; s++) {
            if (pm[s] >= 1e29f) continue;
            for (int b = 0; b < 2; b++) {
                const int ns = ((s << 1) | b) & 0x3F;
                const float v = pm[s] + c[out[s][b]];
                if (v < npm[ns]) { npm[ns] = v; prevOf[t * kStates + (size_t)ns] = (uint8_t)s; from[t * kStates + (size_t)ns] = (uint8_t)b; }
            }
        }
        pm.swap(npm);
    }
    if (pm[0] >= 1e29f) return false;
    bits.assign(nb, 0);
    int st = 0;
    for (size_t t = nb; t-- > 0;) {
        bits[t] = from[t * kStates + (size_t)st];
        st = prevOf[t * kStates + (size_t)st];
    }
    return true;
}

bool ao40DecodeWindow(const float* w, std::vector<uint8_t>& frame, int& rsErrors, std::vector<uint8_t>* post) {
    // deinterleave: d[i] = w[80 * (i % 65) + i / 65]; the coded symbols start at 65
    std::vector<float> coded((size_t)kAo40Coded);
    for (int m = 0; m < kAo40Coded; m++) {
        const int i = kAo40SyncBits + m;
        coded[(size_t)m] = w[kAo40Step * (i % kAo40SyncBits) + i / kAo40SyncBits];
    }
    std::vector<uint8_t> bits;
    if (!ao40Viterbi(coded.data(), coded.size(), bits)) return false;
    std::vector<uint8_t> by(320, 0);
    for (size_t i = 0; i < 2560; i++) by[i >> 3] = (uint8_t)(by[i >> 3] | (bits[i] << (7 - (i & 7))));
    if (post) *post = by;
    ccsdsRandomize(by.data(), by.size());
    frame.assign((size_t)kAo40Frame, 0);
    rsErrors = 0;
    for (int j = 0; j < 2; j++) {
        uint8_t cw[160];
        for (int k = 0; k < 160; k++) cw[k] = by[(size_t)(j + 2 * k)];
        const int e = rsDecode(cw, 160);
        if (e < 0) return false;
        rsErrors += e;
        for (int k = 0; k < 128; k++) frame[(size_t)(j + 2 * k)] = cw[k];
    }
    return true;
}

std::vector<float> ao40Encode(const std::vector<uint8_t>& fr) {
    std::vector<uint8_t> f = fr;
    f.resize((size_t)kAo40Frame, 0);
    std::vector<uint8_t> by(320, 0);
    for (int j = 0; j < 2; j++) {
        uint8_t d[128], p[32];
        for (int k = 0; k < 128; k++) d[k] = f[(size_t)(j + 2 * k)];
        rsParity(d, 128, p);
        for (int k = 0; k < 128; k++) by[(size_t)(j + 2 * k)] = d[k];
        for (int k = 0; k < 32; k++) by[(size_t)(j + 2 * (128 + k))] = p[k];
    }
    ccsdsRandomize(by.data(), by.size());
    // rate 1/2 convolutional code, terminated with six zero bits
    std::vector<uint8_t> sym;
    unsigned reg = 0;
    for (size_t i = 0; i < 2566; i++) {
        const unsigned b = i < 2560 ? (unsigned)((by[i >> 3] >> (7 - (i & 7))) & 1) : 0u;
        reg = ((reg << 1) | b) & 0x7Fu;
        sym.push_back((uint8_t)par7(reg & 79u));
        sym.push_back((uint8_t)(par7(reg & 109u) ^ 1));
    }
    std::vector<float> d((size_t)kAo40Window, 1.f);   // padding symbols
    for (int i = 0; i < kAo40SyncBits; i++) d[(size_t)i] = kSync[i] == '1' ? 1.f : -1.f;
    for (int m = 0; m < kAo40Coded; m++) d[(size_t)(kAo40SyncBits + m)] = sym[(size_t)m] ? 1.f : -1.f;
    std::vector<float> w((size_t)kAo40Window);
    for (int i = 0; i < kAo40Window; i++) w[(size_t)(kAo40Step * (i % kAo40SyncBits) + i / kAo40SyncBits)] = d[(size_t)i];
    return w;
}

void Ao40Hunter::reset() { buf_.assign((size_t)kAo40Window, 0.f); pos_ = 0; filled_ = 0; skip_ = 0; }

void Ao40Hunter::push(float soft) {
    if (buf_.empty()) reset();
    buf_[pos_] = soft;
    pos_ = (pos_ + 1) % (size_t)kAo40Window;
    if (filled_ < (size_t)kAo40Window) { filled_++; }
    if (filled_ < (size_t)kAo40Window) return;
    if (skip_ > 0) { skip_--; return; }
    // the window starts at pos_ (the oldest symbol); the sync bit j sits at +80 j
    int match = 0;
    for (int j = 0; j < kAo40SyncBits; j++) {
        const float v = buf_[(pos_ + (size_t)(j * kAo40Step)) % (size_t)kAo40Window];
        match += ((v > 0) == (kSync[j] == '1')) ? 1 : 0;
    }
    const bool inv = match <= maxSyncErrors;
    if (!(match >= kAo40SyncBits - maxSyncErrors || inv)) return;
    syncs++;
    std::vector<float> w((size_t)kAo40Window);
    for (int i = 0; i < kAo40Window; i++) w[(size_t)i] = (inv ? -1.f : 1.f) * buf_[(pos_ + (size_t)i) % (size_t)kAo40Window];
    std::vector<uint8_t> fr;
    int e = 0;
    if (!ao40DecodeWindow(w.data(), fr, e)) { failed++; return; }
    frames++;
    skip_ = (size_t)kAo40Window - 1;   // the next frame starts after this one
    if (onFrame) onFrame(fr, e);
}

} // namespace cubesat
} // namespace dect2
