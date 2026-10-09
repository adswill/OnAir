// HF digital receiver: one upper sideband channel on the HF bands, demodulated once to audio, and three decoders that all listen to the
// same audio at the same time: RTTY (hfdig_rtty.h), SSTV (hfdig_sstv.h) and FreeDV digital voice (hfdig_freedv.h).
// The channel is mixed to 0 Hz (setSignalOffset) and brought to 24 kHz complex by the marine receiver's front end (marine_dsp.h); the
// upper sideband from 200 to 3800 Hz above the dial frequency becomes real audio at 8000 Hz with a slow AGC (about -1 .. 1), as on a
// communications receiver. Every decoder's feedAudio() gets that audio. FreeDV's speech goes back out through the receiver, with the
// volume and mute the app pushes as for the other modes that make sound.
// Written to the engine's contract: configure() once the sample rate is known, feed() from the analysis thread (never blocks),
// telemetry() and the setters from the interface thread. The decoders live on the receiver thread only: their feedAudio(), reset()
// and telemetry() are never called at the same time, so they need no locks of their own.
#pragma once
#include "mode_tuning.h"
#include "ring.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace dect2 {

struct HfdigTelemetry;   // hfdig_tel.h
class HfdigRtty;         // hfdig_rtty.h

constexpr double kHfdigAudioRate = 8000.0;   // the audio every decoder gets, and the rate of FreeDV's speech

// One decoder of the demodulated audio. Each is declared in its own header with its telemetry (hfdig_rtty.h, hfdig_sstv.h,
// hfdig_freedv.h) and made by its factory there (makeRttyDecoder(), makeSstvDecoder(), makeFreedvDecoder()).
class HfdigDecoder {
public:
    virtual ~HfdigDecoder() = default;
    virtual void reset() = 0;                                  // after a retune: forget the signal and the decoded state
    virtual void feedAudio(const float* x, size_t n) = 0;      // 8000 Hz, about -1 .. 1
};

class HfdigReceiver {
public:
    HfdigReceiver();
    ~HfdigReceiver();

    void configure(double inputRateHz);
    void setSignalOffset(double hz);                     // where the user's (dial) frequency sits relative to 0 Hz in the input
    bool ready() const;                                  // false when the input rate is too low for this mode
    void reset();                                        // after a retune: forget the signal and the decoded state
    void feed(const cf32* x, size_t n);
    bool telemetry(HfdigTelemetry& out, uint64_t lastSeq);
    void setLogCallback(std::function<void(const std::string&)> cb);   // events for the log pane; may be called from the receiver thread

    // Sound (volume, mute) as in FmReceiver; the app pushes its shared controls here. Only FreeDV makes sound.
    void setVolume(float v);                             // 0 .. 1
    void setMuted(bool m);
    void setSilent(bool s);                              // decode but do not open the sound device (tests, command line)
    void setAudioTap(std::function<void(const float* x, size_t n)> cb);    // tests: the demodulated 8 kHz audio, called from feed()
    void setSpeechTap(std::function<void(const float* x, size_t n)> cb);   // tests: FreeDV's 8 kHz speech, called from feed()
    HfdigRtty& rtty();                                   // the RTTY decoder's settings (thread safe, see hfdig_rtty.h)

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

ModeTuning hfdigTuning();

} // namespace dect2
