// OFDM interferer locator (3.9, docs/SMART5-tv-3.9.md item 12). The per-carrier SNR (or MER) an OFDM decoder already measures
// shows where in the channel the damage is. Runs of carriers well below the channel's median are grouped and named:
//   narrow (a few carriers)                 -> a CW spur or carrier (a clock harmonic, an analogue carrier)
//   wide, matching an LTE / NR block width  -> LTE / 5G (occupied 1.08, 2.7, 4.5, 9, 13.5, 18 MHz; NR 5 - 20 MHz)
//   touching a channel edge                 -> adjacent-channel spill
//   otherwise                               -> wideband interference
// Works for DVB-T/T2, ISDB-T, DTMB (multicarrier) and DAB: give the carrier spacing and the RF of carrier 0.
#pragma once
#include <string>
#include <vector>

namespace dect2 {
namespace smart5 {

enum class InterfKind { CwSpur, LteBlock, AdjacentChannel, Wideband };

struct Interferer {
    InterfKind kind = InterfKind::Wideband;
    int firstCarrier = 0, lastCarrier = 0;
    double centreHz = 0;     // RF
    double widthHz = 0;
    double depthDb = 0;      // median SNR minus the run's mean SNR
};

struct OfdmInterfParams { double dropDb = 6; int maxGap = 2; int cwMaxCarriers = 5; double edgeFrac = 0.02; };

// snrDb[k] per active carrier in frequency order; carrier 0 is at rfCarrier0Hz, spacing spacingHz.
std::vector<Interferer> locateInterferers(const std::vector<float>& snrDb, double rfCarrier0Hz, double spacingHz, OfdmInterfParams p = {});
const char* interfName(InterfKind k);
std::string interfText(const Interferer& i);

} // namespace smart5
} // namespace dect2
