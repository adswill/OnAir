// DAB census: every ensemble of Band III inside one wide capture, at once: channel, ensemble, services, TII and quality, with the same service heard on two ensembles shown once
// (at its best one). One receiver per channel holding a signal, fast information channel only (no audio): the census is what you pick the channel to listen to from.
#pragma once
#include "dect2/common/ring.h"
#include <memory>
#include <string>
#include <vector>

namespace dect2 {

struct DabChannelPlan { const char* name; double mhz; };
const std::vector<DabChannelPlan>& dabBandIII();

struct DabCensusService { uint32_t sid = 0; std::string label; bool dabPlus = false; int bitrate = 0; };
struct DabCensusRow {
    std::string channel; double mhz = 0;
    float levelDb = -200, aboveDb = 0;
    float snrDb = 0;
    bool active = false, locked = false;
    uint16_t eid = 0;
    std::string ensemble;
    std::vector<DabCensusService> services;
    std::vector<std::pair<int, int>> tii;       // main id, sub id of the transmitters heard, strongest first
    double lastSeenSec = 0;
};
struct DabCensusMerged {                         // one row per service id, across the ensembles
    uint32_t sid = 0; std::string label;
    bool dabPlus = false; int bitrate = 0;
    std::string channel; double mhz = 0;         // the ensemble with the best SNR
    float snrDb = 0;
    std::vector<std::string> alsoOn;             // the other channels carrying it
};
std::vector<DabCensusMerged> dabCensusMerge(const std::vector<DabCensusRow>& rows);

struct DabCensusConfig { int maxEnsembles = 6; double onDb = 6; };

class DabCensus {
public:
    DabCensus();
    ~DabCensus();
    bool configure(double rate, double centreHz, const DabCensusConfig& c = {});   // false: no Band III channel fits, or the capture is narrower than 3 MHz
    bool ready() const;
    void reset();
    void feed(const cf32* x, size_t n);
    std::vector<DabCensusRow> rows() const;
    int ensemblesDecoding() const;
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

} // namespace dect2
