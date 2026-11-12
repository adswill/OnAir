// ITU-R P.1812-8 (09/2025): a path-specific propagation prediction method for point-to-area terrestrial services, 30 MHz to 6 GHz
// (used here up to 50 GHz like its sister P.452 with the same mechanisms). Basic transmission loss not exceeded for p % of the time
// (1..50 %) and pL % of locations (1..99 %), from a terrain profile with representative clutter heights:
//   free space with multipath / focusing corrections (section 4.2), delta-Bullington diffraction with spherical-earth first-term
//   diffraction for the median and the beta0 % effective earth radius (4.3), troposcatter (4.4), ducting / layer reflection (4.5),
//   the blend of section 4.6 and the location variability of 4.7 / 4.9; path parameters from Attachment 1, I(x) from Attachment 2.
// Written from the recommendation text. Checked against all 57 ITU-R SG3 validation results of P_1812_8.zip (tests/prop/test_prop_p1812.cpp).
#pragma once
#include <vector>

namespace dect2::prop::p1812 {

struct Input {
    std::vector<double> dKm, hM, clutterM;   // profile from the transmitter: distance, terrain height above sea level, representative clutter
                                             // height added to the intermediate points (P.1812 eq. 1d); clutterM may be empty (bare terrain)
    double fGHz = 0.1;
    double timePct = 50;                     // p: the loss is not exceeded for p % of the time (1..50; 50 = median)
    double locPct = 50;                      // pL: ... and for pL % of locations (1..99)
    double txHeightM = 10, rxHeightM = 10;   // antenna centre above ground
    bool vertical = true;                    // polarisation (horizontal otherwise)
    double latDeg = 50;                      // path centre latitude
    double deltaN = 45;                      // average refractivity lapse rate of the lowest 1 km, N-units/km (P.453 map DN50; 45 = a common value)
    double n0 = 325;                         // sea-level surface refractivity (P.453 map N050)
    double seaFraction = 0;                  // omega: fraction of the path over water
    double dtmKm = -1, dlmKm = -1;           // longest continuous land / inland section (< 0: the whole path is inland)
    double sigmaLocDb = 5.5;                 // location variability standard deviation (Table 6: 5.5 dB; eq. 64 gives smaller values for small areas)
};

struct Result {
    bool valid = false;
    bool los = true;                         // no obstacle above the line of sight for the median earth radius
    double dKm = 0;
    double ae = 0, beta0 = 0;                // median effective earth radius (km), time percentage of anomalous refraction (%)
    double dlt = 0, dlr = 0, tht = 0, thr = 0, theta = 0;   // horizon distances (km), horizon angles and angular distance (mrad)
    double hts = 0, hrs = 0, hstd = 0, hsrd = 0, hte = 0, hre = 0, hm = 0;
    double lbfs = 0, lb0p = 0, lb0b = 0;     // free space, LoS for p % and beta0 %
    double lbulla = 0, lbulls = 0, ldsph = 0, ld50 = 0, ldb = 0, ldp = 0, lbd = 0;   // diffraction parts
    double lbs = 0, lba = 0;                 // troposcatter, ducting
    double lbc = 0;                          // p % time, 50 % locations
    double sigmaLoc = 0;
    double lb = 0;                           // p % time, pL % locations: the answer (dB)
    int mechanism = 0;                       // the strongest contribution: 0 line of sight, 1 diffraction, 2 troposcatter, 3 ducting
};

Result compute(const Input& in);
double knifeEdgeJ(double v);                                             // eq. (12)
double invQ(double x);                                                   // Attachment 2 (0.000001..0.999999)
double sigmaLocationDb(double fGHz, double resolutionM);                 // eq. (64)
double heightVariation(double hM, double clutterM);                      // u(h), eq. (65)

} // namespace dect2::prop::p1812
