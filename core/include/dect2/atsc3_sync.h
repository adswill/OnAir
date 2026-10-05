// ATSC 3.0 synchronisation on a continuous stream of samples: finding the bootstrap with an unknown carrier frequency offset, refining the
// offset from the guard intervals, cutting out the frame and handing it to the frame decoder, and finding the next bootstrap.
#pragma once
#include "atsc3_bootstrap.h"
#include "atsc3_receiver.h"
#include "resampler.h"
#include <deque>
#include <memory>
#include <vector>

namespace dect2 {
namespace atsc3 {

struct BootstrapFind {
    bool found = false;
    double start = 0;          // index of the first sample of the bootstrap (6.144 Msamples/s domain)
    double cfoHz = 0;          // carrier frequency offset: the signal is at +cfoHz from the tuned frequency
    float metric = 0;
    Detection det;             // the signalling read from it
};

// Searches `n` samples at 6.144 Msamples/s. `maxCfoHz` bounds the offsets tried (the integer part is searched in steps of the 3 kHz carrier spacing).
// Needs at least 4 symbols after the start of the bootstrap.
BootstrapFind findBootstrap(const cf32* x, size_t n, double maxCfoHz = 20000.0, double centerHz = 0.0, int seedOnly = -1);   // centerHz: a known approximate offset, searched around

// Resamples a block by any ratio with an exact (double) phase: out[k] is the signal at input position k * inRate / outRate + startOffset
// (in input samples, may be fractional). Edges are treated as zeros. Windowed sinc with interpolated phases; used where a rational
// approximation would give a sample rate error of tens of ppm.
void resampleExact(const cf32* in, size_t n, double inRate, double outRate, std::vector<cf32>& out, double startOffset = 0.0);

// Removes a carrier offset: y[i] = x[i] * exp(-j 2 pi f (i + startIndex) / rate).
void derotate(cf32* x, size_t n, double f, double rate, double startIndex = 0);

// Fractional carrier offset from the guard intervals of `symbols` consecutive OFDM symbols starting at x (guard + fft samples each), in Hz at `rate`.
double estimateCfoGuard(const cf32* x, size_t n, int fft, int guard, int symbols, double rate);

struct SyncStats {
    long bootstraps = 0, searches = 0, frames = 0;
    double secSearch = 0, secCut = 0, secDecode = 0;   // CPU time spent (diagnostics)
    double cfoHz = 0;
    bool locked = false;
};

// Streaming synchroniser: feed samples at the radio's rate; frames are decoded and given to the receiver.
class Atsc3Sync {
public:
    Atsc3Sync(double sampleRate, Atsc3Receiver* rx);
    ~Atsc3Sync();
    // Decode frames on `n` worker threads (the frames are independent; they reach the receiver in order). 0 or 1: decode inside push().
    void setThreads(int n);
    // Waits until every frame handed over so far has reached the receiver.
    void flush();
    void push(const cf32* x, size_t n);
    SyncStats stats() const;
    double sampleRate() const { return rate_; }

private:
    struct Pool;
    std::unique_ptr<Pool> pool_;
    void work();
    bool search();
    bool cutFrame();

    double rate_;
    Atsc3Receiver* rx_;
    std::vector<cf32> raw_;              // the radio samples not yet consumed
    long rawStart_ = 0;                  // stream index of raw_[0]
    long consumed_ = 0;
    // locked state
    bool locked_ = false;
    Bootstrap bs_;
    double cfo_ = 0;
    long frameStart_ = 0;                // stream index of the first sample after the bootstrap
    long frameSamples_ = 0;              // length of the frame in radio samples, once the Preamble has been read
    long bsStart_ = 0;                   // stream index of the bootstrap start
    double minToNextSec_ = 0;
    long searchFrom_ = 0;
    SyncStats st_;
    int missed_ = 0;
    int hintMisses_ = 0;
    bool haveHint_ = false;              // a previous lock gives the expected place and offset of the next bootstrap
};

} // namespace atsc3
} // namespace dect2
