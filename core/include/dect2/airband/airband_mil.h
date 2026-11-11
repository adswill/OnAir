// The military UHF air band (225.000 - 399.975 MHz, AM, 25 kHz channels: 7000 channels) for the airband mode: the band limits, the raster
// and the few frequencies that are published for common use. Everything else in the band is assigned per country, base and exercise; the
// user adds those channels by hand, from the band watch or from a fast scan ("Military UHF air 225-400" profile).
// Sources: 243.000 MHz the military UHF guard / emergency channel (ICAO Annex 10 Vol V lists 243.0 MHz for survival craft; FAA notices
// tell military aircraft to watch VHF guard 121.5 or UHF 243.0); 282.800 MHz the NATO joint / combined on-scene search and rescue and
// direction finding frequency (US Navy NTTP 3-50.1 and the published SAR frequency lists). Receive only.
#pragma once
#include "dect2/airband/airband_rx.h"
#include <vector>

namespace dect2 {

constexpr double kMilUhfLoHz = 225.000e6, kMilUhfHiHz = 399.975e6, kMilUhfStepHz = 25000.0;

struct AirbandKnownChannel {
    double freqHz;
    const char* label;   // short tag for the channel list ("UHF guard")
    const char* note;    // what it is for
};

const std::vector<AirbandKnownChannel>& airbandMilUhfKnown();
bool airbandIsMilUhf(double freqHz);                       // inside 225.000 - 399.975 MHz
std::vector<AirbandChannel> airbandMilUhfChannels();        // the known channels as a list for the airband mode (25 kHz, labelled)
int airbandMilUhfChannelCount();                           // channels on the 25 kHz raster: 7000

} // namespace dect2
