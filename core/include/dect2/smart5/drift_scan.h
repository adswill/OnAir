// Sun drift-scan pointing calibration (3.9, docs/SMART5-science-3.9.md item 24). Point the dish just ahead of the Sun and leave
// it: the Sun drifts through the beam and the total power traces the beam. A Gaussian on a sloping baseline is fitted to that
// trace, giving
//   - the half-power beamwidth (the drift rate is 15.04 deg/h x cos(declination); the Sun's disk, 0.53 deg, is taken out as a
//     Gaussian of 0.31 deg FWHM, its second-moment equivalent),
//   - the pointing error along the Sun's path (when the peak came against when the Sun was closest to where the dish was said to
//     point), as azimuth and elevation corrections,
//   - the Sun-to-cold-sky ratio (Y factor), a quick system check.
// The error across the Sun's path cannot be seen in one scan (it only lowers the peak): repeat the scan at another hour, or give
// the expected Y factor and it is estimated from the peak's shortfall.
#pragma once
#include <string>
#include <vector>

namespace dect2 {
namespace smart5 {

struct DriftScanResult {
    bool valid = false;
    double peakTime = 0;          // unix seconds
    double fwhmSec = 0, beamDeg = 0;
    double alongErrDeg = 0;       // positive: the beam points west of (later than) the given position along the Sun's path
    double azCorrDeg = 0, elCorrDeg = 0;   // add to the dish's stated pointing
    double yFactorDb = 0;
    double crossErrDeg = -1;      // only when an expected Y factor is given
};

// t: unix seconds, p: linear power (any unit); the dish was said to point at (dishAz, dishEl) from (lat, lon)
DriftScanResult driftScan(const std::vector<double>& t, const std::vector<double>& p, double latDeg, double lonDeg,
                          double dishAzDeg, double dishElDeg, double expectedYdb = 0);
std::string driftScanText(const DriftScanResult& r);

} // namespace smart5
} // namespace dect2
