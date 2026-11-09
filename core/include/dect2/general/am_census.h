// AM census: every carrier of a wide capture of a medium-wave or short-wave band from one spectrum: which raster channel holds what (AM, DRM, SSB,
// a bare carrier), how strong, how it fades, which sideband is hit by interference. Analysis only (no demodulator per carrier); one FFT per frame for the whole span,
// so the cost does not grow with the number of carriers. Ranked with the listenable ones first: the app offers one-click listen on the best.
//
// Fade tracking: the carrier level of each channel is sampled about 4 times per second. Fade depth is the 10th-90th percentile spread over the last
// half minute; selective fading is when the sidebands do not fade with the carrier.
#pragma once
#include "dect2/common/ring.h"
#include <memory>
#include <string>
#include <vector>

namespace dect2 {

enum class AmKind { Am, Drm, Ssb, Carrier };
const char* amKindName(AmKind k);

struct AmCensusRow {
    double khz = 0;              // on the raster
    AmKind kind = AmKind::Am;
    float carrierDb = -200;      // line power (dBFS)
    float snrDb = 0;             // carrier to noise floor
    float fadeDb = 0;            // 10th-90th percentile spread of the carrier over the last half minute (0 until enough history)
    bool selective = false;      // selective fading: the sidebands do not fade with the carrier
    bool fading = false;         // fadeDb above 6 dB
    int ecss = 0;                // +1 listen to the upper sideband, -1 to the lower (the other carries interference), 0 no advice
    float score = 0;             // ranking: higher is easier to listen to
    double lastSec = 0;          // seconds of capture when last seen
};

struct AmCensusConfig {
    double stepHz = 0;           // the raster: 0 auto (9 or 10 kHz medium wave by which fits, 5 kHz above 2 MHz)
    double onDb = 10;            // a channel counts above this over the noise floor
};

class AmCensus {
public:
    AmCensus();
    ~AmCensus();
    bool configure(double rate, double centreHz, const AmCensusConfig& c = {});   // at least 500 kHz
    void reset();
    void feed(const cf32* x, size_t n);
    std::vector<AmCensusRow> rows() const;    // ranked, best first (any thread)
    double stepHz() const;                    // the raster in use
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

} // namespace dect2
