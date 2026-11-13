// Transmit planning helpers (warn only, nothing here ever blocks a transmission): a link-budget estimate, a frequency-plan check that lists
// what a carrier or its harmonics land on, and a power ramp with an SWR-based advice. The panels show these as notes next to the Arm switch.
#pragma once
#include <cmath>
#include <string>
#include <vector>
namespace dect2 {

// free-space path loss in dB (distance in km, frequency in MHz)
inline double fsplDb(double km, double mhz) { return km <= 0 || mhz <= 0 ? 0.0 : 20.0 * std::log10(km) + 20.0 * std::log10(mhz) + 32.44; }

struct LinkBudget {
    double txDbm = 0;        // power at the SDR / amplifier output
    double txLossDb = 0;     // cable, filter
    double txGainDbi = 0;
    double rxGainDbi = 0;
    double rxLossDb = 0;
    double extraLossDb = 0;  // terrain, rain, body, fade margin the user wants to count
    double rxSensDbm = -110; // receiver sensitivity at the wanted bit rate
};
struct LinkResult { double eirpDbm = 0, pathLossDb = 0, rxDbm = 0, marginDb = 0; double maxKm = 0; };
// distance km, frequency MHz; maxKm: the free-space distance at which the margin is zero
inline LinkResult linkBudget(const LinkBudget& b, double km, double mhz) {
    LinkResult r;
    r.eirpDbm = b.txDbm - b.txLossDb + b.txGainDbi;
    r.pathLossDb = fsplDb(km, mhz) + b.extraLossDb;
    r.rxDbm = r.eirpDbm - r.pathLossDb + b.rxGainDbi - b.rxLossDb;
    r.marginDb = r.rxDbm - b.rxSensDbm;
    const double allow = r.eirpDbm - b.extraLossDb + b.rxGainDbi - b.rxLossDb - b.rxSensDbm;   // total free-space loss that is still allowed
    r.maxKm = mhz > 0 ? std::pow(10.0, (allow - 32.44 - 20.0 * std::log10(mhz)) / 20.0) : 0;
    return r;
}

struct PlanWarning { std::string what; double mhz = 0; };
// Protected / sensitive bands a carrier or one of its harmonics (2nd to 5th) should not land on. Nothing is blocked: the list is shown as a warning.
inline std::vector<PlanWarning> planCheck(double carrierMhz) {
    struct B { double lo, hi; const char* name; };
    static const B kBands[] = {
        {108.0, 117.975, "aeronautical navigation (VOR / ILS)"}, {117.975, 137.0, "airband voice"}, {121.4, 121.6, "aviation emergency 121.5 MHz"},
        {156.7, 156.9, "marine distress channel 16"}, {242.9, 243.1, "military emergency 243 MHz"}, {406.0, 406.1, "distress beacons (EPIRB / ELT) 406 MHz"},
        {960.0, 1215.0, "aeronautical DME / SSR / ADS-B"}, {1164.0, 1300.0, "satellite navigation (GPS L5, L2, Galileo)"}, {1559.0, 1610.0, "satellite navigation (GPS L1, GLONASS)"},
        {1525.0, 1559.0, "mobile satellite downlinks (Inmarsat, Iridium)"}, {1610.0, 1626.5, "Iridium"}};
    std::vector<PlanWarning> w;
    if (carrierMhz <= 0) return w;
    for (int h = 1; h <= 5; h++) {
        const double f = carrierMhz * h;
        for (const auto& b : kBands) {
            if (f >= b.lo && f <= b.hi) {
                w.push_back({std::string(h == 1 ? "the carrier is inside " : "harmonic " + std::to_string(h) + " is inside ") + b.name, f});
            }
        }
    }
    return w;
}

// Output powers (dBm) to step through from startDbm up to targetDbm in stepDb steps, the last step is exactly the target. Always at least one.
inline std::vector<double> powerRamp(double startDbm, double targetDbm, double stepDb) {
    std::vector<double> v;
    if (stepDb <= 0 || startDbm >= targetDbm) { v.push_back(targetDbm); return v; }
    for (double p = startDbm; p < targetDbm - 1e-9; p += stepDb) v.push_back(p);
    v.push_back(targetDbm);
    return v;
}

// Share of the forward power that is reflected for a given SWR, and how far to back off: advice only. SWR <= 1.5 no change, above 3 half power (-3 dB).
inline double reflectedFraction(double swr) { if (swr < 1.0) swr = 1.0; const double g = (swr - 1.0) / (swr + 1.0); return g * g; }
inline double swrBackoffDb(double swr) {
    if (swr <= 1.5) return 0;
    if (swr >= 3.0) return swr >= 5.0 ? 10.0 : 3.0 + (swr - 3.0) * 3.5;
    return (swr - 1.5) * 2.0;   // 1.5 -> 0 dB, 3 -> 3 dB
}

}  // namespace dect2
