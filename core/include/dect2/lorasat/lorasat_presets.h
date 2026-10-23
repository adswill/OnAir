// LoRa satellite presets: public frequencies and LoRa settings of satellites and space-gateway services that use LoRa (issue #229). Public knowledge only,
// written from what is published by the operators and by the community ground-station networks (TinyGS, SatNOGS DB, the satellites' own documentation).
// NOTHING HERE WAS CHECKED AGAINST A REAL SIGNAL by the authors. Satellites change frequency, retire and re-launch; every entry carries how sure we are
// and the list is only a starting point (see docs/LORA_SATELLITES.md). A frequency of your own can always be typed in.
#pragma once
#include <string>
#include <vector>

namespace dect2 {
namespace lorasat {

enum Confidence { Uncertain = 0, Plausible = 1, Documented = 2 };

struct Preset {
    const char* name;
    double mhz;                 // the nominal downlink (or, for uplink entries, the uplink) frequency
    int sf;                     // 0: unknown, the SFs are searched
    int bwKhz;                  // 41 (41.7), 62 (62.5), 125, 250, 500 (bandwidthHz() gives the exact value)
    int cr;                     // 5..8 for 4/5..4/8, 0 unknown (an explicit header carries it anyway)
    int sync;                   // sync word, -1: try 0x12 and 0x34
    bool uplinkOnly;            // ground devices transmit to the satellite: you hear the devices, not the satellite
    Confidence conf;
    const char* tleName;        // start of the satellite's name in the elements files ("" none): the pass planner offers it
    const char* note;
};

const std::vector<Preset>& presets();
inline double bandwidthHz(int bwKhz) { return bwKhz == 41 ? 125e3 / 3 : bwKhz == 62 ? 62500.0 : bwKhz * 1e3; }
// The preset whose frequency is within tolKhz of mhz, or -1
int nearest(double mhz, double tolKhz = 15.0);
const char* confidenceName(Confidence c);   // "documented", "plausible", "uncertain"

// What a pass does to the signal at a frequency: the largest Doppler shift and the largest Doppler rate for a satellite at hKm altitude (circular orbit),
// seen from the ground at 0 degrees elevation (shift) and straight overhead (rate). Used for the filter width and the notes in the UI.
double maxDopplerHz(double mhz, double altitudeKm = 600.0);
double maxDopplerRateHzS(double mhz, double altitudeKm = 600.0);

} // namespace lorasat
} // namespace dect2
