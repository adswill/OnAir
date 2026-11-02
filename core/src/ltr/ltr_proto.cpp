// LTR word encode / parse and the test-signal modulator (see ltr_proto.h for the sources and what is not established).
#include "dect2/ltr/ltr_proto.h"
#include <cmath>
#include <cstdio>

namespace dect2::ltr {

static void put(std::vector<uint8_t>& b, uint32_t v, int n) { for (int i = n - 1; i >= 0; i--) b.push_back((v >> i) & 1); }
static uint32_t get(const uint8_t* b, int& p, int n) { uint32_t v = 0; for (int i = 0; i < n; i++) v = (v << 1) | (b[p++] & 1); return v; }

std::vector<uint8_t> encodeWord(const Word& w) {
    std::vector<uint8_t> b;
    put(b, kSync, 9);
    put(b, (uint32_t)w.area, 1);
    put(b, (uint32_t)w.gotoCh, 5);
    put(b, (uint32_t)w.home, 5);
    put(b, (uint32_t)w.id, 8);
    put(b, (uint32_t)w.freeCh, 5);
    put(b, (uint32_t)w.chk, 7);
    return b;
}

bool parseWord(const uint8_t* b, Word& w) {
    int p = 0;
    if (get(b, p, 9) != kSync) return false;
    w.area = (int)get(b, p, 1);
    w.gotoCh = (int)get(b, p, 5);
    w.home = (int)get(b, p, 5);
    w.id = (int)get(b, p, 8);
    w.freeCh = (int)get(b, p, 5);
    w.chk = (int)get(b, p, 7);
    return true;
}

// One 7-bit value per information bit, MSB (AREA) first: area, go-to 4..0, home 4..0, id 7..0, free 4..0 (table from sdrtrunk CRCLTR, GPL-3).
static const uint8_t kChk[24] = {
    0x38, 0x1C, 0x0E, 0x46, 0x23, 0x51, 0x68, 0x75, 0x7A, 0x3D, 0x1F, 0x4F,
    0x26, 0x52, 0x29, 0x15, 0x0B, 0x45, 0x62, 0x31, 0x19, 0x0D, 0x07, 0x43};

int checksum(const Word& w) {
    const uint32_t info = ((uint32_t)(w.area & 1) << 23) | ((uint32_t)(w.gotoCh & 31) << 18) | ((uint32_t)(w.home & 31) << 13) |
                          ((uint32_t)(w.id & 255) << 5) | (uint32_t)(w.freeCh & 31);
    int c = 0;
    for (int i = 0; i < 24; i++) if ((info >> (23 - i)) & 1) c ^= kChk[i];
    return c;
}

bool checkOk(const Word& w, bool inverted) {
    const int c = checksum(w);
    return w.chk == c || (inverted && w.chk == (c ^ 127));
}

bool plausible(const Word& w) {
    return w.home >= 1 && w.home <= 20 && (w.gotoCh <= 20 || w.gotoCh >= 29) && (w.freeCh <= 20 || w.freeCh == 31);
}

std::string talkgroup(const Word& w) {
    char s[24];
    snprintf(s, sizeof s, "%d-%02d-%03d", w.area, w.home, w.id);
    return s;
}

std::string kindOf(int id) {
    if (id == 255) return "idle";
    if (id == 254) return "callsign ID";
    if (id == 253) return "busy";
    if (id >= 240 && id <= 247) return "token";
    return "";
}

std::vector<cf32> modulate(const std::vector<uint8_t>& bits, double fs, double offsetHz, double devHz, float amp, double voiceHz, double voiceDevHz, bool invert) {
    const double pi = 3.14159265358979323846;
    const size_t n = (size_t)std::llround((double)bits.size() / kBaud * fs);
    std::vector<cf32> out(n);
    double phase = 0, lev = 0, vph = 0;
    const double a = 1.0 - std::exp(-2 * pi * 150.0 / fs);   // the data is smoothed to the subaudible band
    for (size_t i = 0; i < n; i++) {
        size_t k = (size_t)((double)i * kBaud / fs);
        if (k >= bits.size()) k = bits.size() - 1;
        const double target = (bits[k] ? 1.0 : -1.0) * (invert ? -1.0 : 1.0);
        lev += a * (target - lev);
        double f = lev * devHz + offsetHz;
        if (voiceHz > 0) { f += voiceDevHz * std::sin(vph); vph += 2 * pi * voiceHz / fs; }
        phase += 2 * pi * f / fs;
        if (phase > pi) phase -= 2 * pi; else if (phase < -pi) phase += 2 * pi;
        out[i] = cf32(amp * (float)std::cos(phase), amp * (float)std::sin(phase));
    }
    return out;
}

} // namespace dect2::ltr
