// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// HF digital receiver: one upper sideband channel on the HF bands, demodulated once to audio, and three decoders that all listen to the
// same audio at the same time: RTTY (hfdig_rtty.h), SSTV (hfdig_sstv.h), FreeDV digital voice (hfdig_freedv.h) and FT8 / FT4 / FT2 /
// WSPR (hfdig_ftx.h).
// The channel is mixed to 0 Hz (setSignalOffset) and brought to 24 kHz complex by the marine receiver's front end (marine_dsp.h); the
// upper sideband from 200 to 3800 Hz above the dial frequency becomes real audio at 8000 Hz with a slow AGC (about -1 .. 1), as on a
// communications receiver. Every decoder's feedAudio() gets that audio. FreeDV's speech goes back out through the receiver, with the
// volume and mute the app pushes as for the other modes that make sound.
// Written to the engine's contract: configure() once the sample rate is known, feed() from the analysis thread (never blocks),
// telemetry() and the setters from the interface thread. The decoders live on the receiver thread only: their feedAudio(), reset()
// and telemetry() are never called at the same time, so they need no locks of their own.
#pragma once
#include "dect2/audio/hf_demod.h"
#include "dect2/engine/mode_tuning.h"
#include "dect2/common/ring.h"
#include "dect2/hfdig/hfdig_mbsel.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace dect2 {

struct HfdigTelemetry;   // hfdig_tel.h
class HfdigRtty;         // hfdig_rtty.h
class HfdigFtx;          // hfdig_ftx.h
class HfdigHamdrm;       // hfdig_hamdrm.h
class HfdigKbd;          // hfdig_kbd.h

constexpr double kHfdigAudioRate = 8000.0;   // the audio every decoder gets, and the rate of FreeDV's speech

// One decoder of the demodulated audio. Each is declared in its own header with its telemetry (hfdig_rtty.h, hfdig_sstv.h,
// hfdig_freedv.h) and made by its factory there (makeRttyDecoder(), makeSstvDecoder(), makeFreedvDecoder()).
class HfdigDecoder {
public:
    virtual ~HfdigDecoder() = default;
    virtual void reset() = 0;                                  // after a retune: forget the signal (what was decoded stays)
    virtual void feedAudio(const float* x, size_t n) = 0;      // 8000 Hz, about -1 .. 1
};

class HfdigReceiver {
public:
    HfdigReceiver();
    ~HfdigReceiver();

    void configure(double inputRateHz);
    void setSignalOffset(double hz);                     // where the user's (dial) frequency sits relative to 0 Hz in the input
    bool ready() const;                                  // false when the input rate is too low for this mode
    void reset();                                        // after a retune: forget the signal (the decoded results stay: clearResults)
    void feed(const cf32* x, size_t n);
    bool telemetry(HfdigTelemetry& out, uint64_t lastSeq);
    // The RTTY text, SSTV pictures, FT8 / FT4 / FT2 / WSPR decodes and HamDRM files stay through reset(), configure() and retunes:
    // only clearResults() empties them (mask as setDecoders(): 1 RTTY, 2 SSTV, 8 FT8 / FT4 / FT2 / WSPR, 16 HamDRM)
    void clearResults(int mask = 31);
    std::string saveResults() const;                     // for the results file (results_io.h)
    bool loadResults(const std::string& blob);           // false: not this build's format, nothing changes
    void setLogCallback(std::function<void(const std::string&)> cb);   // events for the log pane; may be called from the receiver thread

    // Sound (volume, mute) as in FmReceiver; the app pushes its shared controls here. FreeDV plays its speech; the other decoders are
    // silent unless setListen(true), which plays the sideband audio they decode (not while FreeDV runs alone).
    void setVolume(float v);                             // 0 .. 1
    void setMuted(bool m);
    void setSilent(bool s);                              // decode but do not open the sound device (tests, command line)
    void setListen(bool on);                             // "Listen": the 8 kHz sideband audio on the sound card (default off)
    bool listening() const;
    void setListenTap(std::function<void(const float* x, size_t n)> cb);   // tests: the 8 kHz audio Listen plays (also when silent)
    void setAudioTap(std::function<void(const float* x, size_t n)> cb);    // tests: the demodulated 8 kHz audio, called from feed()
    void setSpeechTap(std::function<void(const float* x, size_t n)> cb);   // tests: FreeDV's 8 kHz speech, called from feed()
    HfdigFtx& ftx();                                     // FT8 / FT4 / FT2 / WSPR: settings and the slot clock (thread safe, hfdig_ftx.h)
    HfdigRtty& rtty();
    HfdigKbd& kbd();                                     // Olivia, Contestia, MFSK, THOR, DominoEX, Hell (thread safe, hfdig_kbd.h)
    HfdigHamdrm& hamdrm();                               // HamDRM / EasyPal pictures (receiver thread only, or while stopped)
    void setDecoders(int mask);                          // which decoders run: 1 RTTY, 2 SSTV, 4 FreeDV, 8 FT8 / FT4 / FT2 / WSPR, 16 HamDRM, 32 the keyboard modes (default 31: all but the keyboard modes)
    int decoders() const;
    void setDemod(int d);                                // the front end's demodulator (HfDemod, hf_demod.h): USB (default), LSB, NFM, AM; Auto counts as USB
    // Whole span (the "wide decode" switch, hfdig_wide.h): FT8 / FT4 / WSPR on every standard dial slot in the span, RTTY on every pair of
    // tones in up to 100 kHz of it; the main decoders still work on the dial frequency. dialHz: the radio's tuned frequency (the slots are
    // found from it; 0: only RTTY). Any thread; taken up by the next feed().
    void setWide(bool on, double spanHz = 250000);
    bool wide() const;
    void setDialHz(double hz);
    // Multi-band monitor (hfdig_multiband.h): FT8 / FT4 / FT2 / WSPR on the selected (band, mode) pairs that lie inside the capture, all at
    // once (the radio's centre is dialHz - the signal offset). While it is on the main FT8 decoder on the dial frequency is silent: the
    // monitor covers that dial too when selected. Any thread; taken up by the next feed(). Receive only.
    void setMultiBand(bool on, const std::vector<MbSel>& sels = {});
    bool multiBand() const;
    int demod() const;                                   // the RTTY decoder's settings (thread safe, see hfdig_rtty.h)

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

ModeTuning hfdigTuning();

} // namespace dect2
