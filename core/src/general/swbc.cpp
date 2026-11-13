// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/general/swbc.h"

namespace dect2 {

// ITU Radio Regulations Art. 5 broadcasting allocations in the HF range, checked against the ITU-R terrestrial broadcast bands page
// (itu.int/en/ITU-R/terrestrial/broadcast/Pages/bands.aspx), Wikipedia "Shortwave bands" and hqshortwaveradio.com/bands.
// startKhz / endKhz is the widest edge over the three regions; reg[] holds each ITU region's own edges ({0, 0}: no allocation).
static const SwBand kBands[] = {
    // ITU-R bands page 2300-2495 A4, 2495-2498 R1 A4; Wikipedia Shortwave bands 2.3-2.495
    {"120 m", 2300, 2498, true, {{2300, 2498}, {2300, 2495}, {2300, 2495}}, "tropical zone only; R1 extends to 2498 kHz (2495-2498 R1 only)"},
    // ITU-R bands page 3200-3400 A4; Wikipedia 3.2-3.4; hqshortwaveradio 3.2-3.4
    {"90 m", 3200, 3400, true, {{3200, 3400}, {3200, 3400}, {3200, 3400}}, "tropical zone only"},
    // ITU-R bands page 3900-3950 R3, 3950-4000 R1+R3; Wikipedia 3.9-4.0
    {"75 m", 3900, 4000, false, {{3950, 4000}, {0, 0}, {3900, 4000}}, "Region 1 3950-4000 only; Region 2 none; Region 3 3900-4000"},
    // ITU-R bands page 4750-4995 and 5005-5060 A4; Wikipedia 4.75-5.06
    {"60 m", 4750, 5060, true, {{4750, 5060}, {4750, 5060}, {4750, 5060}}, "tropical zone only; 4750-4995 and 5005-5060 kHz (5000-5005 is the standard-frequency service)"},
    // ITU-R bands page 5900-6200; Wikipedia 5.9-6.2
    {"49 m", 5900, 6200, false, {{5900, 6200}, {5900, 6200}, {5900, 6200}}, ""},
    // ITU-R bands page 7200-7300 R1+R3, 7300-7400 all, 7400-7450 R1+R3
    {"41 m", 7200, 7450, false, {{7200, 7450}, {7300, 7400}, {7200, 7450}}, "Regions 1 and 3 7200-7450; Region 2 7300-7400 (7200-7300 is amateur)"},
    // ITU-R bands page 9400-9900
    {"31 m", 9400, 9900, false, {{9400, 9900}, {9400, 9900}, {9400, 9900}}, "the busiest band"},
    // ITU-R bands page 11600-12100 (Wikipedia gives 12.2)
    {"25 m", 11600, 12100, false, {{11600, 12100}, {11600, 12100}, {11600, 12100}}, ""},
    // ITU-R bands page 13570-13870
    {"22 m", 13570, 13870, false, {{13570, 13870}, {13570, 13870}, {13570, 13870}}, ""},
    // ITU-R bands page 15100-15800
    {"19 m", 15100, 15800, false, {{15100, 15800}, {15100, 15800}, {15100, 15800}}, ""},
    // ITU-R bands page 17480-17900
    {"16 m", 17480, 17900, false, {{17480, 17900}, {17480, 17900}, {17480, 17900}}, ""},
    // ITU-R bands page 18900-19020
    {"15 m", 18900, 19020, false, {{18900, 19020}, {18900, 19020}, {18900, 19020}}, "narrow band, digital (DRM) trials"},
    // ITU-R bands page 21450-21850
    {"13 m", 21450, 21850, false, {{21450, 21850}, {21450, 21850}, {21450, 21850}}, ""},
    // ITU-R bands page 25670-26100 (Wikipedia says 25.6 start)
    {"11 m", 25670, 26100, false, {{25670, 26100}, {25670, 26100}, {25670, 26100}}, "little used; best around the sunspot maximum"},
};

const SwBand* swBroadcastBands(size_t& count) { count = sizeof kBands / sizeof *kBands; return kBands; }
double swBandCentreKhz(const SwBand& b) { return 0.5 * (b.startKhz + b.endKhz); }
bool swBandInRegion(const SwBand& b, int region, double& startKhz, double& endKhz) {
    if (region < 1 || region > 3) region = 1;
    const SwRange& r = b.reg[region - 1];
    startKhz = r.startKhz; endKhz = r.endKhz;
    return r.endKhz > r.startKhz;
}
double swBandCentreKhz(const SwBand& b, int region) {
    double s, e;
    return swBandInRegion(b, region, s, e) ? 0.5 * (s + e) : swBandCentreKhz(b);
}
const SwBand* swBandAt(double khz) {
    for (const SwBand& b : kBands) if (khz >= b.startKhz && khz <= b.endKhz) return &b;
    return nullptr;
}

}  // namespace dect2
