// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Multi-band monitor for FT8, FT4, FT2 and WSPR: the user picks (band, mode) pairs (160 m .. 6 m), and every pair that lies inside the
// radio's capture is decoded at once, whatever the radio: an RTL-SDR V4 (2.4 MHz) tuned to 14 MHz hears 20 m FT8, FT4, FT2 and WSPR,
// a wideband radio (Airspy HF+, SDRplay, Hermes Lite 2, HackRF, RX888) hears every selected band.
//
//   feed(capture) -> per band: HfdigWide (WideDecimator + one 12 kHz-class chain per dial frequency) -> HfdigFtx slot decoders
//                 -> the main HfdigFtx (one combined list, FtxDecode::dialHz is the true dial frequency of the decode)
//
// The slot clock is the main decoder's (copyClockFrom): FT8 15 s, FT4 7.5 s, FT2 3.75 s, WSPR 120 s, all from the same clock.
// Pure logic (no DSP) lives in the free functions: the band table, the dial frequencies, coverage and the best centre, and the CPU guard.
// Receive only.
#pragma once
#include "hfdig_ftx.h"
#include "hfdig_mbsel.h"
#include "dect2/common/ring.h"
#include <cstddef>
#include <memory>
#include <vector>

namespace dect2 {

struct MbStatus {
    double centreHz = 0, rateHz = 0;         // the capture the monitor was built for
    int bands = 0, channels = 0;             // bands with a decimator, slot decoders running
    int dropped = 0;                         // selections given up by the CPU guard
    bool tooSlow = false;
    double load = 0;                         // processing time / real time, smoothed
    std::vector<MbSel> active, notCovered, droppedSel;
};

class HfdigMultiBand {
public:
    explicit HfdigMultiBand(HfdigFtx& mainFtx);
    ~HfdigMultiBand();
    // centreHz: the radio's centre frequency (offsetHz = the dial frequency's place in the input is derived per band)
    void configure(double rateHz, double centreHz, const std::vector<MbSel>& sels, double reportDialHz = 0);   // reportDialHz: the radio's tuned frequency (spots are reported against it; 0: the centre)
    void feed(const cf32* x, size_t n);
    MbStatus status() const;
    void setLoadClock(double (*clock)());    // tests: replaces the wall clock used to measure the load
    void testForceLoad(double busySec, double blockSec);   // tests: one guard step with an invented load
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

} // namespace dect2
