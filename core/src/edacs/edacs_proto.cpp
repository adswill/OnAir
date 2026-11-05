// EDACS frame encode / vote, the BCH check bits and the test-signal modulator (see edacs_proto.h for the sources).
#include "dect2/edacs/edacs_proto.h"
#include <cmath>

namespace dect2::edacs {

static void put(std::vector<uint8_t>& b, uint64_t v, int n) { for (int i = n - 1; i >= 0; i--) b.push_back((uint8_t)((v >> i) & 1)); }

static void putMessage(std::vector<uint8_t>& b, uint32_t data28, uint32_t check12) {
    const uint64_t m = ((uint64_t)(data28 & 0xFFFFFFF) << 12) | (check12 & 0xFFF);
    put(b, m, 40);
    put(b, ~m & 0xFFFFFFFFFFULL, 40);   // the middle copy is inverted
    put(b, m, 40);
}

uint32_t bch(uint32_t data28) {
    constexpr uint32_t kG = 0x1539;   // x^12 + x^10 + x^8 + x^5 + x^4 + x^3 + 1
    uint64_t r = (uint64_t)(data28 & 0xFFFFFFF) << 12;
    for (int i = 39; i >= 12; i--)
        if ((r >> i) & 1) r ^= (uint64_t)kG << (i - 12);
    return (uint32_t)(r & 0xFFF);
}

bool bchOk(uint64_t v) { return bch((uint32_t)((v >> 12) & 0xFFFFFFF)) == (uint32_t)(v & 0xFFF); }

std::vector<uint8_t> encodeFrame(uint32_t msgA, uint32_t msgB, int checkA, int checkB) {
    std::vector<uint8_t> b;
    put(b, kSyncPattern, kSyncBits);
    putMessage(b, msgA, checkA < 0 ? bch(msgA) : (uint32_t)checkA);
    putMessage(b, msgB, checkB < 0 ? bch(msgB) : (uint32_t)checkB);
    return b;
}

uint64_t vote(const uint8_t* b, int& dis) {
    uint64_t v = 0;
    dis = 0;
    for (int i = 0; i < kMsgBits; i++) {
        const int x = b[i] & 1, y = (b[40 + i] & 1) ^ 1, z = b[80 + i] & 1;
        const int s = x + y + z;
        if (s == 1 || s == 2) dis++;
        v = (v << 1) | (uint64_t)(s >= 2);
    }
    return v;
}

std::vector<cf32> modulate(const std::vector<uint8_t>& bits, double fs, double offsetHz, double devHz, float amp, bool invert) {
    const double pi = 3.14159265358979323846;
    const size_t n = (size_t)std::llround((double)bits.size() / kBaud * fs);
    std::vector<cf32> out(n);
    double phase = 0, lev = 0;
    const double a = 1.0 - std::exp(-2 * pi * 0.75 * kBaud / fs);   // a little edge smoothing
    for (size_t i = 0; i < n; i++) {
        size_t k = (size_t)((double)i * kBaud / fs);
        if (k >= bits.size()) k = bits.size() - 1;
        lev += a * ((bits[k] ? 1.0 : -1.0) * (invert ? -1.0 : 1.0) - lev);
        phase += 2 * pi * (lev * devHz + offsetHz) / fs;
        if (phase > pi) phase -= 2 * pi; else if (phase < -pi) phase += 2 * pi;
        out[i] = cf32(amp * (float)std::cos(phase), amp * (float)std::sin(phase));
    }
    return out;
}

} // namespace dect2::edacs
