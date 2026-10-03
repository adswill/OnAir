// Offline DVB-T2-like signal generator used for testing and the built-in synthetic source.
// Produces genuine P1 symbols (S1/S2 signalling) followed by P2/data symbols whose cells are random QPSK.
// Never transmits anything - it only writes samples to memory/files.
#pragma once
#include "ring.h"
#include "t2.h"
#include "t2pilots.h"
#include "t2l1.h"
#include <memory>
#include <random>
#include <vector>
#include <cstdint>

namespace dect2 {

struct TxParams {
    int s1 = 0;          // 0 = T2-Base SISO
    int s2field1 = 1;    // FFT size code, see FftMode
    bool mixed = false;  // S2 field 2
    int giIdx = 4;       // see guardName()
    bool ext = false;    // extended carrier mode
    int pp = 0;          // pilot pattern 0..7 = PP1..PP8
    bool tr = false;     // tone-reservation PAPR (reserved carriers left empty)
    int dataSymbols = 0; // symbols per frame after P1 (incl. P2); 0 = auto (~60 ms)
    int l1Mod = 1;       // L1-post modulation: 0 BPSK 1 QPSK 2 16QAM 3 64QAM
    bool l1Scrambled = true;
    int cellId = 0x1001, networkId = 0x3085, systemId = 0x8001;
    uint32_t seed = 1;
};

class T2Generator {
public:
    explicit T2Generator(const TxParams& p);
    ~T2Generator();
    const TxParams& params() const { return p_; }
    int fftN() const { return n_; }
    int guard() const { return g_; }
    int carriers() const { return k_; }
    const PilotMap& pilots() const { return *pm_; }
    int dataSymbols() const { return symbols_ - nP2_; }
    const L1Pre& l1pre() const { return pre_; }
    const L1Post& l1post() const { return post_; }
    int symbolsPerFrame() const { return symbols_; }
    size_t frameLength() const { return (size_t)kP1Len + (size_t)symbols_ * (n_ + g_); }
    void nextFrame(std::vector<cf32>& out); // one full frame: P1 + symbols

    struct Impl;

private:
    Impl* i_;
    TxParams p_;
    std::unique_ptr<PilotMap> pm_;
    L1Pre pre_;
    L1Post post_;
    int frameNo_ = 0;
    int n_, g_, k_, symbols_, nP2_;
};

} // namespace dect2
