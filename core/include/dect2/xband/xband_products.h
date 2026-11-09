// Products made from the channels of an X-band pass (xband_tel.h): colour composites and, where the instrument carries its own calibration, brightness temperature, sea surface and fire maps.
// RGBA 8 bit, top to bottom. AVHRR is calibrated from its on-board black body and space views (NOAA KLM method, coefficients from SatDump's resources). MODIS, VIIRS, METimage etc. are
// not calibrated here, so their band differences are relative contrast only ("uncalibrated").
#pragma once
#include "dect2/xband/xband_proto.h"
#include <cstdint>
#include <string>
#include <vector>

namespace dect2::xband {

struct XbProduct {
    std::string name;
    int width = 0, height = 0;
    std::vector<uint32_t> rgba;
    std::string note;
};

// AVHRR line calibration: on-board targets of each line
struct AvhrrCal {
    double satIndex = 0;      // 0 MetOp-B, 1 MetOp-C
    struct Line { float prt[4] = {0, 0, 0, 0}; int nPrt = 0; float cs[3] = {0, 0, 0}, cb[3] = {0, 0, 0}; bool is3b = false; };
    std::vector<Line> lines;
};
// Channel 3b (index 0), 4 (1) or 5 (2) counts of a line to brightness temperature in kelvin, 0 when the targets are missing
double avhrrTemperature(const AvhrrCal& cal, int y, int ch, int count10);
double avhrrPlanckRadiance(double t, double vc);

// All products the channels of this profile allow; maxWidth: reduced for the screen (0: full size).
std::vector<XbProduct> makeProducts(const Store& s, int profile, const AvhrrCal* cal, int maxWidth);
// The names of the products a profile can make, without computing them (for the screen)
std::vector<std::string> productNames(int profile);

} // namespace dect2::xband
