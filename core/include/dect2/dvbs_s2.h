// DVB-S2 (EN 302 307-1) building blocks shared by the transmitter (test signal), the receiver and the tests: code parameters, MODCOD table,
// BBHEADER, BB scrambler, bit interleaver, constellations, soft demapper, PLHEADER and PL scrambling.
// Clause numbers refer to ETSI EN 302 307 V1.3.1.
#pragma once
#include "ldpc.h"
#include "ring.h"
#include "t2fec.h"
#include <bit>
#include <complex>
#include <cstdint>
#include <vector>

namespace dect2 {
namespace dvbs {

// ---- code rates, in the order of the LDPC tables
constexpr int kS2Rates = 11;                       // 1/4 1/3 2/5 1/2 3/5 2/3 3/4 4/5 5/6 8/9 9/10
const char* s2RateName(int rate);
double s2RateValue(int rate);                      // k/n as a number (1/4 ... 9/10)
enum S2Mod { kQpsk = 0, k8psk = 1, k16apsk = 2, k32apsk = 3 };
const char* s2ModName(int mod);
inline int s2BitsPerSymbol(int mod) { return mod + 2; }

// ---- code dimensions (tables 5a, 5b, 7a, 7b)
struct S2Dims {
    bool ok = false;           // false: this combination does not exist (short 9/10, a modulation without code rates, ...)
    int nldpc = 0, kldpc = 0;  // LDPC code; kldpc = BCH coded block
    int kbch = 0, t = 0;       // BCH uncoded block and its error correction
    int q = 0;
    int bitsPerSym = 0;
    int xfecSymbols = 0;       // symbols of the XFECFRAME
    int slots = 0;             // S, table 11
};
S2Dims s2Dims(int mod, int rate, bool shortFrame);
// PLFRAME length in symbols: PLHEADER, slots and pilot blocks (clause 5.5)
int s2FrameSymbols(int mod, bool shortFrame, bool pilots);
inline int s2FrameSymbols(const S2Dims& d, bool pilots) { return 90 * (d.slots + 1) + (pilots ? 36 * ((d.slots - 1) / 16) : 0); }

// Es/N0 in dB for quasi error free reception over AWGN (table 13, ideal demodulator, 50 LDPC iterations); short frames: 0.25 dB more (the standard
// says "an additional degradation of 0,2 dB to 0,3 dB"). 99 when the combination does not exist.
double s2QefEsN0(int mod, int rate, bool shortFrame);

// ---- MODCOD (table 12): 1..28 are the modulation and code rate pairs, 0 is a dummy PLFRAME
int s2Modcod(int mod, int rate);                   // -1 when there is no such MODCOD
bool s2ModcodSplit(int modcod, int& mod, int& rate);

// ---- codes. The LDPC tables are the DVB-S2 ones (they differ from DVB-T2 for normal 2/3 and short 3/5). The BCH code is shared with DVB-T2.
const LdpcCode& s2Ldpc(int rate, bool shortFrame);
const BchCode& s2Bch(int rate, bool shortFrame);
// Normalisation factor of the min-sum decoder for this code rate. The T2 default (0.78) fails on the low rates (1/4 to 2/5), which is why it
// is chosen per rate: measured with tests/test_dvbs_s2fec.cpp-style sweeps, within about 0.3 dB of the Es/N0 of table 13.
float s2LdpcAlpha(int rate);
// BBFRAME (kbch bits, already scrambled) -> FECFRAME (nldpc bits, before the bit interleaver)
void s2EncodeFec(const std::vector<uint8_t>& bb, int rate, bool shortFrame, std::vector<uint8_t>& fec);
// Hard bits of an LDPC codeword (nldpc) -> BCH decoded BBFRAME (kbch bits). Returns the number of corrected bits, or -1.
int s2BchDecode(const uint8_t* hard, int rate, bool shortFrame, std::vector<uint8_t>& bb);

// ---- bit interleaver (clause 5.3.3), 8PSK, 16APSK and 32APSK only: written by columns, read by rows
// perm[j] = column that carries label bit j (0 = MSB of the label)
void s2InterleavePattern(int mod, int rate, int cols[5]);
void s2BitInterleave(const std::vector<uint8_t>& in, int mod, int rate, std::vector<uint8_t>& out);
void s2BitDeinterleaveLlr(const float* in, int n, int mod, int rate, float* out);   // LLRs of label bits in symbol order -> LLRs in codeword order

// ---- constellations (clause 5.4). Index = label, bit 0 (MSB) first. Unit average symbol energy.
const cf32* s2Constellation(int mod, int rate);
int s2ConstellationSize(int mod);
// Maps a FECFRAME (after the bit interleaver) to symbols
void s2MapBits(const uint8_t* bits, int nbits, int mod, int rate, cf32* out);
// Max-log demapper. sigma2 is the noise variance per real dimension. llr[i * m + b] > 0 means bit 0.
void s2Demap(const cf32* sym, int n, int mod, int rate, float sigma2, float* llr);

// ---- BBHEADER (clause 5.1.6)
struct S2BbHeader {
    int tsGs = 3;              // 3 transport stream, 0 generic packetized, 1 generic continuous, 2 reserved
    bool sis = true;           // single input stream
    bool ccm = true;           // constant coding and modulation (false: ACM or VCM)
    bool issyi = false, npd = false;
    int ro = 0;                // 0: 0.35, 1: 0.25, 2: 0.20, 3: reserved
    int isi = 0;
    int upl = 188 * 8, dfl = 0, sync = 0x47, syncd = 0;
    bool crcOk = false;
};
void s2BuildBbHeader(const S2BbHeader& h, uint8_t* bits80);       // one bit per byte
bool s2ParseBbHeader(const uint8_t* bits80, S2BbHeader& h);        // false when the CRC-8 does not match
uint8_t s2Crc8(const uint8_t* bytes, int n);                       // g(X) = X^8+X^7+X^6+X^4+X^2+1, clause 5.1.4
// BB scrambling (clause 5.2.2): XOR in place with the PRBS 1 + X^14 + X^15, which restarts at every BBFRAME
void s2BbScramble(uint8_t* bits, int n);

// ---- physical layer framing (clause 5.5)
constexpr uint32_t kSof = 0x18D2E82;               // 26 bits
// 64 coded and scrambled PLS bits for MODCOD (5 bits), short frame, pilots. The PLS code is a (64,7) code.
uint64_t s2PlsCode(int modcod, bool shortFrame, bool pilots);      // bit 63 is the first transmitted bit
// The 90 PLHEADER symbols. Symbol j uses the pi/2 BPSK rule of clause 5.5.2
void s2PlHeader(int modcod, bool shortFrame, bool pilots, cf32* out90);
const cf32* s2SofSymbols();                        // the 26 symbols of the SOF
// Decodes the PLS from 64 symbols that are close to the transmitted ones (carrier phase and frequency already corrected).
// Correlates against all 128 codewords. Returns the best score (1 = perfect) and the runner-up in `second`.
struct PlsResult { int modcod = 0; bool shortFrame = false, pilots = false; float score = 0, second = 0; };
PlsResult s2PlsDecode(const cf32* sym64);
// The same correlation for one given code word (modcod, short, pilots), 1 = perfect
float s2PlsScore(const cf32* sym64, int modcod, bool shortFrame, bool pilots);
// pi/2 BPSK symbol for header bit y at position j (0-based) in the header
inline cf32 s2Bpsk(int j, int y) {
    const float a = (1 - 2 * y) * 0.70710678f;
    return (j & 1) ? cf32(-a, a) : cf32(a, a);
}
// Physical layer scrambling sequence (clause 5.5.4), Gold code number n: the symbol i is multiplied by exp(j R(i) pi/2).
// Returns R(i) in 0..3 for i < count.
const std::vector<uint8_t>& s2ScramblingRn(int n);
inline cf32 s2RotateByR(cf32 s, int r) {   // s * exp(j r pi/2), without a branch: r is random, so a jump table here mispredicts most of the time
    r &= 3;
    const float re = s.real(), im = s.imag();
    const bool odd = r & 1;
    const float a = odd ? im : re, b = odd ? re : im;
    // sign flips: real part for r = 1, 2; imaginary part for r = 2, 3
    const uint32_t sr = (uint32_t)(((r + 1) >> 1) & 1) << 31, si = (uint32_t)((r >> 1) & 1) << 31;
    return cf32(std::bit_cast<float>(std::bit_cast<uint32_t>(a) ^ sr), std::bit_cast<float>(std::bit_cast<uint32_t>(b) ^ si));
}

} // namespace dvbs
} // namespace dect2
