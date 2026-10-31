// Two-branch diversity combiner for narrowband signals heard by two radios tuned to the same frequency (two dongles, two antennas).
//
// Both inputs are complex baseband at the same nominal sample rate but come from free-running radios, so branch B differs from branch A by a
// delay (cable, USB, start-up), a frequency offset (clock error x carrier, a few hundred Hz to several kHz), a drifting sampling phase and an
// unknown complex gain. The combiner measures and removes all of that on its own, then merges the branches:
//   * Mrc       maximal-ratio combining per frequency bin: Y = (A/Na + conj(g) B/Nb) / (1/Na + |g|^2/Nb). The signal adds coherently, the
//               noises add as powers: two equal branches gain 3 dB, unequal ones gain at least as much as the better one.
//   * Selection the better branch of the moment (hysteresis 1 dB) is passed through, aligned and level-matched, with a cross-fade
//               (overlap-add) at the switch. Never worse than the better branch; the cheap fallback when the branches do not stay coherent.
// How it works (all in dsp/diversity.cpp): an STFT with sqrt-Hann frames (50 % overlap, N about fs/300 bins). Per bin it keeps smoothed
// power spectra of both branches and the smoothed cross spectrum. The noise level of each branch is the median bin power (the wanted signal
// is assumed to occupy well under half of the bins: a narrowband mode in a wide capture). The branch gain per bin is |g| = sqrt(Sb/Sa) from the
// noise-subtracted powers and the phase of the cross spectrum. The frequency offset is found by sliding the power spectrum of B over that of A
// (coarse, whole bins), then tracked by the phase advance of the cross spectrum from frame to frame; the delay by the phase slope of the cross
// spectrum over frequency (whole samples are removed in time, the rest is absorbed in the per-bin phase).
// Not for wide signals that fill the capture (the noise estimate fails) and not for signals the two antennas hear differently.
// Output: the combined stream aligned to branch A, delayed by latency() samples. Thread safety: one thread pushes and pulls.
#pragma once
#include "dect2/common/ring.h"
#include <memory>
#include <vector>

namespace dect2 {

struct DiversityConfig {
    enum Mode { Selection = 0, Mrc = 1 } mode = Mrc;
    double rateHz = 250e3;       // sample rate of both branches
    int fftSize = 0;             // 0 = about rate / 300 rounded to a power of two (256 .. 16384)
    double maxCfoHz = 40e3;      // search range of the frequency offset between the radios
    int maxLag = 0;              // delay window searched around the current alignment, in samples (0 = fftSize / 4)
    int lagLimit = 0;            // largest total delay the combiner follows (drift of the two sample clocks adds up); 0 = a quarter second
    float smooth = 0.95f;        // per-hop smoothing of the spectra (0.95 is about 40 hops)
};

struct DiversityStatus {
    bool locked = false;         // the branches are coherent: the combiner works, otherwise it passes branch A
    double cfoHz = 0;            // frequency offset removed from B
    int lagSamples = 0;          // whole-sample delay removed from B
    double snrADb = -99, snrBDb = -99;   // in-band signal-to-noise estimates of each branch (over the bins that carry the signal)
    double gainBDb = 0;          // level of B relative to A
    double outSnrDb = -99;       // predicted SNR of the combined output
    int selected = 0;            // Selection: 0 = A, 1 = B (Mrc: 2)
    int bins = 0;                // bins that carry the signal
    uint64_t frames = 0;
};

// Coarse alignment for the start: two free-running radios deliver their first samples tens of milliseconds apart, far more than the combiner's
// own delay window (fftSize / 4 samples). n samples (a power of two, at least 16384) taken from the heads of the two queues are compared: the
// power spectra find the frequency offset to within a few bins, then the cross spectrum over the signal bins is summed coherently for the
// shifts around it and the delay is read from the peak of its inverse FFT. skewSamples > 0: B lags A, so drop that many samples from the head of
// B (< 0: from A). Needs a signal both radios hear; ok = false when none stands out.
struct DiversityAlign {
    bool ok = false;
    int skewSamples = 0;
    double cfoHz = 0;
    float confidence = 0;          // peak against the typical level of the delay scan (above about 8 is solid)
};
DiversityAlign diversityCoarseAlign(const cf32* a, const cf32* b, size_t n, double rateHz, double maxCfoHz, int maxSkew);

class DiversityCombiner {
public:
    DiversityCombiner();
    ~DiversityCombiner();
    DiversityCombiner(const DiversityCombiner&) = delete;
    DiversityCombiner& operator=(const DiversityCombiner&) = delete;
    bool configure(const DiversityConfig& c);
    void setMode(DiversityConfig::Mode m);
    void reset();                                            // forget the alignment (after a retune)
    // equal numbers of samples from both branches; the combined samples are appended to `out` by pull()
    void push(const cf32* a, const cf32* b, size_t n);
    size_t pull(std::vector<cf32>& out);
    int latency() const;                                     // samples the output lags the input by
    int fftSize() const;
    DiversityStatus status() const;

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

} // namespace dect2
