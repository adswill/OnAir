// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// KAVL receiver (frame layout in kavl_phy.h).
//
// Input: IQ at any sample rate (resampled to 4 samples per symbol), any carrier offset up to about +-0.4 symbol rates (the preamble is found by a
// differential correlation, which does not care about the offset), clock offsets of a few hundred ppm, DC offset and IQ imbalance (RTL-SDR).
// Per frame:
//   0. (input) DC removal and blind IQ-imbalance correction (z = y - w conj(y), w adapted so that E[z^2] = 0), resampling;
//   1. root-raised-cosine matched filter; differential preamble correlation (peak -> timing, its phase -> coarse carrier offset); fine offset from the
//      preamble quarters;
//   2. header: soft Viterbi, CRC-16 -> modcod, sequence number;
//   3. pilots: early-late timing error per pilot, fitted with a line (timing offset and clock drift), 3 passes; carrier phase line fit; smoothed pilot gains
//      interpolated to every data symbol; noise variance from the pilot residuals;
//   4. max-log soft demapper, de-interleaver, layered min-sum LDPC (ATSC 3.0 16200 codes), CRC-32 per code word;
//   5. if a code word failed: decision-directed pass (decisions from the decoder output, a least-squares gain per symbol over a short window, which tracks
//      phase noise), re-demap and decode the failed code words again; up to 2 passes, kept only while more code words pass.
#pragma once
#include "kavl/kavl_phy.h"
#include <memory>
#include <vector>

namespace kavl {

struct RxBlock {
    bool ok = false;
    int iters = -1;
    std::vector<uint8_t> bytes;   // payloadBytes of the modcod (valid when ok)
};

struct RxFrame {
    Header hdr;
    const Modcod* mc = nullptr;
    std::vector<RxBlock> blocks;
    bool allOk = false;
    double esn0Db = 0.0;          // pilot-based estimate
    double merDb = 0.0;           // modulation error ratio on the final decisions
    double cfoHz = 0.0;
    double clockPpm = 0.0;        // sample clock offset of the transmitter against ours
    double startSample = 0.0;     // input-rate sample index of the first preamble symbol
    int ddPasses = 0;             // decision-directed passes used
    std::vector<cf> dataSyms;     // up to 2000 equalised data symbols (constellation display)
};

class Receiver {
public:
    struct Options {
        double symRate = kDefaultSymRate;
        double inRate = 0.0;        // input sample rate; 0 = 4 x symRate (no resampling)
        bool iqCorrect = true;      // DC removal + IQ-imbalance correction
        float detect = 0.30f;       // differential preamble metric threshold (0..1)
        int maxIter = 40;           // LDPC iterations
        bool ddPass = true;
    };
    Receiver();
    explicit Receiver(const Options& o);
    ~Receiver();
    Receiver(const Receiver&) = delete;
    Receiver& operator=(const Receiver&) = delete;
    // Appends samples and returns the frames completed by them. A frame whose header decodes but whose code words fail is returned too.
    std::vector<RxFrame> push(const cf* iq, size_t n);
    void reset();
    int headerRejects() const;
    cf iqWeight() const;            // the current IQ-imbalance correction weight w
private:
    struct Impl;
    std::unique_ptr<Impl> d_;
};

}  // namespace kavl
