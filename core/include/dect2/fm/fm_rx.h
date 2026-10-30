// FM broadcast radio receiver (87.5 - 108 MHz): channel filter, discriminator, stereo decoder (19 kHz pilot) and RDS (57 kHz).
// The station must be in the centre of the input; any input rate from 500 kHz up works.
#pragma once
#include "fm_tel.h"
#include "dect2/common/ring.h"
#include <cstdint>
#include <functional>
#include <memory>

namespace dect2 {

class FmReceiver {
public:
    FmReceiver();
    ~FmReceiver();

    void configure(double inputRateHz);
    bool ready() const;                                  // false when the input rate is below 500 kHz
    void reset();                                        // after a retune: forget the station, drop queued audio
    void feed(const cf32* x, size_t n);
    bool telemetry(FmTelemetry& out, uint64_t lastSeq);

    void setVolume(float v);                             // 0 .. 1
    void setMuted(bool m);
    void setMono(bool on);                               // force mono: the 19 kHz pilot and the stereo decoder are ignored (less noise on a weak station); off = stereo when the pilot is locked
    void setDeemphasis(double microseconds);             // 50 (Europe, Middle East, most of the world) or 75 (Americas, South Korea); 0: off
    // Issue #213 (any thread): 0 auto (stereo when the pilot is locked), 1 mono (as setMono(true)), 2 stereo whenever a pilot is there at all
    void setStereoMode(int mode);
    void setAudioBandwidth(int hz);                      // 0 or >= 15000: full; else a 4th order low pass at hz (12000, 8000, 5000), after the tone
    void setSquelch(float snrDb);                        // mute the sound while the SNR measure is below this (0: off), 2 dB hysteresis
    // Audio tone for the broadcast sound (after de-emphasis, 48 kHz): bass and treble shelves -6 .. +6 dB, and Warm (a -3 dB high shelf above
    // 6 kHz, on top of the treble). All zero and Warm off leaves the audio untouched (any thread).
    void setTone(int bassDb, int trebleDb, bool warm);
    // RDS meanings: 0 Auto, 1 RDS (Europe), 2 RBDS (US): the programme type names and the call letters in the PI code (fm_rbds.h). Auto reads the
    // tuned frequency (odd tenths = RBDS); tunedMhz is the station's frequency, give it again after a retune (any thread).
    void setRdsRegion(int region, double tunedMhz);
    // The channel filter's pass band, the whole width in Hz (0 = the standard 220 kHz); held only to a sane floor (kMinChannelWidthHz) and to what the sample rate carries; 3.4 removed the 100 - 300 kHz limit. Narrower keeps a
    // strong neighbour out at the cost of distortion on loud passages and of RDS; it takes effect with the next samples (any thread).
    static constexpr double kDefaultChannelWidthHz = 220e3, kMinChannelWidthHz = 10e3, kMaxChannelWidthHz = 300e3;   // kMax: the end of the slider's range (a typed width goes beyond it)
    void setChannelWidth(double hz);
    void setStationFinder(bool on);                      // look for a station off the centre and move to it (recordings); off for a live radio
    double channelWidth() const;
    void setSilent(bool s);                              // decode but do not open the sound device (tests, command line)
    // RDS error correction: 0 off (only blocks with a right checkword), 1 bursts of one or two bits (one wrong symbol), 2 also two weak
    // symbols chosen by their reliability (default). A group with a corrected block is used only when it has come the same before.
    // Call before feeding (tests, the command line).
    void setRdsCorrection(int level);
    // Every RDS group as it ends (blocks A-D; err 0 good, 1 corrected, 2 bad), from feed(): a group log, redsea-style hex (rdsHexGroup)
    void setRdsGroupTap(std::function<void(const uint16_t blocks[4], const uint8_t err[4])> cb);
    // Tests: the demodulated audio (left, right) at 48 kHz, called from feed()
    void setAudioTap(std::function<void(const float* left, const float* right, size_t n)> cb);

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

} // namespace dect2
