// Finds DRM signals in a wide capture so that a receiver is only started where there is one. On the averaged spectrum a DRM signal is a block of a few kHz with no
// carrier line and steep skirts; what proves it is OFDM with the right numerology: the guard interval correlation at the useful symbol time Tu of a robustness mode,
// accumulated over the symbol period (ES 201 980 clause 8.2). AM, SSB and noise have no such peak at any symbol phase.
#pragma once
#include "dect2/dsp/carrierfind.h"
#include <memory>
#include <vector>

namespace dect2 {

struct DrmSpanHit {
    double hz = 0;               // the reference frequency (carrier 0) relative to the span centre: what a single DRM receiver would be tuned to
    double widthHz = 0, loHz = 0, hiHz = 0;   // the occupied width and block measured on the spectrum
    int mode = -1;               // 0 A, 1 B, 2 C, 3 D, 4 E
    float corr = 0;              // guard interval correlation 0 .. 1
    float snrDb = 0;
    double lastSec = 0;          // span time it was last confirmed
};

class DrmFinder {
public:
    DrmFinder();
    ~DrmFinder();
    void configure(double spanRate, double centreHz = 0);        // a known centre lets the reference snap to the 1 kHz grid DRM channels are on
    void setCentre(double centreHz);
    void reset();
    void feed(const cf32* x, size_t n);
    bool takeChanged(std::vector<DrmSpanHit>& out);              // true when the list of confirmed signals changed since the last call
    std::vector<DrmSpanHit> hits() const;
    int candidates() const;
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

// The best guard correlation of x (rate 12 kHz for modes A..D, 96 kHz for mode E): returns the mode (-1 none) and its normalised correlation and the ratio of its peak over the mean of the other symbol phases
int drmGuardTest(const cf32* x, size_t n, bool modeE, float& corr, float& peakOverMean);
// Where the reference frequency lies for a block occupying [lo, hi] (Table 49: 4.5 and 5 kHz start at the reference, 9 and 10 kHz centre on it, 18 and 20 kHz start about 4.5 kHz below it)
double drmReferenceFromBlock(double loHz, double hiHz);

} // namespace dect2
