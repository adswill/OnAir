// DTMB (GB 20600-2006, China / Hong Kong / Macau): frame geometry and the tables that the receiver, the test signal and the tests share.
// The sources of the numbers are named in dtmb_defs.cpp. Everything here is plain data and small helpers.
#pragma once
#include <array>
#include <cstdint>
#include <vector>

namespace dect2::dtmb {

constexpr double kSymbolRate = 7560000.0;          // symbols per second, 8 MHz channel
// The same signal scaled to a 6 MHz channel (Cuba, in the IARU region 2 / American 6 MHz raster): 5.67 Msym/s, everything else the same.
inline double symbolRateFor(double bwMhz) { return bwMhz > 0 && bwMhz < 7.0 ? kSymbolRate * 6.0 / 8.0 : kSymbolRate; }
constexpr double kRollOff = 0.05;                  // square-root raised cosine
constexpr int kBody = 3780;                        // symbols (C=1) or carriers (C=3780) of a frame body
constexpr int kSiSymbols = 36;                     // system information symbols in a body
constexpr int kDataSymbols = kBody - kSiSymbols;   // 3744 = 72 * 52
constexpr int kBranches = 52;                      // symbol interleaver
constexpr int kBchN = 762, kBchK = 752;            // BCH(762,752), g(x) = x^10 + x^3 + 1
constexpr int kLdpcZ = 127;                        // circulant size of the LDPC codes
constexpr int kLdpcVars = 7493, kLdpcSent = 7488;  // code length, transmitted bits (the first five are punctured)
constexpr int kTsBits = 188 * 8;

enum class Header : int { Pn420 = 0, Pn595 = 1, Pn945 = 2 };
enum class Mapping : int { Qam4Nr = 0, Qam4 = 1, Qam16 = 2, Qam32 = 3, Qam64 = 4 };
enum class Rate : int { R04 = 0, R06 = 1, R08 = 2 };

struct HeaderInfo {
    int length;           // symbols of the frame header
    int core;             // period of the PN sequence in the header (PN595: the whole header, not periodic)
    int prefix, suffix;   // cyclic extension before and after the core (0 for PN595)
    int framesPerSuper;   // signal frames in a super-frame
    double powerRatio;    // header power relative to the body
    const char* name;
    bool cyclic() const { return prefix > 0; }
};
const HeaderInfo& headerInfo(Header h);
inline int frameLength(Header h) { return headerInfo(h).length + kBody; }

// PN chips (+1 / -1) of the header of the first frame of a super-frame, length = header length. For PN420 and PN945 the sequence is the
// m-sequence continued periodically, so the chips after the core repeat it.
const std::vector<int8_t>& pnChips(Header h);
// The PN phase of frame number `frame` of a super-frame (0 .. framesPerSuper - 1): 0 for PN595; 0, +1, -1, +2, -2, ... for the others.
int pnPhase(Header h, int frame);
// Chips of the header of a frame with this phase: out[n] = core[(n + phase) mod core]. `out` holds `length` values.
void pnHeader(Header h, int phase, int8_t* out);

// What the system information says. Exists for 4QAM-NR and 32QAM at rate 0.8 only.
struct Profile {
    Mapping map = Mapping::Qam64;
    Rate rate = Rate::R06;
    bool mode2 = false;   // interleaver mode 2 (M = 720) instead of mode 1 (M = 240)
    bool operator==(const Profile& o) const { return map == o.map && rate == o.rate && mode2 == o.mode2; }
};
bool profileValid(const Profile& p);
int siIndex(const Profile& p);                 // 3 .. 24, or -1
bool profileFromSi(int index, Profile& p);
// The 36 chips of the system information of an index: four zeros, then the 32 bit codeword (complemented for interleaver mode 2).
void siChips(int index, uint8_t* chips36);
// Logical positions (before the carrier interleaver) of the 36 system information symbols in a C=3780 body.
const std::array<int16_t, kSiSymbols>& siPositions();
// C=3780: physical carrier (FFT bin) of every logical position
const std::array<int16_t, kBody>& carrierMap();

const char* mappingName(Mapping m);
const char* rateName(Rate r);
inline int bitsPerSymbol(Mapping m) { return m == Mapping::Qam16 ? 4 : m == Mapping::Qam32 ? 5 : m == Mapping::Qam64 ? 6 : 2; }
inline int interleaverDelay(const Profile& p) { return p.mode2 ? 720 : 240; }
int bchBlocks(Rate r);                         // BCH blocks per LDPC codeword: 4, 6, 8
inline int payloadBits(Rate r) { return bchBlocks(r) * kBchK; }   // transport stream bits per codeword: 3008, 4512, 6016
inline int ldpcInfoBits(Rate r) { return bchBlocks(r) * kBchN; }  // 3048, 4572, 6096
int framesPerGroup(Mapping m);                 // signal frames that hold a whole number of codewords: 2 for 4QAM-NR and 32QAM, else 1
int codewordsPerGroup(Mapping m);              // 1, 1, 2, 5, 3
int packetsPerFrame(const Profile& p);         // transport stream packets in one signal frame
double netBitrate(Header h, const Profile& p, double symRate = kSymbolRate); // transport stream bits per second
inline double frameSeconds(Header h, double symRate = kSymbolRate) { return frameLength(h) / symRate; }

// Constellation points of a mapping with unit mean power, indexed by the label whose bit 0 is the first transmitted bit.
// 4QAM and 4QAM-NR: bit 0 -> I, bit 1 -> Q. 16QAM / 64QAM: the I label bits first (least significant first), then the Q label bits; each
// axis uses the reflected Gray code of the level number. 32QAM: the cross constellation of the standard, indexed by b0 + 2 b1 + 4 b2 + 8 b3 + 16 b4.
struct QamPoint { float re, im; };
const QamPoint* qamPoints(Mapping m);          // 4, 16, 32 or 64 entries
int qamCount(Mapping m);

// Square-root raised cosine pulse (roll-off 0.05) at time t in symbol periods, unit energy: sum over integer t of srrcPulse(t)^2 = 1 (almost)
double srrcPulse(double t);

// The (16,256,6) Nordstrom-Robinson code of 4QAM-NR: eight input bits x0 .. x7 (x0 is the most significant bit of `x`) give the parity
// byte y0 .. y7 (y0 most significant); the 16 transmitted bits are x0 .. x7, y0 .. y7.
uint8_t nrParity(uint8_t x);

} // namespace dect2::dtmb
