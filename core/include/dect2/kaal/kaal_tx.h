// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// KAAL voice transmitter (port of kaal_tx.py, streaming). v3 (setBitrate 12/16/24/32): 16 kHz speech -> codec v3 frames -> rate-matched LDPC words
// of 480 bits -> 4-word interleave -> 16-QAM in frames of 160 symbols (20 ms) -> RRC -> sweep with the station ID in its variants. Legacy (setBitrate 0):
// 16 kHz speech -> codec frames (320 bits) -> LDPC -> pseudo-random interleave per
// 4-frame block -> 16-QAM -> pilots + 32-symbol preamble per frame -> RRC at 6 samples per symbol -> sweep signature at -10 dB, with the
// beacon slice optionally added. Generated at 48 kHz and resampled when the output rate differs.
#pragma once
#include <complex>
#include <cstddef>
#include <cstdint>
#include <string>
#include "dect2/kaal/kaal_beacon.h"

namespace dect2 {
namespace kaal {

// Output scale: the unit-power Python signal times this gives a peak of about 0.5 for speech.
constexpr float kTxGain = 0.4f;
// Mean power of the shaped data signal (symbol power of a frame, 210 / 202, spread over 6 samples per symbol). The sweep and the
// beacon levels are relative to this fixed value, since a stream cannot know its own average in advance (the Python measures it).
constexpr double kTxDataPower = 210.0 / 202.0 / 6.0;
constexpr double kTxBeaconDb = -10.0;   // beacon slice level relative to the data power
// The same for the v3 frames (32 preamble + 120 data + 8 pilots symbols = 166.4 / 160 symbol power).
constexpr double kTxDataPowerRt = 166.4 / 160.0 / 6.0;

// v3 transmitter air time per second of speech. 12 and 16 kbit/s fill one 160-symbol frame (20 ms) per codec frame: exactly real time.
// 24 and 32 kbit/s need two frames per codec frame and go out at 16000 symbols/s (the same stream at twice the speed, about 20 kHz wide): 1.0 too. (formerly 2.0: files only; fine for
// files, recordings and test signals). 0 = the legacy v0.1 format (25.25 ms per 20 ms, 1.2625).
double kaalAirTimeRatio(int kbit);

class VoiceTx {
public:
    VoiceTx();
    ~VoiceTx();
    VoiceTx(const VoiceTx&) = delete;
    VoiceTx& operator=(const VoiceTx&) = delete;

    void configure(double sampleRate);               // output sample rate; internally 48 kHz, resample if it differs. Resets the stream.
    void pushAudio(const int16_t* pcm, size_t n);    // 16 kHz mono speech in, any chunk size
    size_t popIq(std::complex<float>* out, size_t maxSamples);   // transmit IQ out, unit peak about 0.5; may return fewer; silence is NOT inserted
    void flush();                                    // pad the last partial codec frame and 4-frame block with zeros; ends the stream
    // 0 = the legacy v0.1 stream (codec v1, 202-symbol frames, single up sweep every 0.5 s; kept for the golden tests), 12 / 16 / 24 / 32 =
    // codec v3 at that bitrate in 160-symbol frames with the sweep period of 24240 samples. Resets the stream. Default 0 for VoiceTx itself;
    // the transmit generator makes 16.
    bool setBitrate(int kbit);
    int bitrate() const;
    // v3 only: the callsign carried in the sweep (6 characters of the sweep alphabet + suffix class, see kaal_sweepid.h). An empty or
    // unusable callsign turns the ID off (plain up sweeps). Resets the sweep cycle.
    bool setSweepCallsign(const std::string& callsign);
    void setBeaconMix(bool on);                      // when on, adds the beacon slice to the output on the 2 s / 10 s schedule
    void setBeacon(const BeaconFields& f, const std::string& text);

    size_t available() const;                        // IQ samples ready for popIq
private:
    struct Impl;
    Impl* p_;
};

}  // namespace kaal
}  // namespace dect2
