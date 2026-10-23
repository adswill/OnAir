// "Synthetic wideband": 2-5 ordinary narrow radios (RTL-SDRs, mixed types), each tuned to an adjacent slice with overlap and all at the same nominal
// sample rate, are stitched into ONE wide complex stream (rate = M * fs/N). Pure DSP, no hardware; the app feeds each radio's blocks to push().
//
//   per radio: FracResampler (clock drift ppm + frequency offset removed, closed loop) -> ring
//   per hop N/2: sqrt-Hann frame, N-FFT -> per-slice gain/phase/delay (measured on the overlap with the neighbour) -> weighted stitch into an M-bin
//   spectrum (raised-cosine cross-fade across each overlap) -> M-IFFT -> sqrt-Hann -> overlap-add -> wide samples (hand to the channelizer / WideBank).
// Alignment is measured on the signal the neighbours have in common (anything wideband that both hear: noise, many carriers; it needs some).
//   coarse frequency offset: cross-correlation of the two overlap power spectra (integer bins); fine: phase advance of the cross-spectrum per hop
//   delay (fractional samples) + constant phase: peak of the transform of the accumulated cross-spectrum; gain: power ratio
//   clock drift: slope of the measured delay over time, fed back into the resampler (so it also follows temperature drift)
//   phase tracking (SynthCfg::trackPhase): every hop the relative phase of each neighbour pair is measured again on that hop's own overlap and
//   filtered by a two-state Kalman filter (phase, phase step per hop); the stitch of the same hop uses it, so there is no prediction lag. This
//   follows the free-running radios' phase wander down to what moves inside one frame (hop N/2 samples; keep N small: 1024 at 2.4 Msps).
// COHERENCE: the stitched stream is amplitude/frequency/time aligned and phase-continuous across the seams as far as the measurement holds, but the
// radios' oscillators are free-running: the phase relation between slices wanders (phase noise, residual ppm). The per-hop tracking removes most of
// it when the overlap holds a signal both radios hear (a broadcast multiplex does; empty noise does not). Suits: spectrum/waterfall, FM/AM/NFM,
// narrow digital voice, scanning, anything inside one slice or tolerating a slowly varying seam. Wide OFDM across seams (DVB-T) is NOT measured: it is expected to be limited by the
// free-running radios' phase (no measurement exists yet; docs/SYNTH-WIDEBAND.md, where present, lists only what was measured).
// Not for: direction finding/beamforming (needs a true shared clock and calibration: KrakenSDR), GNSS.
#pragma once
#include "dect2/common/ring.h"
#include "dect2/dsp/fftutil.h"
#include <cstddef>
#include <memory>
#include <vector>

