// What the app and the command line need to know to tune a mode that is not one of the original families:
// the range, a default, and the radio settings. Each mode describes itself in its own <mode>_rx.cpp (xxxTuning()).
#pragma once
#include <string>

namespace dect2 {

struct ModeTuning {
    int stdMode = 0;             // the engine standard code (Engine::setStandard): 8 DVB-S/S2, 9 DTMB, 10 analog TV, 11 DMR, 12 DRM, 13 ADS-B
    const char* id = "";         // short name: dect2cli --standard <id>, and the prefix of the mode's files
    const char* name = "";       // shown in the app
    double minMhz = 1, maxMhz = 6000, defMhz = 100;   // tuning range, and the frequency a fresh start uses
    double sampleRate = 2e6;     // the radio sample rate the app asks for (a radio that cannot reach it runs as fast as it can)
    double basebandHz = 0;       // the radio's analogue baseband filter (0 = automatic)
    double bandwidthMhz = 1;     // channel width handed to the engine (TuneSettings::bandwidthMhz)
    double minSampleRate = 2e6;  // below this the receiver reports that it cannot work
};

// Registry (modes.cpp): nullptr when the code or the id is not one of these modes
const ModeTuning* modeTuning(int stdMode);
const ModeTuning* modeTuningById(const std::string& id);

} // namespace dect2
