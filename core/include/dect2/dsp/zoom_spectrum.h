// The zoom spectrum (4.0, docs/WATERFALL-4.0.md): when the view is zoomed in so far that the full-band FFT has fewer bins than the plot has
// pixels, the visible part of the band is mixed down to DC, decimated by a power of two (a 4-stage CIC, then half-band filters) and
// transformed on its own, so the view gets at least one real bin per screen pixel at a small fraction of the cost of a full-band FFT that
// fine. The app draws the trace and a waterfall history of its own from these frames (app/gfx/zoom_detail.h).
//
// Also here: the window functions every display FFT can use (specwin), and the pure plan maths (zoomfft) that the tests check.
#pragma once
#include "dect2/common/ring.h"
#include <cstddef>
#include <cstdint>
#include <vector>

namespace dect2 {

// ---- window functions -------------------------------------------------------------------------------------------------------------------
namespace specwin {
enum Type { Hann = 0, BlackmanHarris = 1, Nuttall = 2, FlatTop = 3, Rect = 4, NumTypes = 5 };
const char* name(int type);
// n points of the window, scaled to unit rms (sum of w^2 = n), so a frame of white noise reads the same level whatever the window.
// Hann is hannWindowNorm (bit for bit what the analyser always used).
void make(int type, float* w, int n);
// The equivalent noise bandwidth of the window in bins (Hann 1.5, Blackman-Harris 2.0, flat top 3.8, rectangular 1)
double enbwBins(int type);
}  // namespace specwin

// ---- the plan: which decimation and how many bins a view needs -------------------------------------------------------------------------
namespace zoomfft {
constexpr double kUsable = 0.75;            // the part of the decimated band the display trusts (the half-bands pass 0.78 of it flat)
constexpr double kFill = 0.5;               // a new plan's view fills at most this part of the decimated band: room to pan without a restart
constexpr size_t kMinBins = 512;
constexpr size_t kMaxBins = size_t(1) << 20;   // 1M: the finest transform anything here makes
constexpr int kMaxDecim = 1 << 16;
constexpr int kMaxCic = 1 << 10;            // the CIC's factor at most (its integer registers keep 64 bits: input 23 bits + 4 x 10)

struct Plan {
    bool on = false;
    int decim = 1;          // D: the decimated rate is fs / D
    int cic = 1;            // R: the CIC's part of D (D = R * 2^halfbands)
    int halfbands = 0;
    size_t bins = 0;        // N: the transform's length at the decimated rate
    double offsetHz = 0;    // the centre of the zoom band from the capture's centre (a whole number of bins: a pan shifts the rows exactly)
    double rateHz = 0;      // fs / D
    double binHz = 0;       // rateHz / bins
    uint64_t id = 0;        // set by the engine: a new plan gets a new id
    double usableLo() const { return offsetHz - kUsable * rateHz * 0.5; }
    double usableHi() const { return offsetHz + kUsable * rateHz * 0.5; }
    bool sameShape(const Plan& o) const { return on == o.on && decim == o.decim && bins == o.bins; }
};

// The plan for a view spanHz wide whose centre is offsetHz from the capture's centre, drawn over px pixels, from a capture of fs.
// D: the largest power of two (2 .. kMaxDecim) with span <= kFill * fs / D; N: the power of two >= 2 * px / kFill (so a bin per pixel or
// finer for any span this D takes, and N stays the same over the whole octave of zoom), within [kMinBins, maxBins]. Off when even D = 2
// does not leave room (the view is more than a quarter of the band) or the inputs make no sense.
Plan make(double fs, double offsetHz, double spanHz, int px, size_t maxBins = kMaxBins);
// cur still serves the view: the same shape make() would choose, and the view inside cur's usable band.
bool covers(const Plan& cur, double fs, double offsetHz, double spanHz, int px, size_t maxBins = kMaxBins);
// What to run for the view: off when the full-band FFT (fullBins over fs) already gives a bin per pixel; cur when it covers the view
// (nothing restarts); else a new plan (id = cur.id + 1).
Plan update(const Plan& cur, double fs, size_t fullBins, double offsetHz, double spanHz, int px, size_t maxBins = kMaxBins);
// the split of D into the CIC factor and the half-band stages (at most two half-bands unless the CIC would pass kMaxCic)
void split(int decim, int& cic, int& halfbands);
}  // namespace zoomfft

// ---- the decimator: mixer, CIC, half-bands -----------------------------------------------------------------------------------------------
// Mixes by -offsetHz (an NCO renewed from a double phase every 1024 samples), decimates by R in a 4-stage CIC on 64-bit integers and by two
// in each half-band (47 taps, Kaiser: about 75 dB of alias rejection over the usable band). The gain is 1 in the passband.
class ZoomDecimator {
public:
    void setup(double fs, double offsetHz, int cic, int halfbands);
    void reset();
    // appends the decimated samples of x to out
    void run(const cf32* x, size_t n, std::vector<cf32>& out);
    static int halfbandTaps();            // the length of the half-band filter
    static const float* halfbandCoefs();  // its odd taps h[1], h[3], ... (the centre is 0.5)
private:
    double fs_ = 0, w_ = 0, ph_ = 0;
    int R_ = 1, H_ = 0, cnt_ = 0, shift_ = 0;
    double scale_ = 1, outScale_ = 1;
    uint64_t ig_[2][4] = {}, dl_[2][4] = {};
    struct Hb { std::vector<cf32> buf; size_t next = 0; };
    std::vector<Hb> hb_;
    std::vector<cf32> a_, b_;
    float phr_[1032] = {}, phi_[1032] = {}, mr_[1024] = {}, mi_[1024] = {};   // a piece's phasors and mixed samples
};

// ---- the zoom analyser ------------------------------------------------------------------------------------------------------------------
struct ZoomFrame {
    std::vector<float> dbfs;   // plan.bins values, DC-centred: bin i is at offsetHz + (i - bins / 2) * binHz from the capture's centre
    zoomfft::Plan plan;
    uint64_t seq = 0;
};

// One feeding thread (the engine's spectrum thread) and one taking thread, like SpectrumAnalyzer: the transforms run without the lock.
class ZoomSpectrum {
public:
    ZoomSpectrum();
    ~ZoomSpectrum();
    ZoomSpectrum(const ZoomSpectrum&) = delete;
    ZoomSpectrum& operator=(const ZoomSpectrum&) = delete;
    void configure(const zoomfft::Plan& p, double fs);   // a plan with another id or rate restarts; plan.on = false stops all work
    zoomfft::Plan plan() const;
    void setWindow(int type);
    void setBlocksPerFrame(size_t n);                     // as SpectrumAnalyzer: short transforms average up to n blocks a frame
    void feed(const cf32* x, size_t n);
    bool takeFrame(ZoomFrame& out);                       // false when nothing new (or off)
    void reset();                                         // forget the samples (after a retune); the plan stays
private:
    struct Impl;
    Impl* p_;
};

}  // namespace dect2
