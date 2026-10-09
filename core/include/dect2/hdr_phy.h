// HD Radio (NRSC-5) Layer 1: the OFDM receivers of the FM hybrid (MP1 primary main sidebands: P1 and PIDS) and the AM hybrid (MA1: P1,
// P3 and PIDS) waveforms, and the matching modulators of the test signal.
//
// The receivers follow nrsc5 (GPL-3.0, github.com/theori-io/nrsc5: src/acquire.c, src/sync.c, src/decode.c): the cyclic prefix
// correlation for timing and the fractional carrier offset, Costas loops on the reference subcarriers for the integer offset, block sync,
// channel estimate and sample clock tracking (FM), the analog carrier and the training symbols (AM). The soft decision demapping and the
// Viterbi decoder are OnAir's own. Both take the band at 744187.5 Hz with the station in the middle (the AM receiver decimates by 16).
// FM: the RF spectrum is the mirror image of the baseband one (NRSC-5 1011s section 14.2.2), so the FM receiver works on the conjugate
// and the FM modulator outputs the conjugate.
#pragma once
#include "ring.h"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace dect2 { namespace hdr {

// What the L1 receivers hand up (descrambled bits, one per byte, in L1 order)
class L1Sink {
public:
    virtual ~L1Sink() = default;
    virtual void pids(const uint8_t* bits80) = 0;
    virtual bool transfer(const uint8_t* bits, int lenBits, int channel) = 0;   // channel 0 = P1, 1 = P3; false when the frame header is bad
    virtual void blockSync() = 0;                                              // block sync found (again): the transfer frames restart
};

struct L1Stats {
    int state = 0;           // 0 nothing, 1 coarse timing and frequency, 2 block sync
    double cfoHz = 0;        // carrier offset of the band handed to the receiver
    float merLower = 0, merUpper = 0;
    double ber = 0;          // channel bit errors before the Viterbi decoder (P1)
    int mode = -1;           // FM: PSMI, AM: service mode indicator
    int bc = -1;             // block count of the last block
    uint64_t syncs = 0;      // block syncs found
    uint64_t p1Frames = 0, p1Bad = 0;   // P1 transfer frames decoded, of them without a valid header
    std::vector<cf32> constel;          // equalised data subcarriers of the last block (FM: QPSK at +-1; AM: the primary 64-QAM at +-0.5 .. 3.5)
};

class FmRx {
public:
    explicit FmRx(L1Sink* sink);
    ~FmRx();
    void reset();
    void feed(const cf32* x, size_t n);      // 744187.5 Hz, the RF spectrum
    int state() const;                       // as L1Stats::state, without copying the rest
    L1Stats stats() const;
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

class AmRx {
public:
    explicit AmRx(L1Sink* sink);
    ~AmRx();
    void reset();
    void feed(const cf32* x, size_t n);      // 744187.5 Hz; decimated by 16 inside
    int state() const;
    L1Stats stats() const;
    bool clockPpm(double& ppm);               // a new measurement of the sample clock error left in the input (positive: too many samples)
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

// ---- modulators (test signal). Output at 744187.5 Hz, the RF spectrum, digital part only.
class FmTx {
public:
    FmTx();
    // One L1 block: pm holds 32 rows of the 720 bits of the 20 primary main partitions (row-major); bc 0..15; psmi 1 (MP1), 2 (MP2) or 3 (MP3).
    // px: MP2 and MP3, 32 rows of the bits of the extended partitions (72 or 144 per row, lower ones first, as the receiver reads them).
    // Every subcarrier gets the amplitude `amp` per I and Q component.
    void block(const uint8_t* pm, const uint8_t* px, int bc, int psmi, float amp, std::vector<cf32>& out);
private:
    std::vector<cf32> X_;
    std::vector<float> shape_;
};

struct AmLevels { float pri = 0, sec = 0, ter = 0, pids = 0, ref = 0; };   // amplitude scale of each sideband (multiplies the constellation)
class AmTx {
public:
    AmTx();
    // One AM L1 block of 32 symbols: the words of the PL and PU (6 bits), S (4 bits) and T (2 bits) matrices of this block (32 rows x 25
    // columns, row-major), the PIDS words (32 rows x 2), the block count 0..7. Appends 32 symbols of 4320 samples at 744187.5 Hz.
    void block(const uint8_t* pl, const uint8_t* pu, const uint8_t* s, const uint8_t* t, const uint8_t* pids, int bc, const AmLevels& lv, std::vector<cf32>& out);
private:
    std::vector<cf32> X_;
    std::vector<float> shape_;
};
cf32 amQam64(int w);     // 1012s table 12-1 (units: the points are at +-0.5 .. +-3.5)
cf32 amQam16(int w);     // table 12-5
cf32 amQpsk(int w);      // table 12-4

}} // namespace dect2::hdr
