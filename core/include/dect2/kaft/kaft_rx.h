// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// KAFT receiver: streaming frame receiver. Finds the preamble (the Zadoff-Chu root of each width both detects the frame and identifies the width),
// gets timing, frequency offset and phase, decodes the header, equalises with the pilots, decodes every LDPC block and checks its CRC-32.
//
// Input: complex baseband at config.inputRate (default kRefRate = 3.2 Msps, every width is a whole division of it). The receiver looks for all three
// widths at once, each through its own decimation to 4 samples per symbol; the width whose preamble correlates is the width of the frame (a header that
// disagrees with the preamble width is ignored, as the spec says).
// Carrier offset: first from blocks of 16 preamble symbols, then refined from the two repeats of the preamble (residual within +-symRate/502). A Zadoff-Chu
// sequence couples a timing error with a frequency error (one symbol of timing looks like symRate x root / 251), so the capture range is +-symRate x root / 502:
// +-20 kHz at 1 MHz (root 13), +-8.7 kHz at 500 kHz (root 11) and +-2.2 kHz at 200 kHz (root 7; 5 ppm at 440 MHz, 1.7 ppm at 1.29 GHz).
// Cost: the input is converted and matched-filtered once per width, only the new samples at each call.
// Limits:
// there is no sample clock tracking inside a frame (frames are at most 32000 symbols, so a clock error of a few ppm is harmless).
#pragma once
#include "dect2/kaft/kaft_frame.h"
#include <cstdint>
#include <vector>

namespace dect2::kaft {

struct RxBlock {
    bool ok = false;                  // LDPC decoded and CRC-32 passed
    std::vector<uint8_t> payload;     // payloadBytes(profile) bytes when ok
};

struct RxFrame {
    FrameHeader header;
    std::vector<RxBlock> blocks;      // header.nBlocks entries
    float esN0Db = 0;                 // from the pilots
    float cfoHz = 0;                  // carrier offset that was removed
    float preambleRho = 0;            // normalised preamble correlation (0 to 1)
    uint64_t startSample = 0;         // input sample index (counted from the first sample ever pushed) of the first preamble symbol
    uint64_t endSample = 0;           // input sample index just after the last symbol
    int blocksOk() const { int n = 0; for (const RxBlock& b : blocks) n += b.ok ? 1 : 0; return n; }
};

struct FrameReceiverConfig {
    int inputRate = kRefRate;         // samples per second of the pushed signal (an integer)
    int forceWidth = -1;              // 0 / 1 / 2 to look for one width only, -1 for all
    float detectThreshold = 0.18f;    // normalised lag-251 autocorrelation that makes a candidate
    float verifyThreshold = 0.10f;    // normalised preamble cross-correlation that accepts it
    ResampleFn resample;              // no longer used: the receiver converts the input incrementally with its own copy of the resample() filter
};

class FrameReceiver {
public:
    explicit FrameReceiver(const FrameReceiverConfig& cfg = FrameReceiverConfig());
    void push(const cf* x, size_t n);
    void push(const std::vector<cf>& x) { push(x.data(), x.size()); }
    // end of data: decode everything that is complete, even if the buffer is small
    void flush();
    // frames decoded since the last call
    std::vector<RxFrame> take();
    uint64_t samplesPushed() const { return base_ + in_.size(); }

private:
    struct Cand {
        int w = 0;
        double tau = 0;       // fractional index (internal 4 sps stream of the width) of the first preamble symbol centre
        double posIn = 0;     // the same in input samples, relative to in_[0]
        float cfoHz = 0;
        float rho = 0;
        bool incomplete = false;
    };
    enum class Status { Ok, Incomplete, Reject };
    void process(bool final);
    // Incremental front end: each width keeps its resampled stream (dec_) and its matched-filter output (mf_) from absolute output index decSt_; only the
    // samples that arrived since the last call are converted (the outputs whose filter support is complete; the rest on flush).
    void extendDec(int w, bool final);
    void extendMf(int w, bool final);
    void trimFront();
    Status decodeAt(int w, const std::vector<cf>& mf, const Cand& c, RxFrame& out, double& endIn) const;

    FrameReceiverConfig cfg_;
    int up_[kNumWidths] = {1, 1, 1};
    int down_[kNumWidths] = {1, 1, 1};
    std::vector<cf> in_;
    uint64_t base_ = 0;
    size_t sinceProcess_ = 0;
    size_t minProcess_ = 0;
    std::vector<RxFrame> frames_;
    std::vector<cf> dec_[kNumWidths], mf_[kNumWidths];
    int64_t decSt_[kNumWidths] = {0, 0, 0};
    std::vector<float> tap_[kNumWidths];
    int tapH_[kNumWidths] = {0, 0, 0};
};

// convenience: all frames of one recording
std::vector<RxFrame> decodeWaveform(const std::vector<cf>& x, const FrameReceiverConfig& cfg = FrameReceiverConfig());

}  // namespace dect2::kaft
