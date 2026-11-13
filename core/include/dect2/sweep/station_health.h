// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Station and antenna health report. One run retunes the radio over a few fixed bands and measures, per band, the noise floor, the strongest signal, the
// ADC level (rms, peak, clipping) and the reference stations that should be there (FM, DAB, DVB-T); it also takes an ADS-B range plot and a
// frequency error (ppm) from a known carrier when the caller has them, and turns it all into plain findings (add a filter or LNA, lower the gain,
// move the antenna). The analysis is plain logic on Sweep structs, so it is tested on synthetic sweeps; runHealthCheck() only drives a SweepSource.
// The numbers are relative (dB against the receiver's own floor and ADC): the radio is not calibrated, so there are no dBm values and no
// claim about the antenna's gain. The thresholds are rules of thumb, listed in docs/STATION_HEALTH.md.
#pragma once
#include "dect2/sweep/sweep.h"
#include <functional>
#include <string>
#include <vector>

namespace dect2 {
namespace sweep {

enum class HealthRef { None, Fm, Dab, DvbT };

struct HealthBand {
    std::string id;                  // "fm", "air", "dab", "uhf", "ism868", "adsb"
    double startHz = 0, stopHz = 0;
    double binHz = 25e3;             // sweep bin width
    HealthRef ref = HealthRef::None; // the broadcast service looked for in this band
};
// The fixed bands that lie entirely inside the radio's range (minHz/maxHz 0 = unknown: all of them)
std::vector<HealthBand> healthBands(double minHz = 0, double maxHz = 0);

// What the ADC saw in a capture (complex float, full scale = 1.0 per component)
struct AdcStats {
    double rmsDbfs = -120, peak = 0, clipFraction = 0;
    size_t n = 0;
};
AdcStats measureAdc(const cf32* x, size_t n, float clipLevel = 0.98f);

// The gain-stage model shared with the AGC (core/include/dect2/source/agc.h): quantisation noise of a full-scale-limited complex ADC (dBFS over the
// whole rate), and how far the receiver's noise (a noise floor of floorDb per Hann-windowed bin of binHz, over rateHz) sits above it. Small = the ADC
// limits sensitivity (raise the gain), large = the gain buys nothing more.
double adcQuantFloorDbfs(int adcBits);
double adcMarginDb(double floorDbPerBin, double binHz, double rateHz, int adcBits);

enum class RefLevel { NotChecked, Absent, Weak, Ok };
struct RefResult {
    HealthRef kind = HealthRef::None;
    RefLevel level = RefLevel::NotChecked;
    int count = 0;                   // services found (FM carriers, DAB ensembles, DVB-T channels)
    double strongestHz = 0;
    double strongestSnrDb = 0;       // over the band's floor
    double expectedMinDb = 0;        // the SNR a healthy antenna shows for the strongest one
};

struct BandResult {
    HealthBand band;
    bool measured = false;
    double rateHz = 0;
    double floorDb = -150;           // sweep noise floor per bin
    double densityDbfsHz = -200;     // the same per Hz (comparable between bands)
    double noiseTotalDbfs = -150;    // the noise over the whole sample rate, for comparison with the ADC's own noise
    double adcMarginDb = 0;          // noiseTotalDbfs over the ADC's quantisation noise: small = the ADC limits sensitivity
    double peakDb = -150, peakSnrDb = 0, occupancyPct = 0;
    int nSignals = 0;
    AdcStats adc;
    RefResult ref;
};
// One band from its sweep and capture. adcBits is the radio's converter width (8 for HackRF / RTL-SDR).
BandResult summariseBand(const HealthBand& b, const Sweep& s, const AdcStats& adc, int adcBits = 8);

struct PpmResult {
    bool valid = false;
    double expectedHz = 0, measuredHz = 0, errorHz = 0, ppm = 0;   // ppm > 0: the radio's clock runs fast
};
// The strongest peak within +-searchHz of a carrier of known frequency, with parabolic interpolation (needs a bin well below the error being measured)
PpmResult frequencyError(const Sweep& s, double expectedHz, double searchHz);

struct RangeResult {
    bool valid = false;              // enough positions to judge
    double maxNm = 0, meanSectorNm = 0;
    int sectorsWithData = 0, blindSectors = 0;   // blind: no position at all while the neighbours on both sides have some
    int bestSector = 0, worstSector = 0, blindStart = 0;   // of those with data (10 degree sectors, 0 = north)
    double ratio = 0;                // weakest sector with data / strongest
};
RangeResult assessRange(const float* sectorNm, int nSectors, unsigned long long positions);

enum class Severity { Info, Warn, Bad };
enum class FindCode {
    Overload,        // where = band, a = clipped %, b = peak
    HotHeadroom,     // where, a = rms dBFS
    AdcLimited,      // where, a = margin dB  (gain can still go up -> RaiseGain, else AddLna)
    AdcLimitedMax,   // same, but the gain is already at the top of the radio's range
    WastedGain,      // where, a = dB the gain could drop without losing sensitivity
    StrongOutOfBand, // where = the strong band, a = its peak SNR, b = clipped %: a notch / band-stop filter on that band
    NoiseExcess,     // where, a = dB above the quietest band
    RefAbsent,       // where = service, a = expected min SNR
    RefWeak,         // where, a = measured, b = expected
    PpmHigh,         // a = ppm
    PpmOk,           // a = ppm
    RangeShort,      // a = max NM, b = sector (deg) of the weakest direction
    RangeBlind,      // a = number of blind sectors, b = first blind sector's start (deg)
    RangeNone,       // no ADS-B positions yet
    AllGood
};
struct Finding {
    FindCode code = FindCode::AllGood;
    Severity severity = Severity::Info;
    std::string where;
    double a = 0, b = 0;
};

struct HealthInput {
    std::vector<BandResult> bands;
    double gainDb = 0, gainMaxDb = 0;   // current and highest gain of the radio (gainMaxDb 0 = unknown)
    bool haveRange = false; RangeResult range;
    bool havePpm = false; PpmResult ppm;
    double ppmLimit = 5;                // above this the clock is worth correcting
};
struct HealthReport {
    HealthInput in;
    std::vector<Finding> findings;      // worst first
    Severity worst = Severity::Info;
};
HealthReport assessHealth(const HealthInput& in);

// JSON of the report (numbers and finding codes; the app writes the readable text in the user's language)
std::string healthJson(const HealthReport& r);
const char* findCodeName(FindCode c);
const char* refName(HealthRef r);   // "FM", "DAB", "DVB-T", ""

struct HealthConfig {
    std::vector<HealthBand> bands;
    double dwellSec = 0.05, settleSec = 0.01;
    int adcBits = 8;
    double adcSeconds = 0.05;           // capture length for the ADC statistics
};
// Runs the bands on the source. progress(i, n, bandId) before each band. False when cancelled or a retune failed (partial results are kept).
bool runHealthCheck(SweepSource& src, const HealthConfig& c, std::vector<BandResult>& out, std::string& err,
                    const std::function<void(int, int, const std::string&)>& progress = nullptr, const std::atomic<bool>* cancel = nullptr);

} // namespace sweep
} // namespace dect2
