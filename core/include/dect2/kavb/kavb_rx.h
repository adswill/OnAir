// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// KAVB receiver (spec: KAVB-SPEC-v0.1; frame layout in kavb_phy.h).
//
// Input: IQ at 4 samples per symbol of the profile's symbol rate (the caller resamples), any carrier offset up to about +-0.025 symbol rates (+-17 kHz at 700 ksym/s, +-35 kHz at 1.4 Msym/s; measured, the 16-symbol half-preamble correlation loses its peak beyond that),
// any timing. Per frame:
//   1. root-raised-cosine matched filter; preamble correlation in two halves (non-coherent peak, fractional timing by a parabola, coarse carrier offset from the phase
//      between the halves);
//   2. header: squared-BPSK frequency refinement, soft Viterbi, CRC-16 -> profile, robust flag, sweep index, sequence number;
//   3. pilots (one per 15 data symbols) + preamble: linear phase fit (carrier offset, phase) and amplitude -> one-tap equaliser, noise variance for the soft demapper;
//   4. V-sweep removal: the chirp (known from the sweep index) is estimated per frame on the carrier-corrected samples (timing shift searched on a 0.1-sample grid) and subtracted before a second matched
//      filter pass;
//   5. max-log soft demapper, deinterleave, layered min-sum LDPC, CRC-16 per block;
//   6. if a block failed: residual sweep cancellation in the symbol domain against the re-encoded decoder output, then a decision-directed 7-tap symbol-spaced
//      equaliser trained (least squares) on the same output, up to 2 passes. Each step is kept only if more blocks pass.
#pragma once
#include "dect2/kavb/kavb_phy.h"
#include <vector>

namespace dect2::kavb {

struct RxBlock {
    bool ok = false;
    int iters = -1;
    std::vector<uint8_t> bytes;     // blockPayloadBytes of the profile (valid when ok)
};

struct RxFrame {
    FrameHeader hdr;
    Profile prof{};
    std::vector<RxBlock> blocks;    // 4
    bool allOk = false;
    double esn0Db = 0.0;            // pilot-based estimate of the data Es/N0 (the chirp counts as noise before it is removed; after removal it does not)
    double cfoHz = 0.0;             // carrier offset
    double startSample = 0.0;       // stream index (fractional) of the first preamble symbol
    int eqPasses = 0;               // decision-directed equaliser passes used (0 = the one-tap equaliser was enough)
    bool chirpRemoved = false;
    std::vector<cf> dataSyms;       // up to 400 equalised data symbols (for a constellation display)
    bool chirpCancelled = false;    // the symbol-domain residual sweep cancellation made a failed block decode
};

class Receiver {
public:
    struct Options {
        double expectedSymRate = 0.0;   // > 0: frames of another symbol rate are ignored (header profile check)
        bool removeChirp = true;
        bool equalizer = true;          // decision-directed FIR pass when a block failed
        double detectThreshold = 0.40;  // normalised preamble metric
    };
    Receiver() : Receiver(Options()) {}
    explicit Receiver(const Options& o);
    // Appends samples and returns the frames completed by them. A frame whose header decodes but whose blocks fail is returned too (blocks[i].ok = false).
    std::vector<RxFrame> push(const cf* iq, size_t n);
    void reset();
    long long samplesConsumed() const { return absBase_; }
    int headerRejects() const { return headerRejects_; }
private:
    enum class St { NeedMore, Reject, Ok };
    St tryFrame(double t0, double f0, RxFrame& rf, double& endSample);
    double metricAt(long n, cf* c1 = nullptr, cf* c2 = nullptr) const;
    void computeMf();

    Options o_;
    std::vector<cf> buf_, mf_;
    std::vector<float> taps_;
    long mfDone_ = 0, scanPos_ = 0;
    long long absBase_ = 0;
    int headerRejects_ = 0;
};

}  // namespace dect2::kavb
