// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// ROS (José Alberto Nieto Ros, EA5HVK): a proprietary weak-signal HF chat mode. Its framing, coding and FEC are not publicly specified, so
// OnAir cannot decode it. This is an identifier only: from the publicly reported physical layer (docs/ROS.md lists the sources) it measures a
// stretch of samples and says whether it looks like ROS HF and which sub-mode by its symbol rate:
//   - one tone at a time (MFSK, phase-continuous single tone), on a fine tone grid (reported 144 tones, ~15.5 Hz apart; 144 x 15.625 = 2250 Hz)
//   - about 2.2 to 2.3 kHz wide in USB
//   - ROS 4, 8 and 16 baud (the current ROS-2K sub-modes, sigidwiki "ROS"); an older 1 baud mode is reported but not modelled here
// Nothing is decoded and no text is read. The test-signal generator draws random tones on that grid; it is NOT ROS encoding (which is unknown).
#pragma once
#include "dect2/common/ring.h"
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace dect2 {
namespace ros {

// The publicly reported sub-modes (docs/ROS.md): what is known about each one, and whether the identifier can tell it
struct SubModeInfo {
    const char* id;        // "ros16", ...
    const char* name;      // "ROS 16 baud"
    double baud;           // reported symbol rate (0 not reported)
    double bwHz;           // reported bandwidth (0 not reported)
    bool identified;       // the identifier tells this one (by its rate on the HF tone grid)
    const char* source;    // where the numbers come from
};
const SubModeInfo* subModes(size_t& count);

struct Detection {
    bool valid = false;        // enough signal to measure
    bool ros = false;          // the measurement fits the reported ROS HF physical layer
    bool weak = false;         // it fits, but on few symbols (a short capture of a slow sub-mode): say "perhaps"
    double score = 0;          // 0 .. 1
    std::string subId;         // "ros16" / "ros8" / "ros4", "" when the rate is not one of the reported ones
    std::string name;          // "ROS 16 baud", "ROS-like (rate 11.2 Bd not a reported one)", ...
    double baud = 0;           // measured symbol rate
    double spacingHz = 0;      // measured tone grid spacing
    double gridR = 0;          // how well the tones sit on that grid (0 .. 1)
    double spanHz = 0;         // lowest to highest tone seen
    double loHz = 0, hiHz = 0; // the tones seen, from the samples' centre (complex) or in audio Hz (detectAudio)
    double purity = 0;         // median share of a clean frame's power in its strongest tone (one tone at a time: high)
    double cleanShare = 0;     // share of the frames that held one clear tone
    int segments = 0;          // tone segments (about one per symbol)
    int distinctTones = 0;     // grid positions used
    std::string why;           // one line: what fitted and what did not
};

struct DetectOptions {
    double centreHz = 0;       // the middle of the signal, from the samples' centre
    double halfWidthHz = 1500; // how much around it is looked at
};

// complex samples at fsHz (any rate from 6 kHz up; wide captures are filtered and decimated to about 8 kHz first)
Detection detect(const cf32* x, size_t n, double fsHz, const DetectOptions& o = DetectOptions());
// real USB audio at fsHz: the passband 200 .. 2900 Hz is searched (centre 1550 Hz)
Detection detectAudio(const float* x, size_t n, double fsHz);

// A test signal with the publicly reported physical parameters only: random tones out of `tones` on a `spacingHz` grid centred on centreHz, at
// `baud`, phase-continuous, unit amplitude, complex at fsHz. Not ROS encoding: there is no ROS framing, coding or FEC in it.
std::vector<cf32> synthReportedGrid(double baud, double seconds, double fsHz, double centreHz, uint32_t seed, int tones = 144, double spacingHz = 15.625);

}  // namespace ros
}  // namespace dect2
