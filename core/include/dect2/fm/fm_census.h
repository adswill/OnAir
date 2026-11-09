// FM census: every station of the band from one wide capture, no tuning.
//
//   wide capture -> one FFT (overlap-save) -> stations found from the averaged spectrum on the 100 kHz raster -> per station the bins around it are
//   turned back into a baseband stream by a small inverse FFT (the shared channeliser: the cost per station does not grow with the capture's rate)
//   -> a FmReceiver in analysis-only mode (pilot, RDS, measurements) -> one row per station: PI, name, text, programme type, level, stereo, TA, AF.
#pragma once
#include "dect2/common/ring.h"
#include <memory>
#include <string>
#include <vector>

namespace dect2 {

struct FmCensusRow {
    double mhz = 0;              // on the 100 kHz raster
    float levelDb = -200;        // channel power, dB full scale
    float aboveDb = 0;           // over the noise floor of the capture
    float snrDb = 0;             // audio SNR measure of the receiver
    bool active = false;         // being decoded now; false: seen earlier in this census (the row stays)
    bool stereo = false, rds = false, tp = false, ta = false, music = false;
    int pi = 0, ecc = -1;
    std::string ps, rt, pty;
    std::vector<double> afMhz;
    double lastSeenSec = 0;      // seconds of capture since the census began
};

struct FmCensusConfig {
    int maxStations = 0;         // decoded at once (0: 8 per CPU core), the strongest; a newcomer must be 6 dB stronger than the weakest to replace it
    double onDb = 10;            // a station stands this far above the noise floor
    int rdsRegion = 0;           // 0 auto, 1 RDS, 2 RBDS (FmReceiver::setRdsRegion)
};

class FmCensus {
public:
    FmCensus();
    ~FmCensus();
    // rate: the capture's sample rate (at least 1 MHz: below that one station fills it); centreHz: its centre frequency
    bool configure(double rate, double centreHz, const FmCensusConfig& cfg = {});
    bool ready() const;
    void reset();                       // forget the stations (after a retune)
    void feed(const cf32* x, size_t n);
    std::vector<FmCensusRow> rows() const;   // sorted by frequency (any thread)
    int stationsDecoding() const;
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

} // namespace dect2
