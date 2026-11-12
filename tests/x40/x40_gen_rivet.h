// Test signals for the HF modes taken over from Rivet (XPA, XPA2, CROWD36, FSK200/500, FSK200/1000 and F06a, GW FSK; docs/RIVET-MODES-4.0.md).
// Written from the formats as Rivet reads them, not from the decoders here. They exist only in the tests; none is reachable from the
// transmit side of the application.
#pragma once
#include "x40_gen2.h"
#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace x40gen {

// How a signal reaches the receiver: dial error and drift, the sense of the sideband, the sender's clock error, silence around it
struct Path { double offsetHz = 0, driftHzPerSec = 0, ppm = 0, lead = 1.0, tail = 1.5; bool lower = false; };

// Phase-continuous tones: (audio frequency in Hz, duration in seconds), amplitude 1, in the upper (or lower) sideband of the complex channel
inline std::vector<cf32> toneSeq(const std::vector<std::pair<double, double>>& seg, double fs, const Path& p) {
    double total = p.lead + p.tail; for (auto& s : seg) total += s.second * (1 + p.ppm * 1e-6);
    std::vector<cf32> x((size_t)(total * fs), cf32(0, 0));
    double ph = 0, t = p.lead; size_t i = (size_t)(p.lead * fs);
    const double sg = p.lower ? -1.0 : 1.0;
    for (auto& s : seg) {
        t += s.second * (1 + p.ppm * 1e-6);
        const size_t end = std::min(x.size(), (size_t)std::llround(t * fs));
        for (; i < end; i++) {
            const double f = sg * (s.first + p.offsetHz + p.driftHzPerSec * (double)i / fs);
            ph += 2 * kPi * f / fs; if (ph > kPi) ph -= 2 * kPi; else if (ph < -kPi) ph += 2 * kPi;
            x[i] = cf32((float)std::cos(ph), (float)std::sin(ph));
        }
    }
    return x;
}
// noise for a signal to noise ratio in 3 kHz (the signal has power 1)
inline void addSnr(std::vector<cf32>& x, double snrDb, double fs, unsigned seed) { addNoise(x, noiseFor(snrDb, 3000, fs), seed); }

inline std::vector<std::string> randomGroups(unsigned seed, int n, bool letters = false) {
    std::mt19937 r(seed); std::vector<std::string> g;
    for (int i = 0; i < n; i++) { std::string s; for (int k = 0; k < 5; k++) s += letters ? (char)('A' + r() % 26) : (char)('0' + r() % 10); g.push_back(s); }
    return g;
}
// how many sent groups are found, in order, among the decoded text (a group may be missed or garbled without losing the rest)
inline int groupsFound(const std::vector<std::string>& sent, const std::string& decoded) {
    std::vector<std::string> tok; std::string cur;
    for (char c : decoded + " ") { if (c == ' ' || c == '\n') { if (!cur.empty()) tok.push_back(cur); cur.clear(); } else cur += c; }
    int found = 0; size_t j = 0;
    for (const auto& g : sent) {
        for (size_t k = j; k < tok.size() && k < j + 4; k++) if (tok[k] == g) { found++; j = k + 1; break; }
    }
    return found;
}
// the text of the rows of a kind, oldest first, joined by newlines
inline std::string rowText(const dect2::X40Telemetry& t, const char* kind) {
    std::string s; for (auto it = t.rows.rbegin(); it != t.rows.rend(); ++it) if (it->kind == kind) s += it->text + "\n";
    return s;
}

// ---- XPA and XPA2: tone numbers; a character equal to the one before is sent as the repeat tone
inline std::vector<int> xpaTones(const std::vector<std::string>& groups, bool two) {
    const int kSpace = two ? 0 : 4, kRep = two ? 5 : 17, kEnd = two ? 3 : 5, hi = two ? 15 : 19, syncHi = two ? 4 : 15;
    std::vector<int> t;
    for (int i = 0; i < 5; i++) { t.push_back(0); t.push_back(0); t.push_back(hi); t.push_back(hi); }
    for (int i = 0; i < 8; i++) { t.push_back(2); t.push_back(syncHi); }
    if (!two) { t.push_back(16); t.push_back(16); }
    char prev = 0; bool prevRep = false;
    auto put = [&](char c) {
        const int tone = c == ' ' ? kSpace : 6 + (c - '0');
        if (c == prev && !prevRep) { t.push_back(kRep); prevRep = true; } else { t.push_back(tone); prevRep = false; }
        prev = c;
    };
    for (size_t g = 0; g < groups.size(); g++) { for (char c : groups[g]) put(c); put(' '); }
    t.push_back(kEnd); t.push_back(kRep);
    return t;
}
inline std::vector<cf32> xpaSignal(const std::vector<std::string>& groups, bool two, double baud, double fs, const Path& p) {
    const double base = two ? 978 : 520, sp = two ? 15.625 : 40;
    std::vector<std::pair<double, double>> seg;
    for (int k : xpaTones(groups, two)) seg.push_back({base + k * sp, 1.0 / baud});
    return toneSeq(seg, fs, p);
}

