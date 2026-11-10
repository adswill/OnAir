// HD Radio digital sideband level (3.9, docs/SMART5-broadcast-3.9.md item 9). From a power spectrum of the FM channel (centred on
// the carrier, at least +/- 200 kHz), the power of the lower and upper primary main sidebands (129.4 - 198.4 kHz from the
// carrier, NRSC-5) against the power of the analogue FM signal (within +/- 100 kHz), in dBc. The total digital power against the
// analogue is what the FCC steps refer to (-20 dBc standard, -14 and -10 dBc allowed since 2010, and asymmetric sidebands).
// The noise floor (from 205 - 240 kHz on each side when the spectrum reaches there) is taken off first.
#pragma once
#include <string>
#include <vector>

namespace dect2 {
namespace smart5 {

struct HdSidebandReport {
    bool valid = false;
    double lowerDbc = -99, upperDbc = -99, totalDbc = -99;
    double imbalanceDb = 0;    // upper minus lower
    int stepDbc = 0;           // the nearest of -20, -14, -10 (0 when the total is below -26 dBc: no digital signal)
    bool asymmetric = false;   // |imbalance| > 3 dB
    std::string summary() const;
};

// powerLin: linear power per bin; bin k is at startHz + k * binHz from the carrier
HdSidebandReport hdSidebandLevels(const std::vector<double>& powerLin, double startHz, double binHz);
// the same from a dB spectrum
HdSidebandReport hdSidebandLevelsDb(const std::vector<float>& powerDb, double startHz, double binHz);

} // namespace smart5
} // namespace dect2
