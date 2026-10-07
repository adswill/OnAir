// DMR receiver: DMR (Digital Mobile Radio) two-slot TDMA, 12.5 kHz channel, 4FSK at 4800 symbols/s (ETSI TS 102 361-1).
// Written to the engine's contract: configure() once the sample rate is known, feed() from the analysis thread (never blocks),
// telemetry() and the setters from the interface thread (they must be safe while feed() runs).
//
//   input (centred on the channel) -> decimation to 48 kHz -> DC removal -> carrier offset correction -> channel filter -> FM discriminator
//   -> root raised cosine matched filter -> frame sync search (24 symbol patterns) -> time slot tracking -> burst decoding -> calls, messages
//
// Voice: the AMBE+2 vocoder is not part of this receiver. Voice bursts are found, counted and handed on as raw bits (setVoiceCallback); the sound
// controls exist so that a codec can be plugged in later and do nothing now.
#pragma once
#include "dmr_tel.h"
#include "mode_tuning.h"
#include "ring.h"
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace dect2 {

class DmrReceiver {
public:
    DmrReceiver();
    ~DmrReceiver();

    void configure(double inputRateHz);
    bool ready() const;                                  // false when the input rate is too low for this mode
    void reset();                                        // after a retune: forget the signal, drop queued output
    void feed(const cf32* x, size_t n);
    bool telemetry(DmrTelemetry& out, uint64_t lastSeq);
    void setLogCallback(std::function<void(const std::string&)> cb);   // events for the log pane; may be called from the receiver thread
    // Sound (volume, mute) as in FmReceiver; the app pushes its shared controls here. Nothing is played: there is no vocoder.
    void setVolume(float v);                             // 0 .. 1
    void setMuted(bool m);
    void setSilent(bool s);                              // decode but do not open the sound device (tests, command line)
    void setAudioTap(std::function<void(const float* left, const float* right, size_t n)> cb);   // tests: the 48 kHz sound, called from feed() (never called)
    // The voice bursts as they arrive: slot (1 or 2), position in the superframe (0 = A ... 5 = F), the 216 vocoder bits VS(215)..VS(0) packed into 27 octets.
    void setVoiceCallback(std::function<void(int slot, int pos, const uint8_t* bits27)> cb);

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

ModeTuning dmrTuning();

} // namespace dect2
