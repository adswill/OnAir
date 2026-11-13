// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Sound-card input for narrowband IF and audio-frequency signals (issue #137): a receiver's 12 kHz IF, SSB audio, an AF tap.
// The sound card gives real samples; decoders want complex IQ with the signal where the dial says. audioIfProcess() does the steps in order:
// gain, DC removal, channel choice, real -> analytic (Hilbert), shift the IF down to 0 Hz, optional sideband inversion.
#pragma once
#include "dect2/source/source.h"
#include <string>

namespace dect2 {

// TuneSettings::radio keys: audio_iq (AudioIqMode 0..3; 3 = Real (mono): analytic, like USB), audio_chan (0 left, 1 right, 2 mix), audio_if_hz (the carrier/centre's audio
// frequency, Hz), audio_invert (0/1), audio_gain_db, audio_dc (0/1, default 1 for the real modes), audio_rate (Hz, 8000..192000).
struct AudioIfConfig {
    AudioIqMode mode = AudioIqMode::StereoIq;
    int chan = 0;
    double ifHz = 0;
    bool invert = false;
    float gainDb = 0;
    bool dc = true;
    int rate = 48000;
    double minOutRate = 0;   // audio_min_rate: the lowest rate the mode's receiver takes (the engine sets it); a lower card rate is interpolated up to the next whole multiple
};
// The rate the source delivers for a card at `rate` Hz: a whole multiple of it that reaches minOut, or the card rate itself (minOut 0, or more than the receivers use, 1.5 Msps)
int audioOutFactor(double rate, double minOut);
AudioIfConfig audioIfConfigOf(const TuneSettings& s);

struct AudioIfState {
    AudioHilbert hil;
    double phase = 0;        // the IF shifter's oscillator, radians
    float dcL = 0, dcR = 0;  // the DC trackers
    float peak = 0;          // input level after gain, 0..1+, decays
    uint64_t frames = 0;     // frames seen
    uint64_t clipFrame = 0;  // the last frame at which a sample reached full scale (0: none yet)
    double sLL = 0, sRR = 0, sLR = 0;   // running power of each channel and their product (decays over about a second): are L and R the same signal?
};

// n stereo frames in (interleaved L,R), n complex samples out at cfg.rate
void audioIfProcess(const float* stereo, int n, const AudioIfConfig& cfg, cf32* out, AudioIfState& st);

// The sound-card source's own checks, for the panel: clipping seen in the last two seconds, and the channel layout the signal looks like
bool audioSourceClipping();
// 0 unknown / silent, 1 left and right are the same signal (an audio feed on both channels: pick a Real mode), 2 left and right differ (a stereo I/Q pair or two different signals)
int audioSourceLayoutHint();
// The input level of the running sound-card source, 0..1 (1 = full scale; >0.95 clips): for the meter. 0 when none runs.
float audioSourceLevel();
// Where the audio spectrum's centre lies, as a frequency from the dial, when a Real mode shows the card's whole 0..Nyquist range
// (display only: the decoders keep the IF centre at 0 Hz). 0 in the I/Q mode, where the card's range is already centred on the dial.
double audioAfViewOffsetHz(const AudioIfConfig& cfg);
// From a spectrum frame (dB bins, DC-centred over `rate` Hz, centred on the dial + viewHz): the audio frequency of the signal's centre
// (the IF centre to enter), and its occupied width. False when nothing stands out.
bool audioDetectCentre(const float* dbfs, int n, double rate, double viewHz, const AudioIfConfig& cfg, double& centreHz, double& widthHz);
// The sample rates offered for a sound card (the card resamples to the one picked)
const int* audioRateChoices(int& n);

} // namespace dect2
