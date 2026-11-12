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

// 4.0: the time stations that OnAir lists as modes of their own (docs/VIEWS-4.0.md): the Time signals receiver with the station fixed (and its own frequency as the default).
// family N, active standard N + 1, engine standard N + 2, poll slot N.
struct TimeView {
    int family;
    const char* id;          // dect2cli --standard
    const char* name;        // the mode-list name
    int station;             // timesig::Station
};
const TimeView* timeViews(int& count);
const TimeView* timeViewForActive(int activeStd);
const TimeView* timeViewForStd(int stdMode);
ModeTuning timeViewTuning(int i);
#define DECT2_TIMEVIEW_ACTIVE_CASES case 608: case 611: case 614: case 617: case 629: case 632: case 635: case 638: case 643: case 646: case 649:

} // namespace dect2
