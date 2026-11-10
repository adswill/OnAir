// DVB-C (ETSI EN 300 429, ITU-T J.83 annex A; annex C is the same with a roll-off of 0.13 in 6 MHz channels): the constellations with their
// quadrant rule, the differential coding of the two most significant bits, the byte to symbol mapping, and the test transmitter.
// The outer code (energy dispersal, Reed-Solomon (204,188), Forney interleaver I = 12) is the one of DVB-T (dvbt.h) and is shared with it.
#pragma once
#include <algorithm>
#include "dect2/common/stable_random.h"
#include "dect2/common/ring.h"
#include <cmath>
#include <cstdint>
#include <functional>
#include <memory>
#include <random>
#include <vector>

namespace dect2 {
namespace dvbc {

// QAM index 0..4: 16, 32, 64, 128, 256-QAM
constexpr int kNumQam = 5;
inline int qamOrder(int q) { return 16 << q; }
inline int qamBits(int q) { return 4 + q; }
const char* qamName(int q);                     // "64-QAM"

// One constellation, unit mean energy. The symbol value is the m-tuple: bit m-1 is I_k, bit m-2 is Q_k (the quadrant), the rest the q LSBs.
// Quadrant 1 is EN 300 429 figure 7 (16, 32, 64-QAM); 256-QAM follows the same Gray rule (the LSBs alternate between the I and Q axes);
// 128-QAM: quadrant 1 is laid out like 32-QAM's, extended (figure 8 of the standard is a picture that could not be checked here).
// Quadrants 2, 3 and 4 (IQ = 10, 11, 01) are quadrant 1 turned by 90, 180 and 270 degrees (table 1), so the LSBs survive a quarter turn.
struct Constellation {
    int q = 0, order = 16, bits = 4;
    float unit = 1;                             // the grid step: points at odd multiples of unit
    int cells = 4;                              // decision grid cells per axis
    std::vector<cf32> pts;                      // by symbol value
    std::vector<uint16_t> lut;                  // cells x cells, nearest symbol of each grid cell (corners of the cross constellations included)
    float r2 = 1;                               // CMA modulus E|s|^4 / E|s|^2
    float dmin = 1;
    // The grid cell of a value (the arithmetic of decide(), without the table lookup): a loop of these over an array vectorises, which the lookup stops
    inline __attribute__((always_inline)) int cellOf(float re, float im) const {
        const float k = 1.f / (2 * unit);
        int ix = (int)(std::clamp(re * k, -64.f, 64.f) + 64.f) - 64 + cells / 2, iy = (int)(std::clamp(im * k, -64.f, 64.f) + 64.f) - 64 + cells / 2;
        ix = ix < 0 ? 0 : ix >= cells ? cells - 1 : ix;
        iy = iy < 0 ? 0 : iy >= cells ? cells - 1 : iy;
        return iy * cells + ix;
    }
    inline __attribute__((always_inline)) int decide(cf32 z) const {
        // floor() without the library call: shift to positive, truncate (cells <= 16, so +-64 cells is far outside the grid)
        const float k = 1.f / (2 * unit);
        int ix = (int)(std::clamp(z.real() * k, -64.f, 64.f) + 64.f) - 64 + cells / 2, iy = (int)(std::clamp(z.imag() * k, -64.f, 64.f) + 64.f) - 64 + cells / 2;
        ix = ix < 0 ? 0 : ix >= cells ? cells - 1 : ix;
        iy = iy < 0 ? 0 : iy >= cells ? cells - 1 : iy;
        return lut[(size_t)iy * (size_t)cells + (size_t)ix];
    }
};
const Constellation& constellation(int q);

// quadrant number (0..3, turning by 90 degrees each) of the two MSBs I Q: 00 -> 0, 10 -> 1, 11 -> 2, 01 -> 3
inline int quadOf(int iq) { static const int t[4] = {0, 3, 1, 2}; return t[iq & 3]; }
inline int iqOf(int quad) { static const int t[4] = {0, 2, 3, 1}; return t[quad & 3]; }

// Transmit mapping: bytes (after the interleaver) to symbols, MSB first, with the differential coding of section 8.
class Mapper {
public:
    explicit Mapper(int q) : c_(&constellation(q)) {}
    void push(const uint8_t* bytes, size_t n, std::vector<cf32>& out);
private:
    const Constellation* c_;
    uint32_t acc_ = 0;
    int nacc_ = 0;
    int prevQuad_ = 0;
};

// Receive: a decided symbol value to its m bits (MSB first into `bits`, one bit per byte), undoing the differential coding.
class Demapper {
public:
    void setQam(int q) { m_ = qamBits(q); prevQuad_ = 0; }
    int bits() const { return m_; }
    void push(int sym, std::vector<uint8_t>& bits) {
        const int quad = quadOf(sym >> (m_ - 2));
        const int d = (quad - prevQuad_) & 3;
        prevQuad_ = quad;
        const int v = (iqOf(d) << (m_ - 2)) | (sym & ((1 << (m_ - 2)) - 1));
        const size_t at = bits.size();
        bits.resize(at + (size_t)m_);   // one resize, then stores: a push_back per bit re-checks the capacity for each of the m bits
        uint8_t* o = bits.data() + at;
        for (int b = m_ - 1, k = 0; b >= 0; b--, k++) o[k] = (uint8_t)((v >> b) & 1);
    }
private:
    int m_ = 6, prevQuad_ = 0;
};

// Root raised cosine pulse, t in symbols, unit energy
double rrc(double t, double beta);

// transport stream bit rate of a channel
inline double netBitrate(int q, double symbolRate) { return symbolRate * qamBits(q) * 188.0 / 204.0; }

// ---- the test transmitter
struct SignalConfig {
    int qam = 2;                     // 64-QAM
    double symbolRate = 6.952e6;
    double rollOff = 0.15;           // 0.13 for annex C
    double sampleRate = 10e6;
    double snrDb = 40;               // Es/N0; 200 = no noise
    double cfoHz = 0;
    double clockPpm = 0;             // the symbol rate is higher by this many ppm
    bool inverted = false;           // spectrum turned around
    double echoDb = 0;               // 0 = off: one echo this many dB below the main path ...
    double echoUs = 0.5;             // ... this late ...
    double echoDeg = 60;             // ... with this phase
    double level = 0.2;
    uint32_t seed = 1;
    // Annex B test signal: the transmitter takes finished symbols from here (j83b.h) instead of making annex A frames
    std::function<void(std::vector<cf32>&)> symbols;
    std::function<void(uint8_t*)> ts;   // packet source; null: the demo programme
};

class Signal {
public:
    explicit Signal(const SignalConfig& cfg);
    ~Signal();
    void generate(cf32* out, size_t n);
    uint64_t packetsSent() const { return packets_; }
    const SignalConfig& config() const { return cfg_; }
private:
    void refill();
    SignalConfig cfg_;
    std::function<void(uint8_t*)> ts_;
    Mapper map_;
    struct Il;
    std::unique_ptr<Il> il_;
    std::vector<cf32> sym_;          // symbols, the oldest first
    size_t symBase_ = 0;             // index (in the symbol stream) of sym_[0]
    double t_ = 0;                   // time of the next output sample, in symbols
    double step_ = 0;
    int half_ = 12, phases_ = 256;
    std::vector<float> taps_;
    std::vector<cf32> echo_;
    size_t echoPos_ = 0;
    int echoLen_ = 0;
    cf32 echoGain_;
    double ph_ = 0, dph_ = 0;
    float noise_ = 0, scale_ = 1;
    std::mt19937 rng_;
    dect2::normal_distribution<float> nd_{0.f, 1.f};
    uint64_t packets_ = 0;
};

} // namespace dvbc
} // namespace dect2
