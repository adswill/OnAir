// ISDB-T (ARIB STD-B31): parameters, carrier layout, pilots, TMCC and the shared parts of the channel-coding chain.
// The transmit-side stages are used by the signal generator and the tests; the receiver uses the same tables and the inverse stages.
#pragma once
#include <array>
#include <complex>
#include <cstdint>
#include <string>
#include <vector>

namespace dect2 {
namespace isdbt {

using cf32 = std::complex<float>;

enum Mod { kDqpsk = 0, kQpsk = 1, k16Qam = 2, k64Qam = 3 };     // the codes of the TMCC carrier modulation field
enum Guard { kGi32 = 0, kGi16 = 1, kGi8 = 2, kGi4 = 3 };        // guard interval 1/32, 1/16, 1/8, 1/4 of the useful symbol
enum Rate { kR12 = 0, kR23 = 1, kR34 = 2, kR56 = 3, kR78 = 4 }; // convolutional coding rate (the TMCC codes)

constexpr int kSegments = 13;
constexpr int kSymbolsPerFrame = 204;
constexpr double kSampleRate = 512e6 / 63.0;      // IFFT sampling frequency, 8.126984 MHz
constexpr float kPilotAmp = 4.f / 3.f;

// One hierarchical layer: how many of the 13 segments it uses and how they are coded. segments == 0: the layer is not used.
struct Layer {
    int segments = 0;
    int mod = kQpsk;
    int rate = kR12;
    int ti = 0;          // time-interleaving length code: 0..3 = I of 0, 4, 8, 16 (mode 1), 0, 2, 4, 8 (mode 2), 0, 1, 2, 4 (mode 3)
    bool used() const { return segments > 0; }
    bool operator==(const Layer& o) const { return segments == o.segments && (segments == 0 || (mod == o.mod && rate == o.rate && ti == o.ti)); }
};

struct Params {
    int mode = 3;                 // 1, 2, 3: carrier spacing about 4, 2, 1 kHz
    int guard = kGi8;
    bool partial = false;         // segment 0 is a partial-reception (one-segment) layer: it is layer A and has one segment
    Layer layer[3];               // A, B, C: consecutive segment numbers, ascending
    int totalSegments() const { return layer[0].segments + layer[1].segments + layer[2].segments; }
    bool operator==(const Params& o) const { return mode == o.mode && guard == o.guard && partial == o.partial && layer[0] == o.layer[0] && layer[1] == o.layer[1] && layer[2] == o.layer[2]; }
    bool valid(std::string* why = nullptr) const;
};

// ---- geometry
inline int fftN(int mode) { return 2048 << (mode - 1); }
inline int carriersPerSegment(int mode) { return 108 << (mode - 1); }
inline int dataPerSegment(int mode) { return 96 << (mode - 1); }
inline int totalCarriers(int mode) { return kSegments * carriersPerSegment(mode) + 1; }
inline int centerCarrier(int mode) { return (totalCarriers(mode) - 1) / 2; }
inline int guardSamples(int mode, int guard) { return fftN(mode) >> (5 - guard); }
inline int symbolSamples(int mode, int guard) { return fftN(mode) + guardSamples(mode, guard); }
inline double frameSeconds(int mode, int guard) { return (double)symbolSamples(mode, guard) * kSymbolsPerFrame / kSampleRate; }
inline int bitsPerCell(int mod) { return mod == k64Qam ? 6 : mod == k16Qam ? 4 : 2; }
int interleavingLength(int mode, int ti);                      // I of the time interleaver
// Delay of the time interleaver and the delay adjustment before it (Table 3-12) add up to a whole number of frames; in symbols
int timeInterleaveAdjust(int mode, int ti);
const char* guardName(int g);
const char* modName(int m);
const char* rateName(int r);
// Transport-stream packets per frame that a layer carries (Table 3-3 of the standard, times the number of segments)
int packetsPerFrame(int mode, const Layer& l);
// Net bit rate of one layer
double layerBitrate(const Params& p, int layer);
double totalBitrate(const Params& p);

// ---- segments
// Segment numbers 0..12 sit at the frequency positions (low to high): 11 9 7 5 3 1 0 2 4 6 8 10 12
extern const int kSegmentAtPosition[kSegments];
int positionOfSegment(int seg);
struct SegmentInfo {
    int layer = -1;          // 0..2, -1 for a segment no layer uses
    bool diff = false;       // differentially modulated (DQPSK)
    bool partial = false;    // segment 0 as the partial-reception layer
    int group = 0;           // frequency-interleaving group: 0 partial reception, 1 differential, 2 synchronous
    int index = 0;           // position inside the group, ascending segment number
    int groupSize = 1;
};
// Fills info[segment number]. Layers take consecutive segment numbers in the order A, B, C. Returns false for an impossible configuration.
bool segmentLayout(const Params& p, SegmentInfo info[kSegments]);

// ---- carrier roles inside a segment
enum Role : uint8_t { kData = 0, kSP = 1, kCP = 2, kTMCC = 3, kAC1 = 4, kAC2 = 5 };
// Roles of the carriers of segment `seg` in OFDM symbol `symIdx` (0..203): carriersPerSegment(mode) entries
void segmentRoles(int mode, int seg, bool diff, int symIdx, uint8_t* roles);
// Pseudo-random sequence W_i of segment `seg`, one value per carrier of the segment (0 or 1)
const std::vector<uint8_t>& prbsW(int mode, int seg);
inline float pilotValue(uint8_t w) { return w ? -kPilotAmp : kPilotAmp; }
// the carrier added at the top of the band: always modulated the same way
inline float lastCarrierValue(int mode) { return mode == 1 ? -kPilotAmp : kPilotAmp; }

// ---- TMCC
struct LayerInfo { int mod = 7, rate = 7, ti = 7, segments = 15; };   // the raw 3+3+3+4 bit fields; all ones: layer not used
struct Tmcc {
    int sysId = 0;                // 0 ISDB-T, 1 ISDB-TSB
    int switching = 15;           // countdown of parameter switching, 15: normal
    bool emergency = false;
    bool partial = false;
    LayerInfo cur[3];
    bool nextPartial = true;
    LayerInfo next[3];            // all ones: no next information
};
constexpr int kTmccInfoBits = 102;
void tmccPack(const Tmcc& t, uint8_t bits[kTmccInfoBits]);
bool tmccUnpack(const uint8_t bits[kTmccInfoBits], Tmcc& t);
// Parity of B122..B203 for the information bits B20..B121 (shortened code (184,102) of the difference cyclic code (273,191))
void tmccParity(const uint8_t info[kTmccInfoBits], uint8_t parity[82]);
// True if the 184 bits B20..B203 form a code word
bool tmccValid(const uint8_t bits184[184]);
// Corrects a received word using the reliabilities (larger = more reliable): tries flipping the few least reliable bits. Returns true on success.
bool tmccCorrect(uint8_t bits184[184], const float* reliability);
constexpr uint8_t kTmccSync0[16] = {0,0,1,1,0,1,0,1,1,1,1,0,1,1,1,0};
// The 204 bits B0..B203 of one frame (B0 is the reference, left at 0): evenFrame picks the synchronising word, diff the segment type
void tmccFrameBits(const uint8_t info[kTmccInfoBits], bool evenFrame, bool diffSegment, uint8_t bits[kSymbolsPerFrame]);
// Band carrier numbers of the TMCC carriers of the segment at frequency position pos (0..12)
std::vector<int> tmccCarrierList(int mode, int pos, bool diffSegment);
LayerInfo toLayerInfo(const Layer& l);
bool fromLayerInfo(const LayerInfo& li, Layer& l);
Tmcc tmccFromParams(const Params& p);
// Takes the layer parameters (not mode and guard) out of a TMCC word; false if they cannot be valid
bool paramsFromTmcc(const Tmcc& t, Params& p);

// ---- constellations
// Label bits b0 (MSB) .. b(m-1) packed as an integer; unit average power. DQPSK uses the QPSK points of the phase step.
cf32 mapLabel(int mod, unsigned label);
// Bit LLRs for one cell (llr > 0: bit 0), n0: noise variance of the cell; b0 first
void demapCell(int mod, cf32 z, float n0, float* llr);
// The same by comparing with every point (reference for the tests)
void demapCellGeneric(int mod, cf32 z, float n0, float* llr);

// ---- the tables (generated from the standard)
namespace tables {
extern const uint16_t kRandomizing1[96], kRandomizing2[192], kRandomizing3[384];
extern const uint16_t kDiffAc11[2][13], kDiffAc21[4][13], kDiffTmcc1[5][13], kSyncAc11[2][13], kSyncTmcc1[1][13];
extern const uint16_t kDiffAc12[4][13], kDiffAc22[9][13], kDiffTmcc2[10][13], kSyncAc12[4][13], kSyncTmcc2[2][13];
extern const uint16_t kDiffAc13[8][13], kDiffAc23[19][13], kDiffTmcc3[20][13], kSyncAc13[8][13], kSyncTmcc3[4][13];
extern const uint8_t kPrbsInit1[13][11], kPrbsInit2[13][11], kPrbsInit3[13][11];
} // namespace tables

} // namespace isdbt
} // namespace dect2
