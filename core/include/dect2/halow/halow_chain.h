// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Wi-Fi HaLow (802.11ah) receive chain at one internal sample rate: front end (brought to the rate the widths of the chain need), STF detector, and
// for every candidate the long training symbols (timing, residual carrier offset, channel), the SIG field of each preamble type and the data field
// (BCC, MCS 0 - 10, one spatial stream). The monitor runs one chain for the 1 / 2 / 4 MHz channels (4 Msps grid), one for 8 MHz (8 Msps) and one for
// 16 MHz (16 Msps) when the radio's sample rate allows, so a narrow signal is never looked at through a wide noise band.
// Spatial streams > 1, STBC, LDPC, A-MPDU and NDP frames are detected from their SIG field and reported without data (HalowEvent::psduValid false).
#pragma once
#include "dect2/common/ring.h"
#include "dect2/halow/halow_phy.h"
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace dect2 {

// One PPDU the chain read: its SIG field, and the PSDU when the data field was decoded
struct HalowEvent {
    halow::Sig sig;
    bool psduValid = false;              // the data field was decoded: psdu holds the bytes with their FCS (not yet checked)
    std::vector<uint8_t> psdu;
    const char* why = "";                // psduValid false: why the data field was not decoded ("2 spatial streams", "LDPC", "STBC", "A-MPDU", "NDP")
    double cfoHz = 0;
    float rssi = -200, snrDb = 0;
};

class HalowChain {
public:
    struct Stats { uint64_t preambles = 0, sigOk = 0, sigBad = 0, dataBad = 0; uint64_t perWidth[halow::kNumWidths] = {}; };
    // inRate: the sample rate of the mixed input; chainRate: 4, 8 or 16 Msps; widths: the channel widths this chain tries
    HalowChain(double inRate, double chainRate, const std::vector<int>& widths);
    ~HalowChain();
    void setWidthMask(int mask);         // bit w: try width w (default all of the chain's)
    void feed(const cf32* mixed, size_t n);   // mixed to 0 Hz: the signal is at the centre
    void process();                      // work on what was fed: reads waiting candidates, drops old samples
    int64_t samples() const;
    const Stats& stats() const;
    std::function<void(const HalowEvent&)> onEvent;

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

} // namespace dect2
