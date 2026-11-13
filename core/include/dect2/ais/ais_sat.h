// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Satellite AIS: what changes when the receiver is on a low earth orbit satellite (or the IQ comes from one) instead of on the coast.
//  - Doppler: up to about +-4 kHz at 162 MHz from 500 km, changing by up to some 50 Hz/s; every ship has its own shift (aisMaxDopplerHz).
//  - Long delays: 1.7 to 8 ms from 500 km, so bursts of neighbouring slots from ships at different ranges run into each other.
//  - Collisions: a footprint some 2250 km wide holds many SOTDMA cells that do not see each other, so their bursts land in one slot.
// aisDecodeCollided() takes one detected burst (which may be several on top of each other) and tries: the burst as it is; the parts between power
// steps (a burst that starts or ends inside another); frequency candidates (the Doppler shifts of stations heard recently, and a grid over the
// +-4 kHz search range), each through the narrow filter; and successive cancellation: every decoded burst is modulated again, fitted (timing,
// frequency, amplitude and phase) and subtracted, and the rest is decoded again.
// AisSatStats keeps the analysis of the satellite AIS literature (Li, Chen, Chen, "Long-range AIS message analysis based on the TianTuo-3
// micro satellite", Acta Astronautica 2017, doi:10.1016/j.actaastro.2017.02.014): the ships heard per pass (the "eavesdropper" map), the
// "new-type" vessels (those that send message 27) and their ratio, the message-type mix, SNR against the satellite's elevation as seen from the
// ship, and a detection probability per month. docs/SATELLITE_AIS.md.
#pragma once
#include "ais_phy.h"
#include "ais_tel.h"
#include "dect2/geo/passes.h"
#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace dect2 {

// ---- geometry
double aisMaxDopplerHz(double freqHz, double altKm);       // the largest shift a ship on the horizon sees (the satellite's speed along the line of sight)
double aisSlantRangeKm(double altKm, double elevDeg);      // from a ship to a satellite at altKm, seen at elevDeg
double aisSwathKm(double altKm, double minElevDeg = 0);    // width on the ground of the area the satellite sees above minElevDeg
inline double aisDelayMs(double rangeKm) { return rangeKm / 299.792458; }

// ---- collision-tolerant decoding of one burst (48 kHz, as aisDecodeBurst)
struct AisCollisionConfig {
    int maxLayers = 3;               // decoded bursts cancelled one after another (0 = no cancellation)
    int maxCandidates = 8;           // frequency candidates tried per layer
    double searchHz = 4500;          // the Doppler grid covers +-searchHz around the burst's own frequency
    double gridHz = 1500;            // its step
    double minSepHz = 500;           // candidates closer than this to one already tried are skipped
    bool splitWindows = true;        // decode the parts of a burst between power steps
    const std::vector<float>* chanTaps = nullptr;   // the receiver's channel filter at 48 kHz: the cancelled burst goes through it as the signal did
};
struct AisFound {
    ais::Bits payload;               // without FCS
    double cfoHz = 0;                // frequency of this burst
    double power = 0;                // mean power of the fitted burst inside it (0 when it was not fitted)
    int layer = 0;                   // 0 direct, 1 a part or a frequency candidate, 2+ after cancelling 1, 2 ... stronger bursts
};
struct AisCollisionResult {
    std::vector<AisFound> frames;    // every different payload with a good FCS
    int bad = 0;                     // as AisBurstResult::bad of the first, direct attempt
    double cfoHz = 0;                // the direct attempt's frequency
    int attempts = 0;                // aisDecodeBurst / aisDecodeBurstAt calls made
};
AisCollisionResult aisDecodeCollided(const cf32* x, size_t n, size_t core0, size_t core1, const AisPhyConfig& cfg,
                                     const std::vector<double>& freqHintsHz, const AisCollisionConfig& cc = AisCollisionConfig());

// The burst a payload makes at 48 kHz (unit amplitude before the ramps) starting at the fractional sample 'start', at cfoHz, n samples long
std::vector<cf32> aisBurstTemplate(const ais::Bits& payload, double start, double cfoHz, size_t n);
// Fits the burst of 'payload' in x (timing searched from 'from' to 'to', the first sample of the training sequence), refines its frequency,
// and subtracts it. Returns false when no fit was found. power: mean power of the fitted burst; startOut: where it was found.
bool aisCancelBurst(std::vector<cf32>& x, const ais::Bits& payload, double cfoHz, size_t from, size_t to, const std::vector<float>* taps = nullptr,
                    double* power = nullptr, double* startOut = nullptr, double* cfoOut = nullptr);

