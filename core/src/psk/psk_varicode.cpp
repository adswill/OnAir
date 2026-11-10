// PSK31 / PSK63 varicode, convolutional code and Viterbi decoder (see psk_varicode.h).
#include "dect2/psk/psk_varicode.h"
#include "../hfdig/hfdig_kbd_tables.h"
#include <algorithm>
#include <map>
#include "dect2/common/bits.h"

namespace dect2 {
namespace psk {

namespace {
const char* const kVaricode[128] = {
    "1010101011", "1011011011", "1011101101", "1101110111", "1011101011", "1101011111", "1011101111", "1011111101",
    "1011111111", "11101111", "11101", "1101101111", "1011011101", "11111", "1101110101", "1110101011",
    "1011110111", "1011110101", "1110101101", "1110101111", "1101011011", "1101101011", "1101101101", "1101010111",
    "1101111011", "1101111101", "1110110111", "1101010101", "1101011101", "1110111011", "1011111011", "1101111111",
    "1", "111111111", "101011111", "111110101", "111011011", "1011010101", "1010111011", "101111111",
    "11111011", "11110111", "101101111", "111011111", "1110101", "110101", "1010111", "110101111",
    "10110111", "10111101", "11101101", "11111111", "101110111", "101011011", "101101011", "110101101",
    "110101011", "110110111", "11110101", "110111101", "111101101", "1010101", "111010111", "1010101111",
    "1010111101", "1111101", "11101011", "10101101", "10110101", "1110111", "11011011", "11111101",
    "101010101", "1111111", "111111101", "101111101", "11010111", "10111011", "11011101", "10101011",
    "11010101", "111011101", "10101111", "1101111", "1101101", "101010111", "110110101", "101011101",
    "101110101", "101111011", "1010101101", "111110111", "111101111", "111111011", "1010111111", "101101101",
    "1011011111", "1011", "1011111", "101111", "101101", "11", "111101", "1011011",
    "101011", "1101", "111101011", "10111111", "11011", "111011", "1111", "111",
    "111111", "110111111", "10101", "10111", "101", "110111", "1111011", "1101011",
    "11011111", "1011101", "111010101", "1010110111", "110111011", "1010110101", "1011010111", "1110110101",
};

const std::map<uint32_t, int>& codeTable() {
    static const std::map<uint32_t, int> t = [] {
        std::map<uint32_t, int> m;
        for (int c = 0; c < 128; c++) {
            uint32_t v = 0;
            for (const char* p = kVaricode[c]; *p; p++) v = (v << 1) | (uint32_t)(*p == '1');
            m[v] = c;
        }
        return m;
    }();
    return t;
}

int parity(uint32_t v) { return dect2::bits::parity((uint32_t)v); }
} // namespace

const char* varicode(int c) { return c >= 0 && c < 128 ? kVaricode[c] : ""; }

std::vector<uint8_t> varicodeBits(const std::string& text) {
    std::vector<uint8_t> b;
    for (unsigned char ch : text) {
        if (ch >= 128) continue;
        for (const char* p = kVaricode[ch]; *p; p++) b.push_back(*p == '1');
        b.push_back(0); b.push_back(0);
    }
    return b;
}

int VaricodeRx::bit(int b) {
    sh_ = (sh_ << 1) | (uint32_t)(b & 1);
    if (++n_ > 24) n_ = 24;
    if ((sh_ & 3) != 0) return -1;
    // two zeros: the bits before them are a character (or nothing, between idle zeros)
    const uint32_t code = sh_ >> 2;
    const int len = n_ - 2;
    sh_ = 0; n_ = 0;
    if (code == 0 || len <= 0) return -1;
    if (len > 12) return -2;
    const auto& t = codeTable();
    const auto it = t.find(code);
    return it == t.end() ? -2 : it->second;
}

int ConvEncoder::encode(int bit) {
    sh_ = ((sh_ << 1) | (uint32_t)(bit & 1)) & 0x1F;
    return parity(sh_ & 0x17) | (parity(sh_ & 0x19) << 1);
}

void Viterbi::reset() {
    for (int s = 0; s < 16; s++) metric_[s] = 0.f;
    hist_.assign(kDepth, 0);
    pos_ = 0; filled_ = 0;
}

int Viterbi::decode(float s0, float s1) {
    float nm[16];
    uint16_t dec = 0;
    for (int ns = 0; ns < 16; ns++) {
        const int b = ns & 1;
        float best = -1e30f; int bx = 0;
        for (int x = 0; x < 2; x++) {
            const int ps = (ns >> 1) | (x << 3);
            const uint32_t reg = (uint32_t)((ps << 1) | b) & 0x1F;
            const int e0 = parity(reg & 0x17), e1 = parity(reg & 0x19);
            const float m = metric_[ps] + (e0 ? s0 : -s0) + (e1 ? s1 : -s1);
            if (m > best) { best = m; bx = x; }
        }
        nm[ns] = best;
        if (bx) dec |= (uint16_t)(1u << ns);
    }
    float top = -1e30f; int bs = 0;
    for (int s = 0; s < 16; s++) if (nm[s] > top) { top = nm[s]; bs = s; }
    for (int s = 0; s < 16; s++) metric_[s] = nm[s] - top;   // keep them small
    hist_[(size_t)pos_] = dec;
    pos_ = (pos_ + 1) % kDepth;
    if (filled_ < kDepth) { filled_++; if (filled_ < kDepth) return -1; }
    int s = bs, out = 0;
    for (int k = 1; k <= kDepth; k++) {
        const uint16_t d = hist_[(size_t)((pos_ - k + 2 * kDepth) % kDepth)];
        out = s & 1;
        s = (s >> 1) | (((d >> s) & 1) << 3);
    }
    return out;
}

int qpskTurns(int sym) { return (2 - sym) & 3; }
int qpskSymbol(int turns) { return (2 - turns) & 3; }

// ---- 8PSK125 / 8PSK250 without FEC (fldigi src/psk/psk.cxx): fldigi's symbol k steps its baseband phase by 180 + 45 k degrees; its modulator
// sends I cos + Q sin, so on the air (upper sideband) the step is the opposite: 4 - k eighth turns
int psk8Eighths(int sym) { return (4 - sym) & 7; }
int psk8Symbol(int eighths) { return (4 - eighths) & 7; }

std::vector<int> psk8Steps(const std::string& text, int preamble, int postamble) {
    std::vector<int> st((size_t)std::max(0, preamble), psk8Eighths(0));   // symbol 0 again and again: the two-tone preamble
    std::vector<uint8_t> bits;
    auto put = [&](int c) { for (const char* q = kbd::kMfskVaricode[c & 255]; *q; q++) bits.push_back((uint8_t)(*q - '0')); };
    put(0);                                    // a <NUL> to start, as fldigi does
    for (unsigned char c : text) put(c);
    for (int i = 0; i < 3; i++) put(0);        // the flush: three <NUL>
    while (bits.size() % 3) bits.push_back(0);
    for (size_t i = 0; i < bits.size(); i += 3) st.push_back(psk8Eighths(bits[i] | bits[i + 1] << 1 | bits[i + 2] << 2));   // least significant bit first
    st.insert(st.end(), (size_t)std::max(0, postamble), psk8Eighths(4));   // symbol 4: the unmodulated carrier
    return st;
}

namespace {
const std::map<uint32_t, int>& mfskTable() {
    static const std::map<uint32_t, int> t = [] {
        std::map<uint32_t, int> m;
        for (int c = 0; c < 256; c++) {
            uint32_t v = 0;
            for (const char* q = kbd::kMfskVaricode[c]; *q; q++) v = (v << 1) | (uint32_t)(*q - '0');
            m[v] = c;
        }
        return m;
    }();
    return t;
}
}  // namespace

// every MFSK code starts with a one and ends with "00", and has no "00" inside: a one after two zeros starts the next character
int MfskVaricodeRx::bit(int b) {
    sh_ = (sh_ << 1) | (uint32_t)(b & 1);
    if (++n_ > 24) { sh_ &= 1; n_ = 1; return -2; }   // too long for any code: noise
    if ((sh_ & 7) != 1 || n_ < 4) return -1;
    const uint32_t code = sh_ >> 1;
    sh_ = 1; n_ = 1;
    const auto& t = mfskTable();
    const auto it = t.find(code);
    return it == t.end() ? -2 : it->second;
}

} // namespace psk
} // namespace dect2
