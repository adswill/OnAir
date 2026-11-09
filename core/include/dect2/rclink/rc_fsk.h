// Shared front end of the frequency-hopping FSK links (SiK, FrSky, FlySky): bursts are found in a short-time spectrum of the whole capture, so every hop
// channel inside the band is covered without a channel filter bank, and each burst is demodulated afterwards from the stored samples.
#pragma once
#include "dect2/common/ring.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace dect2 {
namespace rclink {

struct Burst {
    int64_t start = 0, end = 0;  // sample numbers of the input, pre- and post-roll included
    double freqHz = 0;           // centre of the burst's energy, relative to the middle of the capture
    double bwHz = 0;             // width of the bins it lit up
    float snrDb = 0;
};

class BurstFinder {
public:
    void configure(double rate, double thresholdDb = 9, double maxSec = 0.08);
    void reset();
    // appends samples and reports the bursts that ended; `samples(start)` of the callback stays valid during the call
    void feed(const cf32* x, size_t n, const std::function<void(const Burst&)>& onBurst);
    void flush(const std::function<void(const Burst&)>& onBurst);   // closes what is open (tests, end of a file)
    // the input samples [b.start, b.end) that are still stored
    const cf32* at(int64_t i) const { return hist_.data() + (i - base_); }
    int64_t begin() const { return base_; }
    int64_t end() const { return base_ + (int64_t)hist_.size(); }
    double rate() const { return fs_; }
private:
    struct Open { int lo, hi, lastFrame; int64_t startFrame; double e, ef, peak; int frames; double width; };   // lo..hi: the bins of the last frame
    void frame(const cf32* w, const std::function<void(const Burst&)>& cb);
    void close(const Open& o, const std::function<void(const Burst&)>& cb);
    double fs_ = 0, thr_ = 8, maxSec_ = 0.08;
    int N_ = 256, hop_ = 128;
    int64_t frames_ = 0, base_ = 0, next_ = 0;
    std::vector<cf32> hist_;
    std::vector<float> win_, floor_, power_, smooth_;
    std::vector<cf32> fft_;
    std::vector<Open> open_;
    bool floorSet_ = false;
};

// Mixes [start, end) of the capture to 0 Hz, filters it and returns the phase steps at `outRate` or a little above it. actualRate gets the rate of the result.
std::vector<float> discriminate(const BurstFinder& f, const Burst& b, double cutoffHz, double outRate, double* actualRate);

// Finds a known bit pattern (preamble tail + sync word) in a discriminator stream and reads the bytes behind it.
// msbFirst: bytes are shifted out most significant bit first (false: least significant first).
struct SyncHit {
    bool found = false;
    int errors = 0;
    double position = 0;         // sample of the first bit after the pattern
    double offset = 0;           // discriminator value of the middle (the carrier offset), radians per sample
};
SyncHit findSync(const std::vector<float>& d, double sps, const std::vector<uint8_t>& pattern, int maxErrors, bool invertedOk = false);
std::vector<uint8_t> sliceBytes(const std::vector<float>& d, double sps, const SyncHit& hit, size_t nBytes, bool msbFirst);

std::vector<uint8_t> bitsOfBytes(const std::vector<uint8_t>& bytes, bool msbFirst = true);   // for sync patterns

} // namespace rclink
} // namespace dect2