// ---- Doppler tracking of one channel: the recent bursts' frequencies, a straight-line fit for the trend and its rate
class AisDopplerTracker {
public:
    void add(double tSec, double hz);
    double estimate(double tSec) const;      // the fit at tSec (the median of the recent bursts when there are too few)
    double rateHzS() const { return rate_; }
    bool valid() const { return pts_.size() >= 3; }
    std::vector<double> recent(double tSec, double maxAgeSec = 30) const;   // the frequencies of bursts in the last maxAgeSec
    void reset() { pts_.clear(); rate_ = 0; icpt_ = 0; t0_ = 0; }
private:
    std::vector<std::pair<double, double>> pts_;
    double rate_ = 0, icpt_ = 0, t0_ = 0;
    void fit();
};

// ---- the analysis of satellite AIS passes
struct AisSatVessel {
    uint32_t mmsi = 0;
    double firstUtc = 0, lastUtc = 0;
    uint32_t msgs = 0, longRangeMsgs = 0, commonMsgs = 0;   // all, message 27, the common position reports (1 - 3, 18, 19)
    int passes = 0, lastPass = -1;
    bool hasPos = false; double lat = 0, lon = 0;
    float lastSnrDb = 0;
};
struct AisSatPass {
    int index = 0;
    std::string satName;
    double startUtc = 0, endUtc = 0;             // first and last reception (or the predicted AOS / LOS)
    bool predicted = false;                      // opened by beginPass (a predicted window), not by the first reception after a silence
    uint32_t detections = 0, longRange = 0;      // messages, of which message 27
    std::vector<uint32_t> vessels;               // MMSIs heard in it, sorted
    uint32_t newTypeVessels = 0;                 // of those, the ones that sent message 27 in it
    uint32_t expected = 0;                       // known ships that were inside the footprint (with a TLE)
    uint32_t expectedHeard = 0;                  // of those, the ones heard
    float maxElevDeg = -90;                      // the highest elevation of the satellite seen from a ship heard
    double snrSum = 0;
    std::array<uint32_t, 28> typeMix{};          // index 0: types above 27
};
struct AisSnrBin { double elevLo = 0, elevHi = 0; uint32_t n = 0; double snrMeanDb = 0; };
struct AisMonthStat { int year = 0, month = 0; uint32_t passes = 0, vessels = 0, newType = 0; double detectionProb = 0; bool fromFootprint = false; };

class AisSatStats {
public:
    AisSatStats();
    ~AisSatStats();
    void setSatellite(const Tle& t);             // the receiving satellite: elevation of each reception, footprints
    void clearSatellite();
    bool hasSatellite() const;
    void setPassGapSec(double s) { gapSec_ = s; }   // a silence this long ends a pass (default 15 minutes)
    void setMinElevDeg(double d) { minEl_ = d; }    // the footprint edge for "expected" (default 0)
    // a predicted pass window: receptions between aos and los belong to it
    void beginPass(const std::string& satName, double aosUtc, double losUtc);
    void add(const AisRxRecord& r);
    void addAll(const std::vector<AisRxRecord>& recent);   // the ones not seen yet (by AisRxRecord::n)
    void clear();

    const std::vector<AisSatPass>& passes() const { return passes_; }
    const std::map<uint32_t, AisSatVessel>& vessels() const { return vessels_; }
    uint64_t receptions() const { return total_; }
    uint32_t newTypeCount() const;               // vessels that sent message 27
    double newTypeRatio() const;                 // of all vessels heard
    double newTypeStillCommonShare() const;      // of the new-type vessels, the share also heard sending the common position reports
    std::array<uint64_t, 28> typeMix() const { return mix_; }
    std::vector<AisSnrBin> snrVsElevation(double binDeg = 10) const;
    std::vector<AisMonthStat> months() const;
    double elevationOf(double utc, double lat, double lon) const;   // satellite elevation seen from a ship (-90 without a TLE)

    // CSV (RFC 4180, header row): every reception, the passes, the vessels, the months
    std::string receptionsCsv() const;
    std::string passesCsv() const;
    std::string vesselsCsv() const;
    std::string monthsCsv() const;

private:
    struct Rx { AisRxRecord r; float elevDeg = -90; int pass = -1; };
    struct Impl;
    std::unique_ptr<Impl> sat_;
    std::vector<AisSatPass> passes_;
    std::map<uint32_t, AisSatVessel> vessels_;
    std::vector<Rx> rx_;
    std::array<uint64_t, 28> mix_{};
    uint64_t total_ = 0, lastN_ = 0;
    bool anyN_ = false;
    double gapSec_ = 900, minEl_ = 0;
    int openPass(double utc, const std::string& name, bool predicted, double los);
    int passFor(double utc);
    void finishFootprint(AisSatPass& p) const;
};

} // namespace dect2
