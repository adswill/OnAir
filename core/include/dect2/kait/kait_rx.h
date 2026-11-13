// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// KAIT burst receiver (spec: KAIT-SPEC-v0.1), and the progressive picture.
//
// Chain: FFT correlation with the up and down sweep -> start of the burst, carrier offset (up/down pair) -> derotate, matched RRC filter -> header slots, pilot based
// phase / gain / noise tracking -> header (16 soft combined copies, Viterbi, CRC-16) -> end sweep: sample clock error -> all slots (cubic interpolation at the corrected
// symbol instants) -> per block soft demapping, LDPC, CRC-16 -> the stream prefix -> picture. A block that fails ends the usable stream (the image code is sequential),
// so the picture is whatever the bytes up to the first failed block give.
// Limits: one burst per call; the carrier offset is searched within +-15 kHz; no multipath equalisation (a single carrier on a line-of-sight / AWGN-like channel).
#pragma once
#include "dect2/kait/kait_frame.h"
#include <functional>

namespace dect2::kait {

struct RxConfig {
    double sampleRate = kInternalRate;   // rate of the samples given
    float detectRatio = 40.f;            // sweep correlation peak over the mean (noise) correlation power
    bool stopAtFirstFail = true;         // false: try every block (measurements)
    bool makeImage = true;               // decode the picture at the end (and at each layer when progressive)
    bool progressiveImages = false;      // call the progress callback with a picture after every layer
};

struct LayerResult {
    int blocks = 0;
    int attempted = 0;
    int blocksOk = 0;
    bool complete() const { return blocks > 0 && blocksOk == blocks; }
};

struct RxResult {
    bool synced = false;        // the start sweep was found
    bool headerOk = false;
    bool endSweep = false;      // the end sweep was found and used for the clock
    Header hdr;
    double esn0Db = 0.0;        // measured on the pilots
    double snrDb = 0.0;         // the same in the 190 kHz occupied bandwidth
    double cfoHz = 0.0;
    double clockPpm = 0.0;
    std::vector<LayerResult> layers;
    int layersComplete = 0;     // leading layers with every block good
    size_t bytesGood = 0;       // usable stream bytes (up to the first failed block)
    std::vector<uint8_t> stream;   // hdr.streamBytes() bytes, blocks that failed are zero
    std::vector<uint8_t> blockOk;  // per block in stream order
    Image image;                // from the first bytesGood bytes; empty when there are none
};

using ProgressFn = std::function<void(int layer, const RxResult& partial)>;
RxResult decodeBurst(const std::vector<cf>& in, const RxConfig& cfg = RxConfig{}, const ProgressFn& progress = nullptr);

// The strongest up sweep in x[0 .. n) at the internal rate: its first sample index (fractional), and how far above the mean correlation power it is. False when it is
// below `ratio`. Cheap enough to run on a second of samples at a time (the live receiver does).
bool findStartSweep(const cf* x, size_t n, float ratio, double& pos, double& peakRatio);

// Streaming front end: collects samples and decodes when a whole burst (plus slack) is buffered or on flush().
class StreamReceiver {
public:
    explicit StreamReceiver(const RxConfig& cfg = RxConfig{}) : cfg_(cfg) {}
    void setCallback(std::function<void(const RxResult&)> cb) { cb_ = std::move(cb); }
    void setProgress(ProgressFn p) { prog_ = std::move(p); }
    void feed(const cf* x, size_t n);
    RxResult flush();                 // decodes what is buffered now
    void reset() { buf_.clear(); }
    size_t buffered() const { return buf_.size(); }
private:
    RxConfig cfg_;
    std::vector<cf> buf_;
    std::function<void(const RxResult&)> cb_;
    ProgressFn prog_;
};

}  // namespace dect2::kait
