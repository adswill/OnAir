// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// TX studio, pure measurements on a block of complex samples: power spectrum, occupied bandwidth (99 % and -26 dB), PAPR, adjacent-channel power ratio and a
// check against a spectral mask, plus the impairments of the loopback check (noise, frequency offset, fading). No radio, no file, no global state.
#pragma once
#include <complex>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace dect2 {
using cf32 = std::complex<float>;
namespace txm {

// Averaged power spectrum (Hann window, 50 % overlap, up to maxSeg segments). Bins are in frequency order: bin 0 is -rate/2, bin nfft/2 is DC.
// pw: linear power per bin; the sum of pw is the mean power of the samples (window-corrected).
struct Psd {
    std::vector<double> pw;
    double rateHz = 0, binHz = 0;
    double freqOf(size_t bin) const { return ((double)bin - (double)pw.size() / 2.0) * binHz; }
    double db(size_t bin) const;                       // 10 log10 of pw[bin] (-200 for zero)
    bool empty() const { return pw.empty(); }
};
Psd welchPsd(const cf32* x, size_t n, double rateHz, int nfft = 4096, int maxSeg = 64);

// Bandwidth that holds `fraction` (0.99) of the total power, with the same share cut from each tail. 0 for an empty spectrum.
double obwHz(const Psd& p, double fraction = 0.99);
// Distance between the outermost bins within `dbBelowPeak` (26) of the highest bin of the spectrum smoothed over `smoothHz` (0: 8 bins; the smoothing keeps
// a single noise bin from stretching the figure). 0 for an empty spectrum.
double obwBelowPeakHz(const Psd& p, double dbBelowPeak = 26.0, double smoothHz = 0);
// Power-weighted centre of the spectrum (Hz, relative to the sample-rate centre)
double centroidHz(const Psd& p);

// Peak and RMS of the magnitude (dB re full scale 1.0) and their ratio. -200 dB / 0 for fewer than 2 samples or silence.
struct Papr { double peakDb = -200, rmsDb = -200, paprDb = 0, peak = 0; };
Papr papr(const cf32* x, size_t n);
// PAPR that only `prob` (e.g. 0.001) of the samples exceed: the figure that counts for a power amplifier (dB).
double paprAtProbabilityDb(const cf32* x, size_t n, double prob);

// Adjacent-channel power ratio: power in a channel `chanHz` wide centred `offsetHz` away from the signal's centre against the power in the signal's own
// channel (dB, negative is good). A side whose channel falls outside the sampled span is not valid: no number is made up.
struct Acpr { double lowerDb = 0, upperDb = 0; bool lowerValid = false, upperValid = false; double centerHz = 0; };
Acpr acpr(const Psd& p, double chanHz, double offsetHz, double centerHz);

// A spectral mask: limit in dB relative to the mean power density inside the occupied band, by distance from the centre (linear in dB between points).
struct MaskPoint { double offsetHz; double limitDb; };
struct Mask {
    std::string name;
    bool regulatory = false;           // false: a generic shoulder shape, NOT the mode's own standard
    std::vector<MaskPoint> pts;        // offsets rising; beyond the last point the last limit holds
    double limitAt(double offsetHz) const;
};
// 0 dB to the band edge (half the occupied width), -30 dB at 0.75 x the width from the centre, -50 dB from 1.5 x the width
Mask genericMask(double occupiedBwHz);
// The mask the mode is judged against. No standard's mask is built in yet, so this is the generic one, named as such (regulatory = false).
Mask maskForMode(const char* modeId, double occupiedBwHz);
// excess = level minus limit, so > 0 is a violation. Bins inside the occupied band are not judged. Not valid when the span does not reach past the band.
struct MaskResult { bool valid = false; bool pass = true; double worstExcessDb = -200, atHz = 0, refDb = 0; };
MaskResult checkMask(const Psd& p, const Mask& m, double occupiedBwHz, double centerHz);

// Impairments of the loopback. snrDb >= 99: no noise. SNR is signal power to noise power over the full sample-rate bandwidth.
struct Impair {
    double snrDb = 99;
    double cfoHz = 0;                  // carrier frequency offset
    int fading = 0;                    // 0 none, 1 flat Rayleigh, 2 two-path (second ray at -6 dB, delayUs later)
    double dopplerHz = 5;
    double delayUs = 5;
    uint32_t seed = 1;
    bool any() const { return snrDb < 99 || cfoHz != 0 || fading != 0; }
};
// In place and deterministic for a given seed. Fading keeps the mean power, so the SNR stays what was asked.
void applyImpairments(std::vector<cf32>& x, double rateHz, const Impair& im);

}} // namespace dect2::txm
