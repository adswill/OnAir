// CDR (China Digital Radio, GY/T 268.1-2013 "Digital audio broadcasting in FM band, part 1"): the parameters and building blocks of the
// physical layer that the transmitter (cdr_gen) and the receiver (cdr_rx) share. Clause and table numbers refer to GY/T 268.1-2013.
//
// The signal: OFDM at 816 ksps (T = 1/816000 s), sub-frames of 160 ms (a beacon and SN OFDM symbols), four sub-frames make a logical frame
// of 640 ms, four logical frames a super frame. The spectrum is built from 100 kHz sub-bands (some only half used) placed around the
// centre of an FM channel, so that an analogue FM programme can keep the middle (spectrum modes 9, 10, 22 and 23).
#pragma once
#include "ring.h"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace dect2::cdr {

constexpr double kFs = 816000.0;          // 1 / T
constexpr int kSubframeLen = 130560;      // samples of a 160 ms sub-frame
constexpr int kSiBits = 48, kSiCoded = 216, kSiSymbols = 108;

// Table 1 and the tables of clause 5 that depend on the transmission mode
struct TxParams {
    int tm;                 // 1, 2 or 3
    int ns, nb;             // FFT size of the OFDM symbols and of the sync signal
    int tu, tcp, ts;        // data body, cyclic prefix, symbol period (samples)
    int tbcp, tb;           // beacon cyclic prefix and beacon length
    int tg;                 // guard interval of the windowing (Table 15)
    int sn;                 // OFDM symbols per sub-frame
    int nv;                 // active carriers of a fully used sub-band (half of them in a half-used one)
    double df, dfb;         // carrier spacing of the OFDM symbols and of the sync signal (Hz)
    int L, nzc, m;          // beacon sequence length per sub-band (Table 13), Nzc and m of the beacon formula
    int siRows, siPerRow;   // the 108 SI symbols of a half-sub-band take siRows rows with siPerRow each (Tables 9, 10)
    int siPos[2][4];        // columns (1-based, within Ms,t) of the SI symbols: [lower, upper half][k]
    int sdisRows, sdisValid;// Table 11
    int msdsPerBand;        // MSDS per sub-band and logical frame (Table 12): the length of the MSDS interleaver
    int sdisPerBand;        // SDIS per sub-band and logical frame (Table 12)
    int q[3];               // SDI payload Q per constellation (Table A.2): [(Q + 6) NI - 6] bits per logical frame
};
const TxParams* txParams(int tm);          // nullptr for other values

enum Mod : int { kQpsk = 0, k16Qam = 1, k64Qam = 2 };
inline int modBits(int mod) { return 2 + 2 * mod; }
const char* modText(int mod);

// Table 2: the six spectrum modes
struct SpectrumMode {
    int index;              // 1, 2, 9, 10, 22, 23
    bool classA;            // class A: sub-bands centred on (100 i + 50) kHz; class B: on 100 i kHz
    int ni;                 // NI: bandwidth of the digital signal = NI x 100 kHz
    int nominalKhz;         // sub-band identified frequency (SI bits b10..b12, Table 3)
    int halves;             // active half-sub-bands, in increasing frequency
    struct Half { int band; bool upper; } half[4];   // band: DB1..DB5 or DA1..DA4 (1-based)
    int innerKhz, outerKhz; // the digital signal covers innerKhz .. outerKhz on both sides of the centre (inner 0: one block)
    const char* text;
};
const SpectrumMode* spectrumMode(int index);   // nullptr for reserved indices
const std::vector<int>& spectrumModeIndices(); // 1, 2, 9, 10, 22, 23
int nominalCode(int khz);                      // Table 3: 0 -> 0, 50 -> 1, 100 -> 2, 150 -> 3, 200 -> 4

// Annex C: carrier numbers (FFT bins around the signal centre) of one half-sub-band, ascending; sync = the beacon's sync signal
std::vector<int> halfCarriers(int tm, bool classA, int band, bool upper, bool sync);

// Kinds of the elements of the sub-carrier matrix (5.6)
enum : uint8_t { kElemData = 0, kElemSi = 1, kElemPilot = 2 };

