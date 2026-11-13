// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// NB-IoT (LTE narrowband IoT), the physical layer of the downlink broadcast channels (3GPP TS 36.211 clause 10 / 36.212 clause 6 / 36.213 clause 16
// / 36.331), written from the specifications. A carrier is one 180 kHz resource block (12 subcarriers of 15 kHz). Only what a cell broadcasts to
// everyone is handled: NPSS / NSSS (the cell id), NRS (channel), NPBCH (MIB-NB) and the NPDSCH that carries SIB1-NB. Every UE-specific channel
// (NPDCCH, NPDSCH to a UE, paging), the uplink and user data are never decoded.
#pragma once
#include "dect2/cell/cell_asn.h"
#include "dect2/cell/cell_fec.h"
#include "dect2/common/ring.h"
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace dect2::nbiot {

using cell::Bits;

constexpr int kFs = 1920000;      // the rate of the grid used here: 128 samples per useful symbol
constexpr int kFft = 128;
constexpr int kSubframe = 1920;   // samples per subframe (1 ms)
constexpr int kFrame = 19200;     // samples per radio frame (10 ms)
constexpr int kSc = 12;           // subcarriers
constexpr int kSyms = 14;         // OFDM symbols per subframe (normal cyclic prefix only)

int symCp(int l);                 // cyclic prefix of symbol l (0 .. 13) at kFs: 10 for the first symbol of a slot, else 9
int symUseful(int l);             // first sample of the useful part of symbol l within the subframe

enum OpMode { kInbandSame = 0, kInbandDiff = 1, kGuardband = 2, kStandalone = 3 };
const char* opModeName(int m);

// ---------------------------------------------------------------- the messages (36.331)
struct Mib {                      // MasterInformationBlock-NB, 34 bits
    int sfnMsb = 0;               // 4 most significant bits of the SFN
    int hsfnLsb = 0;              // 2 least significant bits of the hyper SFN
    int sched = 0;                // schedulingInfoSIB1 0 .. 15
    int valueTag = 0;             // 0 .. 31
    bool ab = false;              // access barring enabled
    int mode = kStandalone;       // OpMode
    int crsSeq = 0;               // in-band, same PCI: eutra-CRS-SequenceInfo 0 .. 31
    int numCrs = 0;               // in-band, different PCI: 0 same number of E-UTRA CRS ports as NRS, 1 four
    int raster = 0;               // in-band different PCI and guard band: -7.5, -2.5, 2.5, 7.5 kHz (0 .. 3)
};
Bits mibEncode(const Mib& m);
bool mibParse(const Bits& b, Mib& m);
double rasterOffsetKhz(int idx);

struct Sib1 {                     // SystemInformationBlockType1-NB: what a phone needs to know to camp
    int hsfnMsb = 0;
    std::vector<cell::Plmn> plmns;
    uint32_t tac = 0;             // 16 bits
    uint32_t cellId = 0;          // 28 bits
    bool barred = false, intraFreqReselAllowed = true;
    int qRxLevMinDbm = -140;      // the IE value times two
    int qQualMinDb = -34;
    int pMax = -99;               // dBm, -99 absent
    int band = 0;                 // freqBandIndicator-NB (1 .. 256)
};
Bits sib1Encode(const Sib1& s, size_t padTo = 0);   // the complete BCCH-DL-SCH-Message-NB, padded with zeros to padTo bits
bool sib1Parse(const Bits& b, Sib1& s);

// Table 16.4.1.3-3 / 16.4.1.5.2-1: schedulingInfoSIB1 -> repetitions, transport block size
int sib1Reps(int sched);          // 4, 8, 16, 0 reserved
int sib1Tbs(int sched);           // 208, 328, 440, 680, 0 reserved
// Table 16.4.1.3-4: the starting radio frame (n_f mod 256) of the first of the repetitions
int sib1StartFrame(int reps, int pci);

// ---------------------------------------------------------------- signals
// NPSS: S(l) cover code and the Zadoff-Chu sequence of one symbol
int npssCover(int l);                      // l = 3 .. 13
void npssSeq(cf32 d[11]);                  // without the cover code
// NSSS: d(n), n = 0 .. 131, for a cell and mfi = (n_f / 2) mod 4
void nsssSeq(int pci, int mfi, cf32 d[132]);
// The time-domain useful part (n samples, n = 16 or 128) of one NPSS symbol without its cover code
void npssSymbol(int n, cf32* out);

// NRS (10.2.6) positions and values; port 0 / 1, symbol l of the subframe, subcarrier k
int nrsBase(int port, int l, int pci);     // (v + v_shift) mod 6 when symbol l carries NRS, else -1; the REs are base and base + 6
cf32 nrsValue(int pci, int sf, int l, int m);   // m = 0, 1
// E-UTRA CRS (an in-band cell's own reference signals), port 0 .. 3
int crsBase(int port, int l, int vshift);

// Resource elements (l, k) in mapping order (first k, then l)
std::vector<std::pair<int, int>> npbchRes(int pci);                                  // 100 REs: NRS ports 0 and 1 and CRS ports 0 .. 3 assumed present
std::vector<std::pair<int, int>> npdschRes(int pci, int nrsPorts, bool inband, int crsPorts, int crsShift);

// One subframe of one antenna port or of the received signal
struct Grid {
    cf32 v[kSyms * kSc];
    Grid() { clear(); }
    void clear() { for (auto& x : v) x = cf32(0, 0); }
    cf32& at(int l, int k) { return v[l * kSc + k]; }
    const cf32& at(int l, int k) const { return v[l * kSc + k]; }
};
// The OFDM modulator: one subframe grid -> kSubframe samples; the demodulator: x points at the subframe's first sample
void modulateSubframe(const Grid& g, cf32* out);
void demodSubframe(const cf32* x, Grid& g);

// ---------------------------------------------------------------- receiver parts
struct Chan {
    cf32 h[2][kSyms];              // per port, per symbol (flat over the 180 kHz)
    float nv = 1e-3f;              // noise variance per resource element
    float rsrp = 0;                // mean power of the NRS resource elements of port 0
    int ports = 1;
};
// Channel from the NRS of a subframe (the ports the cell uses); false when the subframe has too little pilot energy
bool estimateChannel(const Grid& y, int pci, int sf, int ports, Chan& c);
// LLRs (positive = bit 0) of the QPSK symbols on the REs, transmit diversity (SFBC) with 2 ports
void demapQpsk(const Grid& y, const Chan& c, const std::vector<std::pair<int, int>>& res, int ports, std::vector<float>& llr);

// The coded NPBCH of a 640 ms period and the scrambling; E = 1600
Bits npbchCode(const Mib& m, int ports, int pci);      // 1600 scrambled bits (the whole TTI)
// A search result
struct Meas { float levelDb = -200, snrDb = -99; };
struct Found {
    int pci = -1;
    double cfoHz = 0;            // carrier offset against the candidate centre (total, including the raster offset)
    double clockPpm = 0;
    float npssScore = 0;         // detection strength: peak of the folded NPSS metric over its mean
    float nsssScore = 0;         // NSSS match, the best identity over the mean of the others
    int nfMod8 = 0;              // n_f mod 8 of frame 0 of the block (0 .. 7), by the NSSS shift
};
struct MibInfo { bool ok = false; Mib mib; int ports = 0; int sfn = -1; /* SFN of frame 0 of the capture */ };
struct CellResult {
    Found found;
    MibInfo mib;
    bool sib1Ok = false;
    Sib1 sib1;
    Meas meas;
    int sib1Tries = 0;
    int frames = 0;              // whole frames searched
};
struct SearchOpts { double maxCfoHz = 15000; bool wantSib1 = true; float minNpss = 0; };
// Search a block (rate fs, the candidate carrier at offsetHz from 0 Hz) for an NB-IoT cell and decode what it broadcasts.
// One carrier is 180 kHz, so there is at most one cell; the vector is empty or holds one.
std::vector<CellResult> searchDecode(const cf32* x, size_t n, double fs, double offsetHz, const SearchOpts& o);
// A quick NPSS-only look at each offset (about 80 ms): the offsets where a PSS stands out, with its strength
struct Probe { double offsetHz = 0; float score = 0; };
std::vector<Probe> probe(const cf32* x, size_t n, double fs, const std::vector<double>& offsetsHz, double maxCfoHz = 15000, float minScore = 0);

// ---------------------------------------------------------------- test signal (nbiot_gen.cpp)
struct GenCfg {
    int pci = 0;
    int ports = 1;               // NRS antenna ports 1, 2
    Mib mib;                     // sfnMsb is set by the generator for the frame being sent
    Sib1 sib1;
    int crsPorts = 1;            // in-band: the LTE cell's own CRS ports 1, 2, 4
    int crsShift = -1;           // in-band: v_shift of those CRS (-1: pci mod 6)
    int sfn0 = 0;                // n_f of the first frame
    float port1Gain = 0.8f, port1Phase = 0.6f;   // the channel of antenna port 1 against port 0 (2 ports)
    double amp = 0.3;
    bool loadOther = true;       // fill the rest of the downlink subframes with random QPSK (other users' data) and NRS
    uint32_t seed = 1;
};
class Generator {
public:
    explicit Generator(const GenCfg& c);
    ~Generator();
    void nextFrame(cf32* out);   // kFrame samples at kFs
    int frame() const;           // n_f of the frame the next call produces
    double carrierPower() const; // mean power of a fully loaded symbol
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

} // namespace dect2::nbiot
