// What the app and the command line need to know to tune a mode that is not one of the original families:
// the range, a default, and the radio settings. Each mode describes itself in its own <mode>_rx.cpp (xxxTuning()).
#pragma once
#include <string>

namespace dect2 {

struct ModeTuning {
    int stdMode = 0;             // the engine standard code (Engine::setStandard): 8 DVB-S/S2, 9 DTMB, 10 analog TV, 11 DMR, 12 DRM, 13 ADS-B, 14 GNSS,
                                 // 15 radiosonde, 16 AIS, 17 marine, 18 ACARS, 19 Inmarsat-C, 20 Inmarsat Aero, 21 Iridium, 22 mesh,
                                 // 23 HD Radio, 24 CDR, 25 pagers, 26 APRS / packet, 27 HF digital, 28 airband
    const char* id = "";         // short name: dect2cli --standard <id>, and the prefix of the mode's files
    const char* name = "";       // shown in the app
    double minMhz = 1, maxMhz = 6000, defMhz = 100;   // tuning range, and the frequency a fresh start uses
    double sampleRate = 2e6;     // the radio sample rate the app asks for (a radio that cannot reach it runs as fast as it can)
    double basebandHz = 0;       // the radio's analogue baseband filter (0 = automatic)
    double bandwidthMhz = 1;     // channel width handed to the engine (TuneSettings::bandwidthMhz)
    double minSampleRate = 2e6;  // below this the receiver reports that it cannot work
    double tuneOffsetHz = 0;     // the radio is tuned this far above the user's frequency so a narrow channel avoids the DC spike; the receiver mixes it out (setSignalOffset(-tuneOffsetHz))
    // What the engine skips of "Remove DC spike" and "IQ correction" (iq_correct.h) for this mode, because it would harm it (tests/test_iq_modes.cpp):
    bool carrierAtCentre = false;   // the wanted signal can have a carrier on the radio's centre, which the DC removal would take with the spike
    bool notCircular = false;       // the wanted signal is not circular around the centre, which the blind IQ estimate would take for an imbalance
};

// Registry (modes.cpp): nullptr when the code or the id is not one of these modes
const ModeTuning* modeTuning(int stdMode);
const ModeTuning* modeTuningById(const std::string& id);

// The slowest radio sample rate the receiver of an engine standard (0 auto DVB-T2/T, 1 DVB-T2, 2 DVB-T, 3 ATSC, 4 DAB, 5 ATSC 3.0, 6 ISDB-T,
// 7 FM, 8 and up the modes above) works with; bandwidthMhz is the DVB-T2 / DVB-T channel width. 0 = no limit known. The engine logs, the
// scanner and the app's rate warning all use it, so they say the same.
double minSampleRateFor(int stdMode, double bandwidthMhz);

} // namespace dect2
