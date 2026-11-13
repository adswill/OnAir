// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// KAPP burst receiver (streaming). Input: complex baseband at any rate (resampled to 640 ksps inside). Output: decoded bursts.
//
// Chain: matched RRC filter (8 samples per symbol) -> preamble search (each Zadoff-Chu half correlated on its own at every sample, so the carrier
// offset does not move the peak; a metric above the threshold = candidate) -> fractional timing by interpolation -> coarse carrier offset from the
// phase between the two halves -> header (4 repeats combined, Viterbi, CRC-16; a few trial offsets around the coarse estimate) -> fine offset, phase,
// gain and SNR from the 942 known symbols -> payload: pilot based residual frequency, phase and gain tracking -> per block soft demapping, LDPC, CRC-16.
// Not done: sample clock offset tracking (Config::clockPpm is a fixed correction for the symbol spacing).
#pragma once
#include "dect2/kapp/kapp_frame.h"
#include <functional>
#include <memory>

namespace dect2::kapp {

struct RxBurst {
    std::string callsign;
    int profile = 0;
    int seq = 0;
    int flags = 0;
    int nBlocks = 0;
    std::vector<uint8_t> blockOk;     // 1 per block: CRC-16 passed
    std::vector<uint8_t> payload;     // nBlocks x payloadBytes; blocks that failed are zero
    float snrDb = 0.f;                // Es/N0 from the preamble and header
    float cfoHz = 0.f;                // carrier offset
    uint64_t startSample = 0;         // absolute index (internal rate) of the first preamble symbol
    std::vector<cf> symbols;          // up to 400 of the equalised data symbols (evenly spread over the burst), for a constellation display
    bool isAck() const { return (flags & kFlagAck) != 0; }
    int blocksOk() const {
        int n = 0;
        for (uint8_t v : blockOk) n += v ? 1 : 0;
        return n;
    }
};

struct RxStats {
    uint64_t samples = 0;
    uint64_t detections = 0;        // preamble candidates
    uint64_t headerAttempts = 0;    // Viterbi + CRC tries (several per candidate)
    uint64_t headerOk = 0;          // candidates whose header passed (CRC and plausibility)
    uint64_t headerFail = 0;
    uint64_t bursts = 0;            // bursts emitted
    uint64_t blocksOk = 0;
    uint64_t blocksBad = 0;
};

class BurstReceiver {
public:
    struct Config {
        double sampleRate = kInternalRate;   // rate of the samples given to feed()
        float detectThreshold = 0.16f;       // sum of the two normalised correlations (noise only: mean 0.016, signal at -10 dB: ~0.18)
        double clockPpm = 0.0;               // symbol clock correction
    };
    BurstReceiver();
    explicit BurstReceiver(const Config& cfg);
    ~BurstReceiver();

    void feed(const cf* x, size_t n);
    void feed(const std::vector<cf>& x) { feed(x.data(), x.size()); }
    void setCallback(std::function<void(const RxBurst&)> cb) { cb_ = std::move(cb); }
    std::vector<RxBurst> takeBursts();    // everything decoded since the last call
    void reset();
    const RxStats& stats() const { return st_; }

private:
    void process();
    void extendFilter();
    bool scanForPeak();
    float metricAt(size_t n) const;
    // Carrier offsets beyond the +-315 Hz that one Zadoff-Chu half tolerates (up to +-5 kHz): the preamble is also correlated in blocks of 8 symbols whose
    // powers are added (the blocks are short enough not to lose the signal to the rotation), then the offset is read from the phase between neighbouring blocks.
    bool blockSums(size_t n, std::complex<double>* b) const;
    double blockMetricHalf(size_t n) const;   // blocks of the first half, noise-only mean 1
    double blockMetricFull(size_t n) const;   // all blocks, noise-only mean 1
    double blockOmega(size_t n) const;        // carrier offset in rad per symbol from the block phases
    double cohMetric(size_t n, double wh) const;   // coherent correlation of the whole preamble with the offset removed
    void refinePeak(size_t coarse);           // sets peak_, frac_ and wh_
    float corrHalf(size_t n, int half) const;
    void decodeAtPeak();
    cf symAt(double pos) const;
    void trim();

    Config cfg_;
    std::unique_ptr<Resampler> rs_;
    std::vector<cf> raw_;         // input at the internal rate
    std::vector<cf> mf_;          // matched-filter output, aligned with raw_ (valid for the first raw_.size() - c samples)
    std::vector<double> cs_;      // running sums of |mf|^2 with stride 8
    uint64_t absBase_ = 0;
    size_t scan_ = 0;
    bool havePeak_ = false;
    size_t peak_ = 0;
    double frac_ = 0.0;
    size_t needMf_ = 0;
    double wh_ = 0.0;             // offset of the burst at the peak (rad per symbol), removed before the rest of the decoder
    std::function<void(const RxBurst&)> cb_;
    std::vector<RxBurst> queue_;
    RxStats st_;
};

}  // namespace dect2::kapp