// ---- CROWD36: tone number of a letter in Rivet's letters table
inline int crowd36Tone(char c) {
    static const char* kLet = "QXWVEK BRJ*GTF*MYC*ZUL*DIH*SON*AP*";
    if (c == ' ') return 6;
    for (int i = 0; i < 34; i++) if (kLet[i] == c && c != '*' && c != ' ') return i;
    return 6;
}
inline std::vector<cf32> crowd36Signal(const std::vector<std::string>& groups, double fs, const Path& p, int syncLow = 10) {
    std::vector<std::pair<double, double>> seg;
    auto tone = [&](int k) { seg.push_back({1410 + 40.0 * k, 1.0 / 40}); };
    for (int i = 0; i < 20; i++) { tone(24); tone(syncLow); }
    tone(26);                                                     // letter shift
    for (size_t g = 0; g < groups.size(); g++) {
        for (char c : groups[g]) tone(crowd36Tone(c));
        if (g % 10 == 9) tone(18); else tone(6);                  // a line end every ten groups
    }
    tone(18);
    return toneSeq(seg, fs, p);
}

// ---- FSK200/500: ITA2 figures, start bit (space, the high tone), five bits least significant first, 1.5 stop bits (mark, the low tone)
inline int ita2Figure(char c) {
    switch (c) { case '0': return 22; case '1': return 23; case '2': return 19; case '3': return 1; case '4': return 10; case '5': return 16; case '6': return 21; case '7': return 7; case '8': return 6; case '9': return 24; case ' ': return 4; case '\r': return 8; case '\n': return 2; default: return 4; }
}
inline std::vector<cf32> f01Signal(const std::vector<std::string>& groups, double centreHz, double fs, const Path& p, bool reversed = false) {
    const double markHz = centreHz + (reversed ? 250 : -250), spaceHz = centreHz + (reversed ? -250 : 250), T = 1.0 / 200;
    std::vector<std::pair<double, double>> seg;
    for (int i = 0; i < 200; i++) seg.push_back({i % 2 ? spaceHz : markHz, T});   // one second of reversals
    seg.push_back({markHz, 4 * T});
    auto chr = [&](int code) {
        seg.push_back({spaceHz, T});
        for (int b = 0; b < 5; b++) seg.push_back({(code >> b) & 1 ? markHz : spaceHz, T});
        seg.push_back({markHz, 1.5 * T});
    };
    chr(27);
    for (size_t g = 0; g < groups.size(); g++) {
        for (char c : groups[g]) chr(ita2Figure(c));
        if (g % 10 == 9) { chr(8); chr(2); } else chr(4);
    }
    chr(8); chr(2);
    seg.push_back({markHz, 10 * T});
    return toneSeq(seg, fs, p);
}

