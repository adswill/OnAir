// Analog TV receiver: PAL, SECAM and NTSC with FM sound.
// The channel is centred in the input; the receiver finds the vision carrier (and the sound carrier), locks to it, demodulates the picture
// (synchronous detection against the recovered carrier, envelope detector as the fall-back), finds line and field sync, decodes luminance
// and colour into 768 x 576 (625 lines) or 640 x 480 (525 lines) pictures, and plays the sound of the FM carrier.
// Written to the engine's contract: configure() once the sample rate is known, feed() from the analysis thread (never blocks),
// telemetry(), frame() and the setters from the interface thread (they are safe while feed() runs).
// NICAM and the two-carrier systems (A2) are not decoded; the sound channel is a class of its own (AtvSound in atv_front.h) so a second
// carrier can be added next to it.
#pragma once
#include "atv_tel.h"
#include "mode_tuning.h"
#include "ring.h"
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace dect2 {

class AtvReceiver {
public:
    AtvReceiver();
    ~AtvReceiver();

    void configure(double inputRateHz);
    bool ready() const;                                  // false when the input rate is too low (below 8 Msps)
    void reset();                                        // after a retune: forget the signal, drop queued output
    void feed(const cf32* x, size_t n);
    bool telemetry(AtvTelemetry& out, uint64_t lastSeq);
    // The newest picture, or nullptr if there is none newer than lastSeq (lastSeq is set to the picture's number). Thread safe.
    std::shared_ptr<const AtvFrame> frame(uint64_t& lastSeq);
    void setLogCallback(std::function<void(const std::string&)> cb);   // events for the log pane; may be called from the receiver thread

    // Sound (volume, mute) as in FmReceiver; the app pushes its shared controls here.
    void setVolume(float v);                             // 0 .. 1
    void setMuted(bool m);
    void setSilent(bool s);                              // decode but do not open the sound device (tests, command line)
    void setAudioTap(std::function<void(const float* left, const float* right, size_t n)> cb);   // tests: the 48 kHz sound, called from feed()

    // Picture and standard (all safe to call while it runs)
    void setDeinterlace(int mode);                       // 0 weave (default), 1 bob
    void setStandard(int system, int colour);            // -1 automatic; system: AtvSys (atv_std.h), colour: AtvColourKind
    void setBlackSetup(int mode);                        // -1 by system (7.5 IRE in M), 0 none, 1 7.5 IRE
    void setChromaDelayNs(double ns);                    // < 0: what the standard says (170 ns)
    void setColour(bool on);
    void setSaturation(float s);                         // 1 = as sent
    void setHue(float degrees);                          // NTSC tint
    void setChannelWidth(int mhz);                       // 6, 7 or 8 (default 8): the channel the user tuned; only matters for the carrier offset of 5.5 MHz sound (B/G)
    void setDetector(int mode);                          // 0 automatic (synchronous when the carrier is locked), 1 envelope detector only
    // 0 broadcast TV (amplitude modulated vision carrier, found by a search), 1 FM video as analog FPV links send it (the channel is the centre of
    // the input; the polarity is found by trial). FM needs 16 Msps or more (a 17 MHz wide signal); the picture decoder is the same.
    void setModulation(int mode);

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

ModeTuning atvTuning();

} // namespace dect2
