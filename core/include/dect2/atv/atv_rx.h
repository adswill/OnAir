// Analog TV receiver: PAL, SECAM and NTSC with FM sound.
// The channel is centred in the input; the receiver finds the vision carrier (and the sound carrier), locks to it, demodulates the picture
// (synchronous detection against the recovered carrier, envelope detector as the fall-back), finds line and field sync, decodes luminance
// and colour into 768 x 576 (625 lines) or 640 x 480 (525 lines) pictures, and plays the sound of the FM carrier.
// Written to the engine's contract: configure() once the sample rate is known, feed() from the analysis thread (never blocks),
// telemetry(), frame() and the setters from the interface thread (they are safe while feed() runs).
// Sound: the FM (or AM) sound carrier is the sound channel AtvSound (atv_front.h); NICAM-728 and the two-carrier system A2 next to it
// are found by themselves and replace the mono sound when valid (atv_sound2.h; setSoundSystem turns that off).
#pragma once
#include "atv_tel.h"
#include "dect2/engine/mode_tuning.h"
#include "dect2/common/ring.h"
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
    void setNoiseReduction(int mode);                    // temporal noise reduction of the picture: -1 automatic (default; by the video S/N), 0 off, 1 light, 2 medium, 3 strong
    void setStandard(int system, int colour);            // -1 automatic; system: AtvSys (atv_std.h), colour: AtvColourKind
    void setBlackSetup(int mode);                        // -1 by system (7.5 IRE in M), 0 none, 1 7.5 IRE
    void setChromaDelayNs(double ns);                    // < 0: what the standard says (170 ns)
    void setColour(bool on);
    void setSaturation(float s);                         // 1 = as sent
    void setHue(float degrees);                          // NTSC tint
    void setChannelWidth(int mhz);                       // 6, 7 or 8 (default 8): the channel the user tuned; only matters for the carrier offset of 5.5 MHz sound (B/G)
    // Without a picture signal show the untuned picture (snow: the noise of the channel on a free-running raster) instead of nothing, as a TV does;
    // for hunting weak signals. Off by default. Such pictures have AtvFrame::untuned set.
    void setShowSnow(bool on);
    void setSoundSystem(int mode);                       // 0 automatic (NICAM-728 or A2 when there, else the FM sound), 1 the FM / AM sound carrier only
    void setDualSound(int channel);                      // two-language NICAM or A2: 0 the first channel (sound A), 1 the second (sound B)
    void setDetector(int mode);                          // 0 automatic (synchronous when the carrier is locked), 1 envelope detector only
    // 0 broadcast TV (amplitude modulated vision carrier, found by a search), 1 FM video as analog FPV links send it (the channel is the centre of
    // the input; the polarity is found by trial). FM needs 16 Msps or more (a 17 MHz wide signal); the picture decoder is the same.
    // 2 NBTV / mechanical television on an AM carrier, 3 the same on narrow FM: the carrier at the centre of the input (within +-50 kHz), its
    // audio is the video (atv_nbtv.h); the line count and rate come from setNbtv.
    void setModulation(int mode);
    // NBTV and mechanical television: lines (24 .. 240; 32 at 12.5 pictures a second is the NBTVA standard), pictures a second, the FM
    // deviation (kHz) and the scan direction (vertical: lines run top to bottom)
    void setNbtv(int lines, double frameHz, double devKhz = 5, bool vertical = true);

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

ModeTuning atvTuning();

} // namespace dect2
