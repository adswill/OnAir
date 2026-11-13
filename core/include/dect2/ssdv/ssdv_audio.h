// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// SSDV from an audio recording (a WAV file). The SSDV receiver (ssdv_rx.h) takes the samples of an FM / FSK channel; a recording of the radio's
// speaker output is that channel after the FM discriminator: the FSK bits as an audio waveform (a 9600 baud G3RUH-style baseband, or the
// 1200 - 19200 baud baseband an FM receiver gives). This reads such a WAV and turns it back into a frequency-modulated carrier at a rate the
// receiver accepts, so the same demodulator, clock recovery and packet decoder run on it. Nothing here transmits.
// Read: any sample rate; 8, 16, 24 or 32-bit PCM or 32 / 64-bit float; any number of channels (the first one is used, or the mean when the
// channels are alike). Not handled: AFSK tone pairs (the tones would have to be demodulated to bits first) and compressed WAV.
#pragma once
#include <complex>
#include <string>
#include <vector>

namespace dect2 {

struct WavAudio { double rate = 0; int channels = 0; std::vector<float> mono; };

// The WAV file as mono floats (peak not normalised). false + err for a file that is not a readable PCM / float WAV.
bool readWavAudio(const std::string& path, WavAudio& out, std::string& err);
// Audio -> IQ of a frequency-modulated carrier at `outRate` (a multiple-friendly 192 kHz at least). The audio is scaled to its peak.
bool ssdvAudioToIq(const std::vector<float>& audio, double audioRate, std::vector<std::complex<float>>& iq, double& outRate, std::string& err);
// WAV -> cf32 file (interleaved float I, Q) at outRate; the caller plays it as an IQ file of that rate.
bool ssdvWavToCf32File(const std::string& wavPath, const std::string& cf32Path, double& outRate, std::string& err);

}  // namespace dect2
