// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Which decoder runs for each mode, and the tuning of each mode.
#include "avroad_int.h"

namespace dect2 {

std::unique_ptr<avroad::Decoder> avroad::makeDecoder(int v) {
    switch (v) {
    case EotHot: return makeEot();
    case Fanet: return makeFanet();
    case Ogn: return makeOgn();
    case Ermes: return makeErmes();
    case MarineRadar: return makeRadar(false);
    case WeatherRadar: return makeRadar(true);
    case Ldacs: return makeOfdmDetect(0);
    case Aeromacs: return makeOfdmDetect(1);
    case Cv2x: return makeOfdmDetect(2);
    case ItsG5: return makeIts();
    case Dsrc: return makeDsrc();
    case Gbas: return makeGbas();
    default: return nullptr;
    }
}

ModeTuning avRoadTuning(int v) {
    using namespace avroad;
    ModeTuning t;
    t.stdMode = avroad::kFamilyBase + 2 + v;
    struct T { const char* id; double minMhz, maxMhz, defMhz, rate, minRate, bwMhz; };
    static const T k[kNumVariants] = {
        {"fanet", 860, 930, 868.2, 1000000, 500000, 0.25},
        {"ogn", 860, 930, 868.3, 1000000, 1000000, 0.6},
        {"gbas", 108, 118, 113.0, 250000, 62500, 0.025},
        {"ldacs", 960, 1215, 1000.0, 2000000, 625000, 0.5},
        {"aeromacs", 5000, 5150, 5095.0, 5600000, 5600000, 5.0},
        {"eot", 150, 520, 457.9375, 1000000, 48000, 0.0125},
        {"dsrc", 5700, 5900, 5802.5, 4000000, 2000000, 1.0},
        {"its", 5800, 6000, 5900.0, 10000000, 10000000, 10.0},
        {"cv2x", 5800, 6000, 5915.0, 15360000, 15360000, 10.0},
        {"ermes", 169, 170, 169.65, 1000000, 48000, 0.025},
        {"mradar", 2900, 9600, 3050.0, 10000000, 1000000, 1.0},
        {"wradar", 2700, 9600, 5625.0, 10000000, 1000000, 1.0},
    };
    if (v < 0 || v >= kNumVariants) return t;
    const T& e = k[v];
    t.id = e.id; t.name = variantName(v);
    t.minMhz = e.minMhz; t.maxMhz = e.maxMhz; t.defMhz = e.defMhz;
    t.sampleRate = e.rate; t.minSampleRate = e.minRate; t.bandwidthMhz = e.bwMhz;
    t.basebandHz = 0; t.tuneOffsetHz = 0;
    return t;
}

} // namespace dect2
