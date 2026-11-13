// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Signal workbench: the shared signal-processing pieces: windows, FFT of any power of two, FIR design, band extraction (mix, filter and
// decimate a time range of a recording to a band), power spectra.
#pragma once
#include "wb_source.h"
#include <functional>
#include <string>
#include <vector>

namespace dect2 {
namespace wb {

enum Window { WinRect = 0, WinHann, WinHamming, WinBlackman, WinBlackmanHarris, WinFlatTop, WinKaiser, WinCount };
const char* windowName(int w);
std::vector<float> makeWindow(int w, int n);

// In-place FFT of the 1 << log2n points of x (forward, unnormalised); re and im are scratch of at least n floats
void fft(cf32* x, int n, std::vector<float>& re, std::vector<float>& im);

// Low-pass FIR (windowed sinc, Blackman), cutoff and transition width as a fraction of the sample rate (0 .. 0.5), unit DC gain
std::vector<float> designLowpass(double cutoff, double transition);

// Mix, filter and decimate: the stream that arrives through push() has the band [centreHz - bwHz / 2, centreHz + bwHz / 2] moved to
// baseband, low-passed and decimated by decim (a product of the stages it chose: see outRate()). Real input goes through a Hilbert
// transformer first, so only the wanted sideband comes out.
class BandExtractor {
public:
    // wantedRate: the output rate is the source rate divided by an integer, at least wantedRate when the source allows
    BandExtractor(double srcRate, bool realInput, double centreHz, double bwHz, double wantedRate);
    ~BandExtractor();
    double outRate() const;
    int decimation() const;
    void push(const cf32* in, size_t n, std::vector<cf32>& out);   // appends
private:
    struct Impl;
    Impl* p_;
};

struct Extracted {
    std::vector<cf32> x;
    double rate = 0;          // sample rate of x
    double centreHz = 0;      // where its zero sits in the source (from the source's DC)
    double bwHz = 0;
    uint64_t start = 0;       // first source sample taken
    uint64_t srcCount = 0;    // source samples taken
    int decim = 1;
    bool truncated = false;   // the range was longer than maxOut allowed
};
// A time range [start, end) of the source, the band round centreHz (bwHz <= 0: everything, no mixing), at least wantedRate (0: as is).
// Longer than maxOut output samples: the range is cut short (truncated). progress, when set, is called with 0 .. 1 and may return false to cancel.
bool extractBand(const Source& s, uint64_t start, uint64_t end, double centreHz, double bwHz, double wantedRate, Extracted& out,
                 size_t maxOut = 1u << 25, const std::function<bool(double)>& progress = nullptr);

// Power spectrum (dBFS: a complex tone of amplitude 1 reads 0 dB; a real one of amplitude 1 reads 0 dB too) of n samples x averaged over
// segments of fftSize with the given overlap. out has fftSize bins in FFT order shifted so that bin 0 is -rate/2 (complex) or fftSize / 2 + 1
// bins 0 .. rate/2 (real).
void welch(const cf32* x, size_t n, int fftSize, int window, double overlap, bool real, std::vector<float>& outDb);

double dbOf(double power);   // 10 log10, floored

// Linear-interpolated resampling of a real signal from one rate to another, low-passed first when going down
std::vector<float> resampleReal(const std::vector<float>& in, double inRate, double outRate);

} // namespace wb
} // namespace dect2