// Everything about where the symbols of one transmission mode / spectrum mode go
struct Layout {
    int tm = 0, sm = 0, ni = 0, nv = 0, cols = 0, sn = 0;
    const TxParams* tp = nullptr;
    const SpectrumMode* spec = nullptr;
    std::vector<int> carrier;           // per matrix column (0-based, Nv NI of them): its OFDM carrier number
    std::vector<int> syncCarrier;       // L NI sync signal carriers, ascending
    std::vector<cf32> beaconSeq;        // Pb(n) on them (5.9.1)
    // one sub-frame (SN rows x cols), the same in all four
    std::vector<uint8_t> kind;
    std::vector<int16_t> siSym;         // SI symbol number 0..107 (or -1)
    std::vector<int16_t> siHalf;        // half-sub-band carrying it: 2 t + (upper), t = sub-band of the matrix (or -1)
    std::vector<cf32> pilot;            // scattered pilot values (0 elsewhere)
    // logical frame: element p = q * SN * cols + row * cols + col, q the logical sub-frame 0..3
    std::vector<int> sdisPos;           // where the SDIS go, in order
    std::vector<int> msdsPos;           // where MSDS m ends up after the interleaving of 5.7
    int elems() const { return 4 * sn * cols; }
    int codewordsFor(int mod) const { return (int)msdsPos.size() * modBits(mod) / 9216; }
    int sdiBits(int mod) const { return (tp->q[mod] + 6) * ni - 6; }        // SDI payload bits per logical frame
    int msdBits(int mod, int rate) const;                                    // service data payload bits per logical frame (Table A.1 times NI)
};
std::shared_ptr<const Layout> layoutFor(int tm, int sm);   // cached; nullptr for invalid modes

// 5.10 sub-frame allocation: which logical frame p (0..3 in the super frame) and logical sub-frame q the physical sub-frame `sub` of
// physical frame `frame` carries, and the other way round. alloc 1..3.
void physToLogical(int alloc, int frame, int sub, int& p, int& q);
void logicalToPhys(int alloc, int p, int q, int& frame, int& sub);

// 5.3.1: v_n = u_R(n) for a block of nmux
const std::vector<int>& interleaver(int nmux);

// 5.4.1 constellations. bits: modBits(mod) values (0/1); beta: 1 for the data, sqrt 2 for the SI and the pilots (QPSK only)
cf32 mapBits(const uint8_t* bits, int mod, float beta = 1.f);
// Max-log LLRs, positive favours 0. nvar: noise variance of the complex point.
void demap(cf32 z, float nvar, int mod, float* llr, float beta = 1.f);

// 5.1 PRBS x^12 + x^11 + x^8 + x^6 + 1, initial state 100000000000, restarted at every logical frame
class Prbs {
public:
    Prbs() { reset(); }
    void reset() { s_ = 1; }
    int next() { const int out = (s_ >> 11) & 1, fb = ((s_ >> 5) ^ (s_ >> 7) ^ (s_ >> 10) ^ (s_ >> 11)) & 1; s_ = (uint16_t)(((s_ << 1) | fb) & 0xFFF); return out; }
private:
    uint16_t s_;
};
void scrambleBits(uint8_t* bits, int n);   // XOR with the PRBS from its start (scrambling and descrambling)

// 4.6 system information
struct SysInfo {
    bool multiFreq = false;  // b0 = 0: multi-frequency cooperation
    int nextFreq = 511;      // b1..b9 (all ones when not cooperating)
    int nominal = 0;         // b10..b12 (Table 3 code)
    int spec = 1;            // b13..b18 spectrum mode index
    int frame = 0;           // b19 b20: physical frame in the super frame, 0..3
    int subframe = 0;        // b21 b22: sub-frame in the physical frame, 0..3
    int alloc = 1;           // b23 b24: sub-frame allocation mode 1..3 (0 reserved)
    int sdiMod = 0, msdMod = 0;   // b25 b26, b27 b28
    int hier = 0;            // b29 b30: 0 none, 1 2 3: alpha 1, 2, 4
    bool uniform = true;     // b31
    int rateHi = 3, rateLo = 0;   // b32 b33, b34 b35 (rate index 0..3 = 1/4, 1/3, 1/2, 3/4)
};
int crc6(const uint8_t* bits, int n);                  // the CRC of 4.6 (Figure 5): returns b42..b47 packed MSB = b42
void siToBits(const SysInfo& si, uint8_t bits[kSiBits]);
bool siFromBits(const uint8_t bits[kSiBits], SysInfo& si);   // false: CRC error
// 1/4 convolutional code (5.2.1), interleaver (5.3.2) and QPSK with beta = sqrt 2: 108 symbols
void siSymbols(const SysInfo& si, cf32 out[kSiSymbols]);

// The 1/4 convolutional code of 5.2.1 (generators 133 171 145 133, 6 tail bits): 4 (n + 6) code bits
void convEncode(const uint8_t* bits, int n, uint8_t* code);
// Soft Viterbi: llr[4 (n + 6)] (positive favours 0), returns the n information bits
void convDecode(const float* llr, int n, uint8_t* bits);

// CRCs of GY/T 268.2 Annex C (MSB first, registers start at all ones, the result is sent inverted)
uint8_t crc8(const uint8_t* d, size_t n);
uint32_t crc32(const uint8_t* d, size_t n);

} // namespace dect2::cdr
