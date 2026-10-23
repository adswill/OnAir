// LoRa satellite presets (see lorasat_presets.h). Read the warning there: unverified, from public documentation and community lists.
#include "dect2/lorasat/lorasat_presets.h"
#include <algorithm>
#include <cmath>

namespace dect2 {
namespace lorasat {

const std::vector<Preset>& presets() {
    // name, MHz, SF, BW kHz, CR, sync, uplink only, confidence, TLE name, note
    static const std::vector<Preset> v = {
        {"FOSSA Systems pico-satellites (FOSSASAT-1B / FOSSASAT-2 family)", 436.8, 11, 125, 8, 0x12, false, Uncertain, "FOSSASAT",
         "LoRa beacons and telemetry of the FOSSA Systems 'PocketQube / 1P' satellites on 70 cm. The SF11 / 125 kHz / 4/8 / sync 0x12 setting is what the FOSSASAT-1 documentation and "
         "the TinyGS list gave when we last read them; later FOSSA satellites moved to other frequencies in 436 MHz and some use FSK or GMSK as well. Check the satellite's page."},
        {"FOSSASAT-2 family (second frequency)", 436.2, 11, 125, 8, 0x12, false, Uncertain, "FOSSASAT-2",
         "Second frequency seen in the community lists for the FOSSASAT-2 series. Uncertain."},
        {"Tianqi constellation (Guodian Gaoke) downlink", 400.45, 0, 500, 0, -1, false, Uncertain, "TIANQI",
         "The Tianqi (TQ) satellites run an IoT service with LoRa-based modulation around 400.45 MHz; the community networks list 500 kHz wide LoRa there with varying spreading factors. "
         "Spreading factor and sync word are searched. Uncertain: the service is commercial and the signal plan is not published in full."},
        {"Lacuna Space - uplink channel 868.1 (EU868)", 868.1, 0, 125, 5, 0x34, true, Plausible, "LACUNA",
         "Lacuna Space's satellites listen to ordinary LoRa / LoRaWAN-style uplinks from ground devices on the EU868 channels (868.1, 868.3, 868.5 MHz, SF7 - SF12). "
         "You hear the devices, not the satellite (a satellite's downlink is on another band). Plausible: from the operator's public descriptions."},
        {"Lacuna Space - uplink channel 868.3 (EU868)", 868.3, 0, 125, 5, 0x34, true, Plausible, "LACUNA", "As above, second default EU868 channel."},
        {"Lacuna Space - uplink channel 868.5 (EU868)", 868.5, 0, 125, 5, 0x34, true, Plausible, "LACUNA", "As above, third default EU868 channel."},
        {"Space LoRaWAN uplinks, US915 sub-band 2 (903.9 - 905.3)", 904.6, 0, 125, 5, 0x34, true, Uncertain, "",
         "Ground devices of satellite LoRa services in North America use the US915 uplink channels; sub-band 2 is the common one. Uncertain which services listen there."},
        {"FOSSASAT (401.703 MHz, settings from issue #229)", 401.703, 11, 125, 8, -1, false, Plausible, "FOSSASAT",
         "SF11, 125 kHz, CR 4/8 as a user posted them from the TinyGS settings (issue #229); the sync word is searched (0x12 and 0x34). Not checked against a signal."},
        {"137.055 MHz LoRa telemetry, SF8 / 41.7 kHz (issue #229)", 137.055, 8, 41, 5, 0x12, false, Plausible, "",
         "SF8, 41.7 kHz, CR 4/5, sync 0x12, 15-symbol preamble, CRC on, as a user posted them from the TinyGS settings (issue #229). Not checked against a signal."},
        {"Generic 70 cm LoRa cubesat, SF10 / 125 kHz", 437.4, 10, 125, 0, 0x12, false, Uncertain, "",
         "A guess for experimenters: many amateur LoRa beacons sit between 435 and 438 MHz at 125 kHz and SF9 - SF12. Use it as a search start, not as a fact."},
    };
    return v;
}

int nearest(double mhz, double tolKhz) {
    const auto& v = presets();
    int best = -1;
    double bd = tolKhz / 1e3;
    for (size_t i = 0; i < v.size(); i++) {
        const double d = std::fabs(v[i].mhz - mhz);
        if (d <= bd) { bd = d; best = (int)i; }
    }
    return best;
}

const char* confidenceName(Confidence c) { return c == Documented ? "documented" : c == Plausible ? "plausible" : "uncertain"; }

namespace {
constexpr double kGm = 3.986004418e14, kRe = 6371e3, kC = 299792458.0;
}

double maxDopplerHz(double mhz, double altKm) {
    const double r = kRe + altKm * 1e3, v = std::sqrt(kGm / r);
    return mhz * 1e6 * v * (kRe / r) / kC;     // radial speed at the horizon: v R / (R + h)
}

double maxDopplerRateHzS(double mhz, double altKm) {
    const double r = kRe + altKm * 1e3, v = std::sqrt(kGm / r);
    return mhz * 1e6 / kC * v * v / (altKm * 1e3) * (kRe / r);   // straight overhead, flat approximation reduced for the curvature: an upper bound
}

} // namespace lorasat
} // namespace dect2
