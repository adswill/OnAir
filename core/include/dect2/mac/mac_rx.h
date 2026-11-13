// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// MAC television receiver: D2-MAC/packet and D-MAC/packet over FM (satellite, ETS 300 250 for D2-MAC).
//
// The input is the FM channel at the centre (16 Msps or more; at 20 Msps the outer part of the 27 MHz channel is cut off and the decoder
// works with what fits). FM discriminator, the linear de-emphasis of Part 7A, then a polyphase interpolator onto the 20.25 MHz line grid,
// locked by a timing loop on the duobinary transitions. Acquisition: the line sync words (W1 / W2 alternate line by line) averaged over
// 96 lines for every position of the line, for both burst formats (D2: 10.125 Mbit/s on even samples, D: 20.25 Mbit/s); frame sync from
// the 96-bit sequence of line 625. Per line: clamp (samples 211-225 to 0.5 V), the burst (duobinary, logic 1 = extreme level), the
// colour difference (349 samples, 3:1) and the luminance (697 samples, 3:2) into a 576-line picture (U and V on alternate lines, the other
// one interpolated vertically), line 624 references for the levels, line 625 for the SDF (aspect ratio, scrambling, channel id) and the RDF.
// Packets: energy dispersal removed, de-interleaved, Golay header, the service identification (address 0: data groups, channel and network
// name, the TV sound address, conditional access), companded 32 kHz sound played through the sound output. EuroCrypt and the other
// conditional-access systems are detected, never descrambled.
// Not decoded: B-MAC and C-MAC (C-MAC's data is PSK at RF, B-MAC's multilevel data and 525-line options are other formats), linear and
// second-level protected sound (the BI interpretation blocks are counted only), stereo, the Golay-coded SI form, teletext, the AM/VSB cable form.
//
// Written to the engine's contract: configure() once the sample rate is known, feed() from the analysis thread (never blocks),
// telemetry(), frame() and the setters from the interface thread.
#pragma once
#include "dect2/atv/atv_tel.h"
#include "dect2/engine/mode_tuning.h"
#include "dect2/common/ring.h"
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace dect2 {

struct MacTelemetry;   // mac_tel.h

class MacReceiver {
public:
    MacReceiver();
    ~MacReceiver();

    void configure(double inputRateHz);
    void setSignalOffset(double hz);
    bool ready() const;
    void reset();
    void feed(const cf32* x, size_t n);
    bool telemetry(MacTelemetry& out, uint64_t lastSeq);
    // the newest picture (768 x 576, 1024 x 576 for 16:9), or nullptr if there is none newer than lastSeq. Thread safe.
    std::shared_ptr<const AtvFrame> frame(uint64_t& lastSeq);
    void setLogCallback(std::function<void(const std::string&)> cb);

    void setVolume(float v);
    void setMuted(bool m);
    void setSilent(bool s);
    void setAudioTap(std::function<void(const float* x, size_t n)> cb);   // tests: the decoded sound, 32 kHz mono, called from feed()

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

ModeTuning macTuning();

} // namespace dect2