namespace dect2::synth {

// ---- pure helpers (unit tested) ----
std::vector<float> sqrtHann(int n);                       // periodic; its square sums to 1 at hop n/2
float fade(int i, int n);                                 // raised-cosine 0 -> 1 over n steps; fade(i,n)+fade(n-1-i,n) == 1
// Integer shift d (bins) with pa(w) ~ pb(w - d), searched in +-maxLag; confidence = peak / mean of the correlation
int estimateShiftBins(const std::vector<float>& pa, const std::vector<float>& pb, int maxLag, float* confidence = nullptr);
struct DelayPhase { double delay = 0; double phase0 = 0; double strength = 0; };   // samples (B lags A when > 0), rad at bin 0, peak/sum|cross| 0..1
// cross[u] = sum over frames of A(u)*conj(B(u)) for the W overlap bins u = 0..W-1; n = the slice FFT size
DelayPhase estimateDelay(const std::vector<cf32>& cross, int n);
// Hz that B is still too low by, from the phase advance of the per-hop cross sums (hopSec apart)
double fineOffsetHz(const std::vector<cf32>& crossPerHop, double hopSec);
double regressSlope(const std::vector<double>& t, const std::vector<double>& y);   // least squares
// Residual of A against B after gain and phase phi(u) = phase0 + 2 pi delay u / n: 10 log10( sum|A - g B|^2 / sum|A|^2 )
double mismatchDb(const std::vector<cf32>& cross, const std::vector<float>& pa, const std::vector<float>& pb, int n, const DelayPhase& dp, double gain);

// 24-tap Kaiser-sinc resampler (flat to 0.4 fs, so a slice's overlap near its band edge is not damaged) + mixer: reads the input at (1 + ppm*1e-6) samples per output sample and multiplies by exp(j 2 pi off t). Changes act at once.
class FracResampler {
public:
    void configure(double fs) { fs_ = fs; reset(); }
    void reset() { buf_.assign(11, cf32(0, 0)); pos_ = 11; ph_ = 0; }
    void set(double ppm, double offHz) { ppm_ = ppm; off_ = offHz; }
    double ppm() const { return ppm_; }
    double offsetHz() const { return off_; }
    void process(const cf32* x, size_t n, std::vector<cf32>& out);   // appends
private:
    double fs_ = 1, ppm_ = 0, off_ = 0, pos_ = 11, ph_ = 0;
    std::vector<cf32> buf_;
};

// ---- the stitcher ----
constexpr int kSynthMaxSlices = 5;

struct SynthCfg {
    int slices = 2;            // 2..kSynthMaxSlices radios
    double fs = 2.4e6;         // the radios' common nominal sample rate
    int n = 4096;              // slice FFT (power of 2)
    int stepBins = 2400;       // slice spacing in bins (even); centre spacing = stepBins * fs / n
    double usable = 0.8;       // fraction of fs per slice that is trusted (the converter rolls off at the edge)
    int alignFrames = 32;      // hops accumulated per alignment solve
    double maxOffsetHz = 40e3; // coarse search range of the radios' frequency error
    bool trackPhase = true;    // per-hop phase tracking of every seam (see the top of this file)
    double trackLinewidthHz = 60; // the phase wander the tracker expects between two radios (sum of their Lorentzian linewidths); sets its process noise
};

struct PairStatus {
    bool valid = false;
    double mismatchDb = 0;     // residual in the overlap after the correction (~ -30 good, > -10 poor)
    double gainDb = 0;         // slice b relative to a
    double delaySamples = 0;
    double confidence = 0;     // 0..1, how coherent the common signal is in the overlap
    bool tracking = false;     // the per-hop phase tracker of this seam is running
    double trackRmsDeg = 0;    // rms of the tracker's per-hop phase corrections (how much the seam phase moves from hop to hop)
};
struct SliceStatus {
    bool alive = true;
    bool locked = false;       // the alignment of this slice has converged
    double offsetHz = 0;       // frequency error relative to slice 0, as corrected
    double driftPpm = 0;       // sample clock difference relative to slice 0, as corrected
    long long dropouts = 0;
};

class SynthWideband {
public:
    // false on a bad configuration (overlap < 64 bins, odd step, n not a power of 2, slices outside 2..kSynthMaxSlices)
    bool configure(const SynthCfg& c);
    const SynthCfg& cfg() const { return c_; }
    double outRate() const { return (double)m_ * c_.fs / c_.n; }
    double centerOffsetHz() const { return 0.5 * (c_.slices - 1) * c_.stepBins * c_.fs / c_.n; }   // of the wide stream, from slice 0's centre
    double spanHz() const { return ((c_.slices - 1) * c_.stepBins + usableBins_) * c_.fs / c_.n; }
    // radio `slice` delivers a block (any size); processes whatever is complete
    void push(int slice, const cf32* x, size_t n);
    // a radio that stopped / came back (a radio that falls far behind the others is also marked down by itself)
    void setAlive(int slice, bool alive);
    // stitched samples produced so far (appends, at outRate()); returns the count
    size_t pull(std::vector<cf32>& out);
    PairStatus pair(int i) const { return pairs_[i]; }     // between slice i and i+1
    SliceStatus slice(int i) const { return sl_[i]; }
    // averaged power spectrum of the wide stream (dB rel. full scale, M bins, negative frequencies first)
    std::vector<float> spectrumDb() const;
    // start the alignment again from nothing (frequency, delay, drift, gain, phase): the "recalibrate" button, or after a radio was retuned
    void recalibrate();
    // centre of slice i in the wide stream (Hz from the wide stream's centre) and the overlap width: where the seams are (DVB-T per-segment phase)
    double sliceCentreHz(int i) const { return (i - 0.5 * (c_.slices - 1)) * c_.stepBins * c_.fs / c_.n; }
    double overlapHz() const { return ov_ * c_.fs / c_.n; }

private:
    struct Radio {
        FracResampler rs;
        std::vector<cf32> ring;          // corrected samples (after the resampler)
        size_t head = 0;
        int pendDrop = 0;                // coarse delay fix not yet applied (samples still to drop from the incoming stream)
        bool alive = true, manualDown = false;
        double off = 0, ppm = 0;
        std::vector<cf32> spec;          // current frame spectrum (n bins, centre in the middle)
    };
    struct Acc {
        std::vector<cf32> cross, rows, crossT; std::vector<float> pa, pb;   // rows: per-hop cross products, frames x W; crossT: with the tracked phase removed
        int frames = 0, trkFrames = 0;
        void clear(int w) { cross.assign(w, cf32(0, 0)); crossT.assign(w, cf32(0, 0)); pa.assign(w, 0.f); pb.assign(w, 0.f); rows.clear(); frames = 0; trkFrames = 0; }
    };
    struct Track {                       // per pair: Kalman filter of the seam phase (rad at overlap bin 0, delay slope removed)
        bool init = false;
        double phi = 0, w = 0;           // phase, phase step per hop
        double p00 = 1, p01 = 0, p11 = 1;
        double delay = 0;                // the pair's delay the measurement removes (from the last solve)
        double sq = 0; int nsq = 0;      // running sum of squared corrections (trackRmsDeg)
    };
    void trackHop();
    void rebuildWeights();
    void run();
    bool haveFrame(int i) const;
    void hop();
    void solve(int p);
    void shiftSlice(int i, int d);
    void markDown(int i);

    SynthCfg c_;
    int m_ = 0, usableBins_ = 0, ov_ = 0;
    std::vector<Radio> r_;
    std::vector<Acc> acc_;
    std::vector<Track> trk_;
    std::vector<PairStatus> pairs_;
    std::vector<SliceStatus> sl_;
    std::vector<double> amp_, ph_, tau_;     // per slice correction relative to slice 0: amp * exp(j(ph + 2 pi tau w / n)), w = wide bin
    std::vector<std::vector<float>> wt_;     // per slice weights over its usable bins
    std::vector<float> win_, wwin_, spAvg_;
    std::unique_ptr<Fft> fftN_, fftM_, fftL_;
    std::vector<cf32> olap_, outQ_;
    std::vector<std::vector<double>> tH_, dH_;   // per pair: time and total delay seen (for the drift slope)
    std::vector<double> shifted_;                // per slice: integer sample shifts applied so far
    std::vector<int> stage_;                     // per pair: 0 coarse offset, 1 fine offset, 2 delay/gain tracking
    std::vector<float> lastMismatch_;
    // per slice: how fast its frequency correction moves (Hz/s, thermal drift against slice 0), measured while it is aligned; while its left
    // neighbour is down nothing can measure it, so the correction goes on at that rate (a middle radio that drops out does not leave the
    // slices to its right drifting)
    std::vector<double> offRate_, lastOff_, lastOffT_;
    double t_ = 0;
};

} // namespace dect2::synth
