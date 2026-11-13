// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// AM stereo receiver (C-QUAM, Motorola; the system of most AM stereo stations: Canada, the USA, Mexico, Brazil, Japan, Australia).
//
// The channel (dial + channel offset) is mixed to 0 Hz and brought to exactly 48 kHz like the AM broadcast entry of the general receiver
// (qo100::Decimator, exact resampler, a complex FIR whose cut-off is the audio bandwidth). C-QUAM sends z = (1 + L + R) + j (L - R) on the
// carrier, with the envelope stretched by 1 / cos(phi) so that a mono envelope detector still hears L + R. A phase-locked loop on the
// carrier (frequency first from the mean phase step, then a loop of 30 Hz that narrows to 4 Hz) brings the carrier to the real axis: the
// real part divided by the carrier amplitude is 1 + L + R, the imaginary part is L - R. The loop is slower than the 25 Hz pilot, which is a
// 5 % phase modulation in the quadrature part: a Goertzel analysis of the quadrature channel at 25 Hz against the bins beside it (every
// half second, a Hann window of one second) finds it; two hits switch to stereo, three misses back to mono (the quadrature gain fades over
// 40 ms). Without a pilot the output is mono (L = R). The audio has a 50 Hz high-pass (pilot and DC out), a squelch on the carrier-to-noise
// ratio and the shared volume and mute; the older Kahn-Hazeltine, Harris and Magnavox systems are not decoded.
// Engine standard 109 (active 108). Written to the engine's contract: configure() once the sample rate is known, feed() from the analysis
// thread (never blocks), telemetry() and the setters from any other thread.
#pragma once
#include "dect2/engine/mode_tuning.h"
#include "dect2/common/ring.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace dect2 {

struct CquamTelemetry;   // cquam_tel.h

constexpr double kCquamAudioRate = 48000.0;

class CquamReceiver {
public:
    CquamReceiver();
    ~CquamReceiver();

    void configure(double inputRateHz);
    void setSignalOffset(double hz);                 // where the dial frequency sits relative to 0 Hz in the input
    void setCenterHz(double) {}
    bool ready() const;
    void reset();                                    // a retune: the loop, the pilot detector and the noise estimate start again
    void feed(const cf32* x, size_t n);
    bool telemetry(CquamTelemetry& out, uint64_t lastSeq);
    void setLogCallback(std::function<void(const std::string&)> cb);

    void setChannelOffsetHz(double hz);              // the station relative to the dial (a few hundred Hz at most are pulled in by themselves)
    void setStereoMode(int m);                       // 0 automatic (the pilot decides), 1 mono, 2 stereo whatever the pilot says
    void setWidthHz(double hz);                      // audio bandwidth of the channel filter: 3000 .. 10000 Hz (default 5000)
    void setSquelch(bool on);                        // mute under a carrier-to-noise ratio of 4 dB (default on)
    void setVolume(float v);                         // 0 .. 1
    void setMuted(bool m);
    void setSilent(bool s);                          // decode without opening the sound device (tests, command line)
    void setAudioTap(std::function<void(const float* left, const float* right, size_t n)> cb);   // tests: the 48 kHz audio

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

ModeTuning cquamTuning();

// Usual medium-wave frequencies with C-QUAM stations (kHz is how a dial reads); steps are 10 kHz in the Americas, 9 kHz elsewhere
struct CquamPreset { const char* name; double mhz; };
const CquamPreset* cquamPresets(int& count);

} // namespace dect2
