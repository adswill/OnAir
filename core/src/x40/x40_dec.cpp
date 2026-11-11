// The decoder factory and the tuning of each 4.0 signal mode.
#include "x40_int.h"

namespace dect2 {

std::unique_ptr<x40::Decoder> x40::makeDecoder(int v) {
    switch (v) {
    case RcPpm: return makeRcPpm();
    case Uvb76: return makeUvb76();
    case Graves: return makeGraves();
    case Jove: return makeJove();
    case Beacon: return makeBeacon();
    case Em4100: return makeEm4100();
    default: return nullptr;
    }
}

ModeTuning x40Tuning(int v) {
    using namespace x40;
    ModeTuning t;
    t.stdMode = kFamilyBase + 3 + v;
    struct T { const char* id; double minMhz, maxMhz, defMhz, rate, minRate, bwMhz; bool carrier; };
    static const T k[kNumVariants] = {
        {"rcppm", 26, 75, 72.350, 1000000, 200000, 0.04, false},
        {"uvb76", 4, 5, 4.625, 1000000, 24000, 0.012, true},
        {"graves", 143, 143.1, 143.050, 250000, 8000, 0.004, true},
        {"jove", 19, 22, 20.100, 1000000, 24000, 0.012, false},
        {"beacon", 28, 450, 144.400, 250000, 8000, 0.004, true},
        {"em4100", 0.1, 0.15, 0.125, 250000, 24000, 0.01, true},
    };
    if (v < 0 || v >= kNumVariants) return t;
    const T& e = k[v];
    t.id = e.id; t.name = variantName(v);
    t.minMhz = e.minMhz; t.maxMhz = e.maxMhz; t.defMhz = e.defMhz;
    t.sampleRate = e.rate; t.minSampleRate = e.minRate; t.bandwidthMhz = e.bwMhz;
    t.basebandHz = 0; t.tuneOffsetHz = 0; t.carrierAtCentre = e.carrier;
    return t;
}

} // namespace dect2