// ---- FSK200/1000: 288-bit blocks. The 16 data bytes and the CRC are spread over the nibbles of bytes 6 - 23 as Rivet unpacks them.
inline uint16_t crcX25(const uint8_t* d, int n) {   // CRC-16/X-25 (bitwise, MSB-first form with reflection)
    auto refl = [](unsigned v, int w) { unsigned r = 0; for (int i = 0; i < w; i++) if (v & (1u << i)) r |= 1u << (w - 1 - i); return r; };
    unsigned c = 0xFFFF;
    for (int i = 0; i < n; i++) { c ^= refl(d[i], 8) << 8; for (int b = 0; b < 8; b++) c = (c & 0x8000) ? ((c << 1) ^ 0x1021) & 0xFFFF : (c << 1) & 0xFFFF; }
    return (uint16_t)(refl(c, 16) ^ 0xFFFF);
}
inline std::vector<int> f06Block(int index, const uint8_t data[16], unsigned seed) {
    uint8_t f[36] = {};
    f[0] = 0x7D; f[1] = 0x12; f[2] = 0xB0; f[3] = 0xE6;
    f[4] = (uint8_t)(index >> 3); f[5] = (uint8_t)((index & 7) << 5);
    const uint16_t crc = crcX25(data, 16);
    auto hi = [&](int byte, int v) { f[byte] = (uint8_t)((f[byte] & 0x0f) | ((v & 15) << 4)); };
    auto lo = [&](int byte, int v) { f[byte] = (uint8_t)((f[byte] & 0xf0) | (v & 15)); };
    const uint8_t* d = data;
    hi(6, d[0] >> 4); hi(8, d[0]); hi(10, d[1] >> 4); hi(12, d[1]); hi(14, d[2] >> 4); hi(16, d[2]); hi(18, d[3] >> 4); hi(20, d[3]);
    hi(22, d[4] >> 4); lo(6, d[4]); lo(8, d[5] >> 4); lo(10, d[5]); lo(12, d[6] >> 4); lo(14, d[6]); lo(16, d[7] >> 4); lo(18, d[7]);
    lo(20, d[8] >> 4); lo(22, d[8]); hi(7, d[9] >> 4); hi(9, d[9]); hi(11, d[10] >> 4); hi(13, d[10]); hi(15, d[11] >> 4); hi(17, d[11]);
    hi(19, d[12] >> 4); hi(21, d[12]); hi(23, d[13] >> 4); lo(7, d[13]); lo(9, d[14] >> 4); lo(11, d[14]); lo(13, d[15] >> 4); lo(15, d[15]);
    lo(17, crc >> 12); lo(19, crc >> 8); lo(21, crc >> 4); lo(23, crc);
    std::mt19937 r(seed); for (int i = 24; i < 36; i++) f[i] = (uint8_t)(r() & 0xff);   // not used by Rivet: random here
    std::vector<int> bits; for (int i = 0; i < 288; i++) bits.push_back((f[i >> 3] >> (7 - (i & 7))) & 1);
    return bits;
}
// twelve 3-figure numbers and two figures, packed as Rivet unpacks them (10 bits each, then 4 + 4)
inline void f06Digits(const int g[14], uint8_t d[16]) {
    std::vector<int> b; auto put = [&](int v, int w) { for (int i = w - 1; i >= 0; i--) b.push_back((v >> i) & 1); };
    for (int i = 0; i < 12; i++) put(g[i], 10);
    put(g[12], 4); put(g[13], 4);
    for (int i = 0; i < 16; i++) { int v = 0; for (int k = 0; k < 8; k++) v = (v << 1) | b[(size_t)(i * 8 + k)]; d[i] = (uint8_t)v; }
}
inline std::vector<cf32> f06Signal(const std::vector<std::vector<int>>& blocks, double centreHz, double fs, const Path& p, bool reversed = false) {
    const double oneHz = centreHz + (reversed ? -500 : 500), zeroHz = centreHz + (reversed ? 500 : -500), T = 1.0 / 200;
    std::vector<std::pair<double, double>> seg;
    for (int i = 0; i < 200; i++) seg.push_back({i % 2 ? oneHz : zeroHz, T});
    for (const auto& blk : blocks) for (int b : blk) seg.push_back({b ? oneHz : zeroHz, T});
    for (int i = 0; i < 20; i++) seg.push_back({i % 2 ? oneHz : zeroHz, T});
    return toneSeq(seg, fs, p);
}

