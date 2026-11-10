// GOES DCS message format (see goesdcs_proto.h).
#include "dect2/goesdcs/goesdcs_proto.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>

namespace dect2 {
namespace dcs {

const uint8_t kSync[kSyncBits] = {1, 0, 0, 0, 1, 0, 0, 1, 1, 0, 1, 0, 1, 1, 1};

namespace {
constexpr uint32_t kGen = 0x769;   // x^10 + x^9 + x^8 + x^6 + x^5 + x^3 + 1

uint32_t remainder(uint32_t data21) {
    uint32_t r = 0;
    for (int i = 20; i >= 0; i--) {
        r = (r << 1) | ((data21 >> i) & 1);
        if (r & 0x400) r ^= kGen;
    }
    for (int i = 0; i < 10; i++) {
        r <<= 1;
        if (r & 0x400) r ^= kGen;
    }
    return r & 0x3FF;
}

uint32_t syndrome(uint32_t w31) {   // remainder of the whole word by the generator: 0 for a codeword
    uint32_t r = 0;
    for (int i = 30; i >= 0; i--) {
        r = (r << 1) | ((w31 >> i) & 1);
        if (r & 0x400) r ^= kGen;
    }
    return r & 0x3FF;
}

const std::map<uint32_t, uint32_t>& errorTable() {   // syndrome -> error pattern (one or two bits)
    static const std::map<uint32_t, uint32_t> t = [] {
        std::map<uint32_t, uint32_t> m;
        for (int i = 0; i < 31; i++) m.emplace(syndrome(1u << i), 1u << i);
        for (int i = 0; i < 31; i++)
            for (int j = i + 1; j < 31; j++) m.emplace(syndrome((1u << i) | (1u << j)), (1u << i) | (1u << j));
        return m;
    }();
    return t;
}
} // namespace

uint32_t bchCheck(uint32_t data21) { return remainder(data21 & 0x1FFFFF); }
uint32_t addressWord(uint32_t data21) { data21 &= 0x1FFFFF; return (data21 << 10) | bchCheck(data21); }

bool addressDecode(uint32_t w, uint32_t& corrected, int& nerr) {
    w &= 0x7FFFFFFF;
    nerr = 0; corrected = w;
    const uint32_t s = syndrome(w);
    if (s == 0) return true;
    const auto& t = errorTable();
    const auto it = t.find(s);
    if (it == t.end()) return false;
    corrected = w ^ it->second;
    nerr = __builtin_popcount(it->second);
    return true;
}

// The operators write the 31 bits left-aligned in 32 (b1 as the top bit, a 0 after b31): EUMETSAT's reference platform is "162096C4"
std::string addressHex(uint32_t w) { char b[16]; snprintf(b, sizeof b, "%08X", (unsigned)((w & 0x7FFFFFFFu) << 1)); return b; }

double channelHz(int ch) { return kChannel1Hz + (ch - 1) * kChannelStepHz; }
int channelOf(double f) {
    const int ch = (int)std::lround((f - kChannel1Hz) / kChannelStepHz) + 1;
    return (ch >= 1 && ch <= 267) ? ch : 0;
}

double channelHz(int ch, Plan plan) {
    switch (plan) {
    case Plan::Goes: return ch >= 1 && ch <= 267 ? channelHz(ch) : 0.0;
    case Plan::Meteosat:
        if (ch >= 1 && ch <= 267) return 402034000.0 + 1500.0 * ch;
        if (ch >= 268 && ch <= 289) return 402002500.0 + 1500.0 * (ch - 268);
        return 0.0;
    case Plan::Himawari: return 0.0;
    }
    return 0.0;
}
int channelOf(double f, Plan plan) {
    switch (plan) {
    case Plan::Goes: return channelOf(f);
    case Plan::Meteosat: {
        const int ch = (int)std::lround((f - 402034000.0) / 1500.0);
        if (ch >= 1 && ch <= 267) return ch;
        const int lo = (int)std::lround((f - 402002500.0) / 1500.0) + 268;
        return (lo >= 268 && lo <= 289) ? lo : 0;
    }
    case Plan::Himawari: return 0;
    }
    return 0;
}
const char* planName(Plan plan) { return plan == Plan::Meteosat ? "Meteosat" : plan == Plan::Himawari ? "Himawari" : "GOES"; }
Plan planForCentre(double c) { return c > 0 && c < 402.0e6 ? Plan::Goes : (c > 0 ? Plan::Meteosat : Plan::Goes); }

uint8_t withParity(uint8_t c7) {
    c7 &= 0x7F;
    return (uint8_t)(c7 | ((__builtin_popcount(c7) & 1) ? 0 : 0x80));   // odd parity over all 8 bits
}
bool parityOk(uint8_t c) { return (__builtin_popcount(c) & 1) == 1; }

std::vector<uint8_t> pseudoBinaryB(const std::vector<int>& v) {
    std::vector<uint8_t> o;
    for (int x : v) o.push_back((uint8_t)(0x40 | (x & 0x3F)));
    return o;
}

std::vector<uint8_t> charBits(const std::vector<uint8_t>& bytes) {
    std::vector<uint8_t> b;
    for (uint8_t c : bytes) for (int i = 0; i < 8; i++) b.push_back((c >> i) & 1);
    return b;
}

std::vector<uint8_t> frameBits(uint32_t a21, bool ext, uint8_t flags, const std::vector<uint8_t>& data7) {
    std::vector<uint8_t> b(kSync, kSync + kSyncBits);
    const uint32_t w = addressWord(a21);
    for (int i = 30; i >= 0; i--) b.push_back((w >> i) & 1);
    if (ext) {
        b.push_back(0);
        for (int i = 0; i < 8; i++) b.push_back((flags >> i) & 1);
    }
    std::vector<uint8_t> chars;
    for (uint8_t c : data7) chars.push_back(withParity(c));
    chars.push_back(kEot);
    const auto cb = charBits(chars);
    b.insert(b.end(), cb.begin(), cb.end());
    return b;
}

bool parseBody(const std::vector<uint8_t>& bits, bool preferExt, DcsMessage& m) {
    auto scan = [&](size_t off, std::vector<uint8_t>* out, int* perr, bool* eot, int limit) {
        int good = 0, seen = 0;
        for (size_t p = off; p + 8 <= bits.size(); p += 8) {
            uint8_t c = 0;
            for (int i = 0; i < 8; i++) c |= (uint8_t)(bits[p + (size_t)i] << i);
            if (c == kEot) { if (eot) *eot = true; break; }
            if (seen >= limit) break;
            seen++;
            if (parityOk(c)) good++; else if (perr) (*perr)++;
            if (out && out->size() < kDcsBytesMax) out->push_back((uint8_t)(c & 0x7F));
        }
        return std::make_pair(good, seen);
    };
    const size_t offs[2] = {0, 9};
    int bestScore = -1, bestOff = 0;
    for (int k = 0; k < 2; k++) {
        const size_t off = offs[k];
        if (bits.size() < off) continue;
        const auto g = scan(off, nullptr, nullptr, nullptr, 16);
        // the offset whose first characters have the right parity; a tie goes to the format of the bit rate
        const int s2 = (g.second ? g.first * 1000 / g.second : 0) * 2 + ((k == 1) == preferExt ? 1 : 0);
        if (s2 > bestScore) { bestScore = s2; bestOff = (int)off; }
    }
    m.raw.clear(); m.parityErrors = 0; m.eot = false; m.flags = -1;
    if ((size_t)bestOff > bits.size()) return false;
    if (bestOff == 9) {
        int f = 0;
        for (int i = 0; i < 8; i++) f |= bits[(size_t)(1 + i)] << i;
        m.flags = f;
    }
    scan((size_t)bestOff, &m.raw, &m.parityErrors, &m.eot, (int)kDcsBytesMax);
    interpret(m);
    return !m.raw.empty() || m.eot;
}

namespace {
bool numeric(const std::string& s, double& v) {
    if (s.empty()) return false;
    char* e = nullptr;
    v = strtod(s.c_str(), &e);
    return e && *e == 0;
}
}

void interpret(DcsMessage& m) {
    m.fields.clear(); m.text.clear();
    int inRange = 0, plain = 0, total = 0;
    for (uint8_t c : m.raw) {
        if (c == 0x0A || c == 0x0D) continue;
        total++;
        if (c >= 0x40) inRange++;
        if (c == ' ' || (c >= '0' && c <= '9') || c == ',' || c == '.') plain++;
    }
    m.format = (total >= 3 && inRange * 10 >= total * 9 && plain * 20 < total) ? 1 : 0;
    if (total == 0) m.format = 0;
    if (m.format == 1) {
        for (uint8_t c : m.raw) m.text.push_back((char)c);
        char b[48];
        int n = 0;
        for (size_t i = 0; i + 3 <= m.raw.size(); i += 3) {
            const int v = ((m.raw[i] & 0x3F) << 12) | ((m.raw[i + 1] & 0x3F) << 6) | (m.raw[i + 2] & 0x3F);
            const int sv = v >= 0x20000 ? v - 0x40000 : v;
            if (sv != v) snprintf(b, sizeof b, "%d: %d (%d signed)", ++n, v, sv); else snprintf(b, sizeof b, "%d: %d", ++n, v);
            m.fields.push_back(b);
        }
        return;
    }
    for (uint8_t c : m.raw) m.text.push_back(c == 0x0A ? '\n' : (c == 0x0D ? '\n' : (c < 0x20 || c == 0x7F ? ' ' : (char)c)));
    // tokens: a label followed by a number becomes "label value"
    std::vector<std::string> tok;
    std::string cur;
    for (char c : m.text) {
        if (c == ' ' || c == ',' || c == '\n' || c == '\t') { if (!cur.empty()) tok.push_back(cur); cur.clear(); }
        else cur.push_back(c);
    }
    if (!cur.empty()) tok.push_back(cur);
    for (size_t i = 0; i < tok.size(); i++) {
        double v;
        if (!numeric(tok[i], v) && i + 1 < tok.size() && numeric(tok[i + 1], v)) { m.fields.push_back(tok[i] + " " + tok[i + 1]); i++; }
        else m.fields.push_back(tok[i]);
        if (m.fields.size() >= 64) break;
    }
}

} // namespace dcs
} // namespace dect2
