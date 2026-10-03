// One number for "how good is this reception": decoder SNR margin over what the current modulation/code rate needs,
// weighted by the share of FEC blocks that actually decode.
#pragma once
#include "t2rx.h"
#include <deque>
#include <string>
#include <cstdint>
#include <utility>

namespace dect2 {

// Approximate AWGN carrier-to-noise ratio (dB) a PLP needs for quasi-error-free reception (EN 302 755 annex C, 64k FEC).
double requiredSnrDb(const PlpFec& f);

struct QualityReport {
    bool valid = false;
    double percent = 0;     // 0 (red) .. 100 (green)
    double snrDb = 0;       // data SNR from the pilots
    double requiredDb = 0;
    double marginDb = 0;
    double fecOk = 0;       // share of FEC blocks decoded over the last frames (0..1)
    std::string label;      // "excellent" / "good" / "marginal" / "poor" / "no lock"
};

class QualityMeter {
public:
    void reset();
    void update(const RxTelemetry& rx);
    const QualityReport& report() const { return rep_; }

private:
    QualityReport rep_;
    uint64_t lastFrames_ = ~0ull, lastOk_ = 0, lastBad_ = 0;
    std::deque<std::pair<uint64_t, uint64_t>> hist_; // (ok, bad) per frame
    double smooth_ = 0;
};

} // namespace dect2