// ---- GW FSK: 100 Bd, the low tone is 1. A packet: 10, type (4 bits), count bit, subtype (7 bits), six bytes, then 8 more bits (their
// meaning is not known; Rivet needs more than 63 bits). A channel marker: bytes with 0x38 0xA3 at byte 1, 0xF2 x3 at 8 - 10, the station x6, 0xFF.
inline std::string gwBytes(const std::vector<int>& v) { std::string s; for (int x : v) for (int i = 7; i >= 0; i--) s += ((x >> i) & 1) ? '1' : '0'; return s; }
inline std::string gwPacket(int type, int count, int sub, const std::vector<int>& six) {
    std::string s = "10";
    for (int i = 3; i >= 0; i--) s += ((type >> i) & 1) ? '1' : '0';
    s += count ? '1' : '0';
    for (int i = 6; i >= 0; i--) s += ((sub >> i) & 1) ? '1' : '0';
    s += gwBytes(six) + "11001010";
    return s;
}
inline std::string gwMarker(int station) {
    std::vector<int> f = {0x55, 0x38, 0xA3, 0x55, 0x55, 0x55, 0x55, 0x55, 0xf2, 0xf2, 0xf2};
    for (int i = 0; i < 6; i++) f.push_back(station);
    f.push_back(0xff); f.push_back(0x55); f.push_back(0x55);
    return gwBytes(f);
}
// the text characters Rivet knows (the first code of each)
inline int gwCode(char c) {
    switch (c) { case '0': return 0x60; case '1': return 0x20; case '2': return 0x40; case '3': return 0x00; case '4': return 0x70; case '5': return 0x30; case '6': return 0x50; case '7': return 0x10; case '8': return 0x68; case '9': return 0x28;
    case '<': return 0x78; case ',': return 0x2e; case '.': return 0x5c; case '$': return 0x74; case '*': return 0x4c; case 'A': return 0x27; case 'B': return 0x47; case 'E': return 0x37; case 'G': return 0x17; case 'L': return 0x7f; case 'N': return 0x5f; case 'O': return 0x1f; case 'W': return 0x13; default: return 0x60; }
}
// a ship MMSI coded as Rivet's displayGW_ShipMMSI reads it: digit i is a nibble whose low three bits give the digit; 1 / 9 and 0 / 8 differ
// only in the top bit of a neighbour (the low nibble's digit looks at the next byte's high nibble, the high nibble's at its own low nibble)
inline std::vector<int> gwShipMmsi(const std::string& digits) {
    auto base = [](char c) { switch (c) { case '3': return 0; case '7': return 1; case '1': case '9': return 2; case '5': return 3; case '2': return 4; case '6': return 5; case '0': case '8': return 6; default: return 7; } };
    auto alt = [](char c) { return c == '9' || c == '8'; };
    std::string d = digits; while (d.size() < 12) d += '3';
    std::vector<int> lo(6), hi(6);
    for (int a = 0; a < 6; a++) { lo[(size_t)a] = base(d[(size_t)(2 * a)]); hi[(size_t)a] = base(d[(size_t)(2 * a + 1)]); }
    for (int a = 0; a < 6; a++) {
        if (alt(d[(size_t)(2 * a + 1)])) lo[(size_t)a] |= 8;            // the high nibble's digit reads the top bit of the low nibble
        if (a < 5 && alt(d[(size_t)(2 * a)])) hi[(size_t)a + 1] |= 8;   // the low nibble's digit reads the top bit of the next high nibble
    }
    std::vector<int> v; for (int a = 0; a < 6; a++) v.push_back((hi[(size_t)a] << 4) | lo[(size_t)a]);
    return v;
}
// bursts of bits with gaps of silence between them
inline std::vector<cf32> gwSignal(const std::vector<std::string>& bursts, double centreHz, double fs, const Path& p, double gap = 0.6) {
    const double oneHz = centreHz - 100, zeroHz = centreHz + 100, T = 1.0 / 100;
    std::vector<cf32> x;
    for (size_t k = 0; k < bursts.size(); k++) {
        std::vector<std::pair<double, double>> seg;
        for (char c : bursts[k]) seg.push_back({c == '1' ? oneHz : zeroHz, T});
        Path q = p; q.lead = k == 0 ? p.lead : gap; q.tail = k + 1 == bursts.size() ? p.tail : 0;
        auto y = toneSeq(seg, fs, q);
        x.insert(x.end(), y.begin(), y.end());
    }
    return x;
}

// the SNR where a fraction first reaches the level, interpolated between the measured points (sorted by SNR, rising); NAN if never
inline double snrAt(const std::vector<std::pair<double, double>>& pts, double level) {
    for (size_t i = 0; i < pts.size(); i++) {
        if (pts[i].second >= level) {
            if (i == 0) return pts[0].first;
            const double a = pts[i - 1].second, b = pts[i].second;
            return pts[i - 1].first + (level - a) / std::max(1e-9, b - a) * (pts[i].first - pts[i - 1].first);
        }
    }
    return NAN;
}

} // namespace x40gen
