// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Analog FPV video receiver for the engine: IQ in, picture and channel levels out.
// Analog FPV links (5.8 GHz bands A / B / E / F / R / L, 1.2 GHz) send composite video (PAL or NTSC, 625 or 525 lines) as frequency modulation of a
// carrier about 17 MHz wide, with one or two sound subcarriers (6.0 / 6.5 MHz) in the composite. The receiver measures every channel of the plans
// that lies inside the capture (power over the noise floor), takes the one on the radio's centre (within 4 MHz) to baseband and gives it to the
// analog TV picture decoder in FM video mode (atv_rx.h), and demodulates a sound subcarrier. The on-screen display of the flight controller is part
// of the picture and is not decoded. Needs a wide radio: 16 Msps and up (a HackRF at 20 Msps).
//
// Written to the engine's contract: configure() once the sample rate is known, feed() from the analysis thread (never blocks),
// telemetry(), frame() and the setters from the interface thread.
#pragma once
#include "fpv_tel.h"
#include "dect2/atv/atv_rx.h"
#include "dect2/engine/mode_tuning.h"
#include "dect2/common/ring.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace dect2 {

class FpvReceiver {
public:
    FpvReceiver();
    ~FpvReceiver();

    void configure(double inputRateHz);
    void setSignalOffset(double hz);            // not used: the channel is placed from the centre frequency
    void setCenterHz(double hz);                // the radio's centre (0: a file: the channel is the middle of the capture)
    bool ready() const;                         // false while the input rate is below 16 Msps
    void reset();                               // after a retune: forget the signal; the channel table stays
    void clearChannels();                       // forget the levels measured so far
    void feed(const cf32* x, size_t n);
    bool telemetry(FpvTelemetry& out, uint64_t lastSeq);
    std::shared_ptr<const AtvFrame> frame(uint64_t& lastSeq);   // the newest picture
    void setLogCallback(std::function<void(const std::string&)> cb);

    // Picture controls, passed on to the picture decoder
    void setDeinterlace(int mode);              // 0 weave (default), 1 bob
    void setStandard(int system, int colour);   // -1 automatic (PAL or NTSC is found)
    void setColour(bool on);
    void setSaturation(float s);

    // Sound: the subcarrier (0 automatic: the stronger of 6.0 and 6.5 MHz; otherwise the frequency in Hz) is demodulated and played only while on
    void setAudio(bool on);
    void setAudioSubcarrier(double hz);
    void setVolume(float v);
    void setMuted(bool m);
    void setSilent(bool s);                     // decode but do not open the sound device (tests, command line)
    void setAudioTap(std::function<void(const float* mono, size_t n)> cb);   // tests: the 48 kHz sound, called from feed()

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

ModeTuning fpvTuning();

} // namespace dect2
