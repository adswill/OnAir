// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Wi-Fi HaLow (IEEE 802.11ah, S1G) physical layer, receive and test-signal side: the OFDM layouts of the 1, 2, 4, 8 and 16 MHz channels, the
// three preambles (S1G_1M, S1G_SHORT, S1G_LONG), the SIG fields (CRC-4), scrambler, BCC (rate 1/2 with 2/3, 3/4, 5/6 puncturing), interleaver,
// BPSK to 256-QAM mapping, MCS 0 - 10 and the PPDU builder. Reference: IEEE 802.11-2020 clause 23 (S1G PHY), derived from the 802.11ac VHT PHY
// clocked down by 10: the tone spacing is 31.25 kHz, the symbol 40 us (8 us guard interval, 4 us short GI).
//
// One tone grid for every width: tone k of a channel is FFT bin k whatever the sample rate, so a 1, 2 or 4 MHz channel is handled on a 128-point
// grid at 4 Msps (the narrow grid, kRate), 8 MHz on 256 points at 8 Msps, 16 MHz on 512 points at 16 Msps (2 x 8 MHz, the 11ac 160 MHz way).
// The tone indices k of a width do not depend on the grid it is sampled on.
//
// Honesty note: the standard was written down from memory and public descriptions; no live 802.11ah capture and no copy of the standard were
// available. Every table or rule that could not be checked is marked "unverified" in halow_phy.cpp / halow_sig.cpp / halow_ppdu.cpp and listed in
// core/src/halow/README.md. LDPC coding is recognised in the SIG field but not decoded (the 802.11 LDPC parity-check matrices were not available).
#pragma once
#include "dect2/common/ring.h"
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace dect2::halow {

constexpr double kRate = 4e6;            // sample rate of the narrow grid (1, 2, 4 MHz channels)
constexpr int kFft = 128;                // narrow grid FFT size
constexpr int kGi = 32;                  // 8 us guard interval on the narrow grid
constexpr int kSym = kFft + kGi;         // 40 us
constexpr int kLag = 32;                 // the STF repeats every 8 us (32 samples on the narrow grid)
enum Width { W1 = 0, W2 = 1, W4 = 2, W8 = 3, W16 = 4, kNumWidths = 5 };
inline int widthMhz(int w) { return 1 << w; }
inline int nfft(int w) { return w <= W4 ? 128 : w == W8 ? 256 : 512; }          // FFT size of the grid the width is handled on
inline double gridRate(int w) { return nfft(w) * 31250.0; }                     // 4, 4, 4, 8, 16 Msps
// Smallest radio sample rate that holds the channel (its occupied band must fit), and the rate the app asks for (clause 23: occupied tones x 31.25 kHz)
inline double minRateHz(int w) { static const double r[kNumWidths] = {2e6, 2e6, 4e6, 8e6, 16e6}; return r[w < 0 ? 0 : w > 4 ? 4 : w]; }
inline double recommendedRateHz(int w) { static const double r[kNumWidths] = {2e6, 4e6, 5e6, 10e6, 20e6}; return r[w < 0 ? 0 : w > 4 ? 4 : w]; }
inline int maxWidthMhz(double rate) { int m = 0; for (int w = 0; w < kNumWidths; w++) if (rate >= minRateHz(w) - 1) m = widthMhz(w); return m; }

enum Preamble { PreShort = 0, PreLong = 1, Pre1M = 2 };   // S1G_SHORT, S1G_LONG, S1G_1M
const char* preambleName(int p);

// Tones of a channel width (tone index k, FFT bin (k + N) % N on the grid of the width)
struct Layout {
    std::vector<int> dataK;              // 0 = a tone that does not exist in this copy (dropped at the band edge)
    std::vector<int> pilotK;
};
struct WidthInfo {
    int w = 0;
    int N = 128;                         // grid FFT size
    int usedHi = 0;                      // highest used tone (13, 28, 58, 122, 250)
    int nUsed = 0;                       // tones carrying something (data + pilots): 26, 56, 114, 242, 484
    int nsd = 0;                         // data tones: 24, 52, 108, 234, 468
    int ncbps = 0;                       // coded bits per symbol at BPSK (= nsd)
    int ncol = 0, nrow = 0;              // interleaver at BPSK; rows grow with the bits per tone (clause 23.3.11.8)
    int sigNcbps = 0, sigNcol = 0, sigNrow = 0;   // the same for the SIG field symbols (the 2 MHz layout in every 2 MHz of the channel)
    Layout data;                         // the data field
    std::vector<Layout> sig;             // the SIG field: one layout, or one 2 MHz copy per 2 MHz of the channel
    int sigSyms = 0;                     // OFDM symbols of the SIG field: 6 (1 MHz), 2
    int sigInfoBits = 0;                 // 36 (1 MHz), 52, with the CRC and the tail
    int stfSamples = 0;                  // on the grid of the width: 160 us (1 MHz) or 80 us
    int ltfSyms = 0;                     // 1 MHz: 4 symbols of GI + LTS; 2 MHz and up: a double GI and two LTS
};
const WidthInfo& info(int w);
inline int bin(int k, int n = kFft) { return (k + n) % n; }

// Preamble spectra and time signals on the grid of the width (N = nfft(w) bins or samples)
void stfSpectrum(int w, cf32* x);        // N bins
void ltfSpectrum(int w, float* l);       // N bins: +-1 on the used tones, 0 elsewhere
float stfScale(int w);                   // time-domain scale giving unit power
float symScale(int w);                   // time-domain scale of the LTF and of every data symbol: 1 / sqrt(nUsed)
void ltfTime(int w, cf32* x);            // the N samples of one LTS
void stfTime(int w, cf32* x);            // the N samples (four periods) of the STF

// bits, coding
uint8_t crc4(const uint8_t* bits, size_t n);                     // init ones, result inverted; bits in transmit order
std::vector<uint8_t> scramblerSeq(uint8_t seed, size_t n);       // the x^7 + x^4 + 1 sequence from a 7 bit state
std::vector<uint8_t> scramblerFrom(const uint8_t* first7, size_t n);   // the sequence that started with these 7 bits
int pilotPolarity(int n);                                        // +1 / -1, the 127 long sequence
std::vector<uint8_t> convEncode(const std::vector<uint8_t>& bits);     // rate 1/2, K = 7, 133 / 171 octal
std::vector<uint8_t> viterbi(const std::vector<float>& llr);           // llr > 0: bit 1; llr.size() / 2 bits out (0 = erased)
inline int interleave(int ncol, int nrow, int k) { return nrow * (k % ncol) + k / ncol; }   // first permutation: coded bit k of a symbol -> its position
// Both permutations of the BCC interleaver for one spatial stream (clause 23.3.11.8): k -> j with s = max(1, nbpscs / 2)
int interleaveJ(int ncol, int nrow, int ncbps, int nbpscs, int k);
std::vector<uint8_t> puncture(const std::vector<uint8_t>& coded, int num, int den);                 // rate num/den from the 1/2 stream
std::vector<float> depuncture(const std::vector<float>& llr, int num, int den, size_t halfRateLen); // erasures (0) back in
// Constellation: Gray mapped square QAM with 802.11 normalisation. nb bits per tone (1, 2, 4, 6, 8)
cf32 mapQam(const uint8_t* bits, int nb);
void demapQam(cf32 z, float nvar, int nb, float* llr);           // z equalised, nvar its noise variance; llr > 0: bit 1

// MCS (clause 23.5): 0 BPSK 1/2, 1 QPSK 1/2, 2 QPSK 3/4, 3 16-QAM 1/2, 4 16-QAM 3/4, 5 64-QAM 2/3, 6 64-QAM 3/4, 7 64-QAM 5/6, 8 256-QAM 3/4,
// 9 256-QAM 5/6, 10 BPSK 1/2 with 2x repetition (1 MHz only)
struct Mcs { int nbpscs, num, den; bool rep; const char* name; };
const Mcs& mcsInfo(int mcs);
bool mcsValid(int w, int mcs, int nss);                          // the combinations the standard allows (unverified exceptions listed in halow_phy.cpp)
int dataBitsPerSym(int w, int mcs, int nss = 1);                 // NDBPS of the whole PPDU (all streams); 0: invalid
int encoders(int w, int mcs, int nss = 1);                       // BCC encoders N_ES
int dataSymbols(int w, int mcs, int len, int nss = 1, bool stbc = false);
double dataRateMbps(int w, int mcs, int nss, bool sgi);          // for the results table

// SIG field
struct Sig {
    int w = 0;                           // channel width the PPDU was sent in
    int pre = PreShort;                  // preamble
    int mcs = 0;
    int len = 0;                         // PSDU bytes (symbols when agg)
    bool agg = false;
    int nsts = 0;                        // space-time streams - 1
    bool sgi = false;
    int id = 0;                          // partial AID (2 MHz and up)
    bool ul = false, stbc = false;
    bool ldpc = false, ldpcExtra = false;
    bool smoothing = true, travelPilots = false, ndp = false, beamformed = false;
    int resp = 0;                        // response indication
    int nss() const { return stbc ? (nsts + 1) / 2 : nsts + 1; }
};
std::vector<uint8_t> sigBits(const Sig& s);                      // with CRC-4 and tail
bool parseSig(const uint8_t* bits, int w, Sig& s, int pre = -1); // false: CRC, bandwidth field or MCS / NSS check failed
int ltfCount(int nsts);                                          // long training fields for the space-time streams: 1, 2, 4, 4
int extraSyms(int pre, int nsts);                                // full-GI symbols between the SIG field and the data (further LTFs, or D-STF, D-LTFs and SIG-B)
int symLen(int w, bool sgi);                                     // samples of one data symbol on the grid of the width (guard 8 us, or 4 us)

// The PPDU
struct PpduSpec {
    int w = W1;
    int pre = -1;                        // -1: S1G_1M at 1 MHz, S1G_SHORT otherwise
    int mcs = 0;
    int nss = 1;                         // > 1, stbc or ldpc: preamble and SIG field as announced, the data field is filler (the monitor cannot decode it either)
    bool stbc = false, ldpc = false, sgi = false, agg = false, beamformed = false, ndp = false, ul = false;
    int id = 0;
    uint8_t seed = 0x5D;                 // scrambler state
    double beamTilt = 0.0;               // beamformed: phase slope (radians per tone) applied to the data, the D-LTF and SIG-B (not the preamble before them)
    std::vector<uint8_t> psdu;
};
std::vector<cf32> buildPpdu(const PpduSpec& s);                  // samples on the grid of the width
size_t ppduSamples(const PpduSpec& s);
int dataBitsPerSym(const PpduSpec& s);
// Old narrow entry points (BPSK / MCS 10 beacons)
std::vector<cf32> buildPpdu(const std::vector<uint8_t>& psdu, int w, int mcs, uint8_t seed = 0x5D, bool agg = false);
size_t ppduSamples(int w, int mcs, int len);

} // namespace dect2::halow
