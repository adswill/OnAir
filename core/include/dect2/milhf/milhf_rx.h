// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Military / government HF waveform identifier: measures the physical layer of an audio-rate signal (occupied band, baud rate, tone comb,
// constellation order, frame periodicity, pilots, bursts) and ranks candidate waveforms. IDENTIFICATION ONLY: nothing is decoded,
// no message content is read and no encryption is touched. The candidates are estimates from measured features, not decoder results.
#pragma once
#include <cstddef>
#include <string>
#include <vector>

namespace dect2::milhf {

struct Features {
    bool signal = false;          // something above the noise floor
    double snrDb = 0;             // in-band power over the noise floor
    double loHz = 0, hiHz = 0, bwHz = 0, centreHz = 0;   // occupied band
    double baudHz = 0, baudDb = 0;       // envelope clock line (0: none) and its height over its surroundings
    int order = 0;                // constellation guess at the baud: 2, 4, 8, 99 (higher or unclear), 0 (not tried)
    double orderScore = 0;        // strength of the M-th power line (noise is about 10)
    int frameLagSym = 0;          // repetition period of the symbol stream (known preamble / probes), symbols; 0 none
    double frameLagMs = 0, frameCorr = 0;
    int nTones = 0;               // spectral peaks in the band
    double toneSpacingHz = 0, combScore = 0;   // regular tone spacing and how regular (0..1)
    double shiftHz = 0;           // two-peak signals: the distance of the peaks
    bool pilot605 = false; double pilot605Db = 0;   // narrow steady tone at 605 Hz (Link 11 Doppler tone)
    bool sync2915 = false;        // narrow tone at 2915 Hz (Link 11 sync tone)
    double carrierHz = 0, carrierDb = 0;   // strongest narrow steady line (0: none)
    double envModHz = 0, envModDb = 0;     // strongest slow envelope modulation, 0.5 - 40 Hz (buzzers, keyed signals)
    double duty = 1; int bursts = 0;       // fraction of time on and number of bursts (0 bursts: continuous)
};

struct Candidate {
    std::string name;
    float conf = 0;               // 0..1, an estimate
    std::string basis;            // which measurements support it
    std::string note;             // what the waveform is (no content)
    std::string url;              // public source
};

struct Result {
    Features f;
    std::vector<Candidate> cands;   // best first; empty when no signal
};

// x: real audio (upper sideband), fs 8000 - 24000 Hz; needs at least about 0.7 s, uses the latest 5.5 s
Result analyze(const float* x, size_t n, double fs);

// Averaged power spectrum in dB (nfft/2+1 bins) for the spectrogram snippet
void psdDb(const float* x, size_t n, double fs, int nfft, std::vector<float>& outDb);

}  // namespace dect2::milhf
