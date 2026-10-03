// Channel bandwidth detection from the spectrum: an OFDM signal has a flat top with sharp edges, so the width of that plateau
// tells which channel raster (1.7, 5, 6, 7 or 8 MHz) is in use.
#pragma once
#include <vector>

namespace dect2 {

struct BandwidthEstimate {
    bool valid = false;
    double occupiedMhz = 0;   // measured width of the flat top
    double bwMhz = 0;         // nearest channel bandwidth
    double snrDb = 0;         // plateau over the noise floor
    bool noFloor = false;     // no quiet part in the spectrum (no signal)
};

double snapBandwidth(double occupiedMhz);   // occupied width -> channel bandwidth

// Averages spectrum frames and measures the plateau
class BandwidthDetector {
public:
    void reset() { sum_.clear(); frames_ = 0; }
    void add(const std::vector<float>& dbfs, double fsMhz);   // DC-centred bins, dBFS
    int frames() const { return frames_; }
    BandwidthEstimate estimate() const;
private:
    std::vector<double> sum_;   // linear power
    int frames_ = 0;
    double fsMhz_ = 0;
};

} // namespace dect2
