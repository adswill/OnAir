// Test signals for the second block of 4.0 signal modes (families 886 and up). Generators exist only in the tests; none is reachable from the transmit side.
#pragma once
#include "x40_gen.h"
#include <algorithm>
#include <cstdint>
#include <string>

namespace x40gen {

// noise power per sample that gives a carrier of amplitude 1 the stated carrier to noise ratio in a bandwidth bwHz (complex input rate fs)
inline double noiseFor(double cnrDb, double bwHz, double fs) { return std::pow(10.0, -cnrDb / 10) * fs / bwHz; }

// ---- EM4100 -------------------------------------------------------------------------------------------------------------------------------
// 64-bit frame of a 40-bit payload (8-bit version + 32-bit ID): 9 ones, ten rows of 4 bits + even parity, four column parities, stop 0
inline std::vector<int> em4100Bits(uint64_t d40) {
    std::vector<int> b(9, 1); int col[4] = {0, 0, 0, 0};
    for (int r = 0; r < 10; r++) {
        int p = 0;
        for (int c = 0; c < 4; c++) { const int bit = (int)((d40 >> (39 - (4 * r + c))) & 1); b.push_back(bit); p ^= bit; col[c] ^= bit; }
        b.push_back(p);
    }
    for (int c = 0; c < 4; c++) b.push_back(col[c]);
    b.push_back(0);
    return b;
}
struct EmTag { uint64_t id = 0x0123456789ull; double depth = 0.08, rfDiv = 64, carrierHz = 125000, clockPpm = 0, start = 0.05; bool invert = false; };
// Complex baseband at the carrier: amplitude 1 carrier loaded by depth while the Manchester half-bit is high. The edges are smoothed (one pole, 20 kHz).
inline std::vector<cf32> emTag(const EmTag& t, double fs, double seconds) {
    std::vector<cf32> x((size_t)(fs * seconds));
    const auto bits = em4100Bits(t.id);
    const double half = t.rfDiv / t.carrierHz / 2 * (1 + t.clockPpm * 1e-6);
    float y = 0; const float a = (float)(1 - std::exp(-2 * kPi * 20000 / fs));
    for (size_t i = 0; i < x.size(); i++) {
        const double tt = i / fs - t.start;
        int lvl = 0;
        if (tt >= 0) {
            const long hidx = (long)(tt / half); const long bi = (hidx / 2) % 64; const int ph = (int)(hidx % 2);
            lvl = ph == 0 ? bits[bi] : !bits[bi];
            if (t.invert) lvl = !lvl;
        }
        y += a * ((float)lvl - y);
        x[i] = cf32((float)(1 - t.depth * y), 0);
    }
    return x;
}


// ---- UHF RFID (EPC Gen2) reader commands --------------------------------------------------------------------------------------------------
using Bits = std::vector<int>;
inline void putBits(Bits& b, uint32_t v, int n) { for (int i = n - 1; i >= 0; i--) b.push_back((v >> i) & 1); }
inline void addCrc5(Bits& b) { unsigned c = 0x09; for (int x : b) { const bool f = ((c >> 4) & 1) ^ x; c = (c << 1) & 0x1F; if (f) c ^= 0x09; } putBits(b, c, 5); }
inline void addCrc16(Bits& b) { unsigned c = 0xFFFF; for (int x : b) { const bool f = ((c >> 15) & 1) ^ x; c = (c << 1) & 0xFFFF; if (f) c ^= 0x1021; } putBits(b, ~c & 0xFFFF, 16); }
inline Bits uhfQuery(int q, int session, int sel = 0, int m = 0, int trext = 0, int target = 0, int dr = 0) {
    Bits b; putBits(b, 8, 4); putBits(b, dr, 1); putBits(b, m, 2); putBits(b, trext, 1); putBits(b, sel, 2); putBits(b, session, 2); putBits(b, target, 1); putBits(b, q, 4); addCrc5(b); return b;
}
inline Bits uhfQRep(int session) { Bits b; putBits(b, 0, 2); putBits(b, session, 2); return b; }
inline Bits uhfQAdj(int session, int updn) { Bits b; putBits(b, 9, 4); putBits(b, session, 2); putBits(b, updn, 3); return b; }
inline Bits uhfAck(int rn) { Bits b; putBits(b, 1, 2); putBits(b, rn, 16); return b; }
inline Bits uhfNak() { Bits b; putBits(b, 0xC0, 8); return b; }
inline Bits uhfReqRn(int rn) { Bits b; putBits(b, 0xC1, 8); putBits(b, rn, 16); addCrc16(b); return b; }
inline Bits uhfRead(int bank, int ptr, int count, int rn) { Bits b; putBits(b, 0xC2, 8); putBits(b, bank, 2); putBits(b, ptr & 0x7F, 8); putBits(b, count, 8); putBits(b, rn, 16); addCrc16(b); return b; }
inline Bits uhfSelect(int target, int action, int bank, int ptr, const std::string& mask) {
    Bits b; putBits(b, 10, 4); putBits(b, target, 3); putBits(b, action, 3); putBits(b, bank, 2); putBits(b, ptr & 0x7F, 8); putBits(b, (int)mask.size(), 8);
    for (char c : mask) b.push_back(c == '1'); b.push_back(0); addCrc16(b); return b;
}
struct Uhf { double tari = 12.5e-6, one = 1.75, pw = 0.45, depth = 0.9, rtcalTari = 0, trcalRt = 2.0, tariPpm = 0; };
// A reader sending the commands in order, with carrier-only gaps in between. withTrcal: the frame starts with the TRcal preamble (Query). Amplitude of the unmodulated carrier is 1.
inline std::vector<cf32> uhfReader(const Uhf& u, const std::vector<std::pair<Bits, bool>>& cmds, double fs, double gapSec = 0.002, double leadSec = 0.002) {
    std::vector<std::pair<double, float>> seg;      // (duration, level)
    const double k = 1 + u.tariPpm * 1e-6, T = u.tari * k, PW = u.pw * T, lowLevel = (float)(1 - u.depth);
    seg.push_back({leadSec, 1});
    auto sym = [&](double len) { seg.push_back({len - PW, 1}); seg.push_back({PW, (float)lowLevel}); };
    for (auto& c : cmds) {
        seg.push_back({12.5e-6 * k, (float)lowLevel});      // delimiter
        sym(T);                                              // data-0
        const double rtcal = T + u.one * T; sym(rtcal);
        if (c.second) sym(rtcal * u.trcalRt);
        for (int bit : c.first) sym(bit ? u.one * T : T);
        seg.push_back({gapSec, 1});
    }
    seg.push_back({0.5, 1});   // the receiver publishes its list every 0.25 s: leave time for the last one
    double total = 0; for (auto& s : seg) total += s.first;
    std::vector<cf32> x((size_t)(total * fs) + 1);
    const float a = (float)(1 - std::exp(-2 * kPi * 500000 / fs)); float y = 1;
    size_t i = 0; double acc = 0;
    for (auto& s : seg) {
        acc += s.first; const size_t end = std::min(x.size(), (size_t)(acc * fs));
        for (; i < end; i++) { y += a * (s.second - y); x[i] = cf32(y, 0); }
    }
    for (; i < x.size(); i++) x[i] = cf32(y, 0);
    return x;
}

}
