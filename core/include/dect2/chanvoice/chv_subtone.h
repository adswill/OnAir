// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Sub-audible tone reader for one channel: the CTCSS tone (50 tones, 67.0 - 254.1 Hz) or the DCS code (134.4 bit/s Golay word) under the voice.
// Input: the discriminator audio at 1 kHz (the receiver low-passes and decimates it; full scale = 1.0 is +-5 kHz). Written from the public tone
// tables in tones/tones_proto.h; the squelch of the receiver never depends on it (a signal without a tone is heard as usual).
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace dect2 {
namespace chv {

struct SubTone {
    int ctcssIdx = -1;       // index into tones::ctcssHz, -1 none
    double ctcssHz = 0;      // the nominal tone
    int dcsCode = -1;        // as written (23 for 023), -1 none
    bool dcsInverted = false;
    int dcsAlias = -1;       // the same waveform also reads as this code (an inverted code is a normal one: 023I is 047N), -1 none
    bool dcsAliasInverted = false;
    bool any() const { return ctcssIdx >= 0 || dcsCode >= 0; }
    bool operator==(const SubTone& o) const { return ctcssIdx == o.ctcssIdx && dcsCode == o.dcsCode && dcsInverted == o.dcsInverted; }
};

class SubToneReader {
public:
    SubToneReader();
    void reset();
    void feed(const float* x1k, size_t n);   // 1 kHz samples
    void feed48k(const float* x, size_t n);  // the discriminator audio at 48 kHz: a 4th order 250 Hz low-pass and every 48th sample go to feed()
    const SubTone& tone() const { return t_; }
    // true while the choice was confirmed over two readings; changes only when a new tone is confirmed or the old one was missed for 1.5 s
private:
    void hop();
    void ctcssHop(const float* w, int n);
    void dcsHop(const float* w, int n);
    std::vector<float> buf_;       // the last 1000 samples, a ring
    size_t pos_ = 0, filled_ = 0, sinceHop_ = 0;
    SubTone t_;
    double bq_[2][2] = {{0, 0}, {0, 0}};   // the two low-pass sections' state
    int dec_ = 0;
    int ctcssPend_ = -1, ctcssRun_ = 0, ctcssMiss_ = 0;
    int dcsPendCode_ = -1; bool dcsPendInv_ = false; int dcsRun_ = 0, dcsMiss_ = 0;
};

} // namespace chv
} // namespace dect2
