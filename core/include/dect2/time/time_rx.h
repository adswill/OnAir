// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Time signal receiver: DCF77, MSF, WWVB, JJY (LF), WWV / WWVH and CHU (HF). It finds the seconds, reads the minute frames (time_proto.h),
// and compares the received time, after the path delay from the receiver position, with the computer's clock.
//
// The input is mixed so the carrier sits at 0 Hz and averaged down to 8 kHz. LF: the carrier envelope after a 20 ms window; folded over
// the second it gives the keying edge, found to a fraction of a millisecond, and each second is read against the always-high and always-low
// parts of the averaged second. HF: the AM audio; the 1000 / 1200 Hz ticks give the seconds, the 100 Hz subcarrier the WWV BCD frame,
// a 300 bit/s Bell 103 FSK demodulator the CHU A frames. The DCF77 phase code and the WWVB phase code are not used.
//
// Written to the engine's contract: configure() once the sample rate is known, feed() from the analysis thread (never blocks),
// telemetry() and the setters from the interface thread.
#pragma once
#include "dect2/engine/mode_tuning.h"
#include "dect2/common/ring.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace dect2 {

struct TimeTelemetry;   // time_tel.h

class TimeReceiver {
public:
    TimeReceiver();
    ~TimeReceiver();

    void configure(double inputRateHz);
    void setSignalOffset(double hz);
    void setDialHz(double hz);                  // the user's frequency: picks the station when none is set (0: unknown, a file)
    void setStation(int s);                     // timesig::Station, -1 from the dial frequency
    // A recording: setClockScale(k): the recorder's clock ran slow by k (samples per signal second = rate * k; BPC and BPM files of a
    // 2.55 % slow recorder need 1.0255); setAudioInput(true, tone): the samples are real, already demodulated audio (HF stations: the AM
    // envelope with the carrier removed; BPC: the carrier is a tone at toneHz, in signal time)
    void setClockScale(double k);
    void setAudioInput(bool on, double toneHz = 0);
    void setPref60(int s);                      // which station 60 kHz means with the station from the dial
    // 4.0 (default on): after one decoded minute, a minute whose parity fails counts when it matches the expected minute's time code (54 of 60 symbols,
    // clearly better than the neighbouring minutes). It confirms the time; it reads nothing new from the signal.
    void setPredictedMinutes(bool on);
    // 0: no clock to compare (a file); 1: the computer clock as the samples arrive (a radio, the test signal);
    // 2: sample 0 was taken at UTC epoch originSec (tests)
    void setClockMode(int mode, double originSec = 0);
    void setReceiverLocation(double lat, double lon, bool userSet);
    bool ready() const;
    int station() const;                        // the station being received
    void reset();                               // the demodulator; the frame log stays
    void clearLog();
    void feed(const cf32* x, size_t n);
    bool telemetry(TimeTelemetry& out, uint64_t lastSeq);
    // The decoded minutes stay through reset(), configure() and retunes: only clearResults() empties them
    void clearResults();
    std::string saveResults() const;                     // for the results file (results_io.h)
    bool loadResults(const std::string& blob);           // false: not this build's format, nothing changes
    void setLogCallback(std::function<void(const std::string&)> cb);
    // "Listen": HF stations (WWV, CHU...) play their AM audio (ticks, voice); LF stations (DCF77...) a 800 Hz tone keyed by the carrier. Off by default
    void setListen(bool on);
    bool listening() const;
    void setVolume(float v);
    void setMuted(bool m);
    void setSilent(bool s);                              // decode but do not open the sound device
    void setListenTap(std::function<void(const float* x, size_t n)> cb);   // tests: the 8 kHz audio (also when silent)

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

ModeTuning timeTuning();

} // namespace dect2
