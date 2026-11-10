// HD Radio blend alignment meter (3.9, docs/SMART5-broadcast-3.9.md item 8). A hybrid station sends the same programme on the
// analogue FM carrier and digitally; the station delays the analogue audio so that a radio blending between the two hears no
// jump. When the station's diversity delay is wrong the listener hears an echo or a skip at every blend. This cross-correlates
// the two programme audios (generalised cross-correlation with phase transform, sub-sample peak) and reports the offset in
// milliseconds, the level difference in dB and how sure the peak is. push() both audios at the same rate, any block sizes.
#pragma once
#include <cstddef>
#include <deque>
#include <string>
#include <vector>

namespace dect2 {
namespace smart5 {

struct BlendReport {
    bool valid = false;
    double offsetMs = 0;       // positive: the digital audio comes later than the analogue
    double levelDb = 0;        // digital minus analogue loudness
    double confidence = 0;     // peak against the correlation's spread, 0..1
    bool misaligned = false;   // |offset| above the tolerance with a confident peak
    bool levelMismatch = false;
    std::string summary() const;
};

struct BlendParams { double workRate = 4000; double windowSec = 6; double maxLagSec = 1.5; double tolMs = 20; double levelTolDb = 3; double minConfidence = 0.25; };

class BlendAlign {
public:
    void configure(double audioRate, BlendParams p = {});
    void pushAnalog(const float* x, size_t n) { feed(x, n, a_, accA_, nA_); }
    void pushDigital(const float* x, size_t n) { feed(x, n, d_, accD_, nD_); }
    // runs the correlation when a window is ready on both; true when the report was refreshed
    bool analyse();
    const BlendReport& report() const { return r_; }
private:
    void feed(const float* x, size_t n, std::deque<float>& q, double& acc, int& k);
    BlendParams p_;
    int D_ = 1; double rate_ = 0;
    std::deque<float> a_, d_;
    double accA_ = 0, accD_ = 0; int nA_ = 0, nD_ = 0;
    size_t need_ = 0;
    BlendReport r_;
    std::vector<double> lags_;   // recent offsets, for a steady reading
};

} // namespace smart5
} // namespace dect2
