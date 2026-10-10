// Airband receiver: AM voice in the VHF aeronautical band (118 - 137 MHz), several channels of the sample band at once.
//
// Every channel of the user's list that lies inside the sample band is mixed to 0 Hz, brought down by a CIC decimator to about 50 kHz,
// corrected by the radio's tuning error, filtered (8.33 kHz channels: pass +-3.4 kHz, stop 4.9 kHz; 25 kHz channels: pass +-5 kHz, stop
// 8 kHz) and brought to the channel rate (16 to 24 kHz). There the carrier is searched (short FFTs) and followed by a phase locked loop:
// synchronous AM where the loop holds, the envelope where it does not (two stations at once, the first milliseconds). The carrier is taken
// out, the audio is scaled by the carrier (AGC), band-passed 300 - 3000 Hz and resampled to 8 kHz.
//
// Squelch: the carrier to noise ratio, measured as the carrier's power against the noise floor of the channel (tracked while the channel is
// quiet), must pass the threshold in two blocks in a row; it closes when the carrier has gone for the hang time. The audio runs 60 ms
// late, so the squelch decision for a sample is made before the sample plays: the first syllable is not cut and no noise burst is heard.
//
// Tuning error: a radio off by 50 ppm is 7 kHz off at 137 MHz, more than the 8.33 kHz raster's half step. The receiver watches the carriers
// of all the listed channels and takes the offset that puts most of them on their channels. One channel alone may only move it by up to
// 4.1 kHz (half a step), so a strong neighbour 8.33 kHz away is never taken for the channel; a larger error needs two channels that agree.
// Set the radio's ppm correction when only one 8.33 channel is listed.
//
// Audio: the open channels are mixed (a priority channel that is open pushes the others 12 dB down); solo plays only the solo channels;
// scan plays one channel at a time, stops on the first that opens (the priority channel first, and it takes over when it opens) and goes on
// two seconds after that channel closed.
//
// Written to the engine's contract: configure() once the sample rate is known, feed() from the analysis thread (never blocks),
// telemetry() and the setters from the interface thread.
#pragma once
#include "mode_tuning.h"
#include "ring.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace dect2 {

struct AirbandTelemetry;   // airband_tel.h

constexpr double kAirbandAudioRate = 8000.0;   // the audio of each channel and of the mix
constexpr double kAirband833 = 25000.0 / 3;    // the 8.33 kHz raster step

// A channel of the user's list
struct AirbandChannel {
    double freqHz = 0;           // carrier frequency
    bool is833 = false;          // 8.33 kHz channel spacing (narrow filter)
    std::string label;           // the user's tag ("Tower")
    bool muted = false, solo = false, priority = false;
};

// Channel names (ICAO): a 25 kHz channel is named by its frequency ("118.025"); the three 8.33 kHz channels of the 25 kHz block that starts
// at X.XX0 / X.XX25 / X.XX50 / X.XX75 are named block + 5, + 10, + 15 kHz ("118.030" = 118.025 MHz, "118.035" = 118.03333, "118.040" =
// 118.04167). Names ending in 20, 45, 70 or 95 do not exist.
// Parses a channel name or a frequency in MHz ("118.0083", "118.008333"); false when it is neither on the 25 kHz nor on the 8.33 kHz raster
// or outside 118.000 - 136.990. A name ending in 0 or 5 that is also a frequency on the raster is read as the name (as a radio does).
bool airbandParse(const std::string& text, double& freqHz, bool& is833);
std::string airbandName(double freqHz, bool is833);   // the channel name: "118.010"
bool airbandOnRaster(double freqHz, bool is833);      // within 10 Hz of the raster

class AirbandReceiver {
public:
    AirbandReceiver();
    ~AirbandReceiver();

    void configure(double inputRateHz);
    void setSignalOffset(double hz);                     // where the user's (dial) frequency sits relative to 0 Hz in the input
    void setCenterHz(double hz);                         // the radio's centre frequency (0: the dial frequency of the test signal, for files)
    bool ready() const;                                  // false when the input rate is too low for this mode
    void reset();                                        // after a retune: forget the transmissions (the radio's tuning error is kept)
    void feed(const cf32* x, size_t n);
    bool telemetry(AirbandTelemetry& out, uint64_t lastSeq);
    void setLogCallback(std::function<void(const std::string&)> cb);   // events for the log pane; may be called from the receiver thread

    void setChannels(const std::vector<AirbandChannel>& ch);   // the list; mute, solo and priority changes do not restart the channels
    void setSquelchDb(float db);                         // carrier to noise threshold, -5 .. 30 dB (default 6)
    void setHangSec(float s);                            // 0 .. 3 s (default 0.5)
    void setScan(bool on);
    void setTuneErrorHz(double hz);                      // a starting guess of the radio's tuning error (tests); measured from then on

    // Sound (volume, mute) as in FmReceiver; the app pushes its shared controls here.
    void setVolume(float v);                             // 0 .. 1
    void setMuted(bool m);
    void setSilent(bool s);                              // decode but do not open the sound device (tests, command line)
    void setChannelTap(std::function<void(int chan, const float* x, size_t n)> cb);   // tests: each channel's 8 kHz audio after the squelch
    void setMixTap(std::function<void(const float* x, size_t n)> cb);                 // tests: the 8 kHz mix that goes to the sound card

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

ModeTuning airbandTuning();

} // namespace dect2
