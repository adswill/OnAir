// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The published band plan of the QO-100 narrowband transponder (AMSAT-DL / BATC, downlink 10489.500 - 10490.000 MHz), a check of a signal
// against it, signal levels relative to the middle beacon (comparable between sessions and set-ups), and the plain-words status line.
#pragma once
#include "dect2/qo100/qo100_tel.h"
#include <cmath>
#include <map>
#include <string>

namespace dect2 {
namespace qo100 {

enum PlanUse { kPlanBeacon = 0, kPlanCw, kPlanNarrowDigital, kPlanDigital, kPlanSsb, kPlanMixed, kPlanUses };

struct PlanSegment {
    double loHz, hiHz;      // downlink RF, true frequency
    PlanUse use;
    const char* name;       // plain words
    double maxBwHz;         // widest signal allowed there (0 for a beacon: nothing may transmit)
};

const PlanSegment* bandPlan(int& count);                 // lowest first, gap-free from 10489.500 to 10490.000 MHz
const PlanSegment* segmentAt(double rfHz);               // nullptr outside the transponder
// "" when the signal fits its segment; else the reason in plain words (too wide for the segment, in a beacon segment, spans two segments)
std::string planProblem(double centreRfHz, double widthHz);
// Level of a signal relative to the middle (PSK) beacon, dB; false without a beacon in the report
bool levelOverBeaconDb(const Qo100Telemetry& t, double levelDb, double& relDb);
// One plain sentence on how the reception is doing and what to do next
std::string plainStatus(const Qo100Telemetry& t, bool running);

// ---- what a click on a signal does -------------------------------------------------------------------------------------------------------------
// QO-100's narrowband transponder is used upper sideband throughout (voice and digital) and CW as a beat note: the dial is the suppressed carrier
// of an SSB or digital signal, a bit under its lower edge; for CW it sits 700 Hz under the carrier. widthHz is the -6 dB width, topHz the top edge.
struct ClickTune { double dialHz; const char* mode; };   // mode: "CW", "USB (SSB)", "USB (digital)"
ClickTune clickTune(double topHz, double widthHz);

// ---- DATV carriers: a plain quality word from the Es/N0 ------------------------------------------------------------------------------------------
enum DatvQuality { kDatvNone = 0, kDatvWeak, kDatvUsable, kDatvLocked, kDatvGood };
constexpr double kDatvThresholdDb = 2.0;   // DVB-S2 QPSK 1/2 needs about 1 dB Es/N0 in theory; 2 dB with a real demodulator
// esN0Db: from the wideband spectrum (plateau over the noise floor, which is Es/N0 for a carrier that fills its bandwidth) or, for the carrier that
// is being decoded, the demodulator's own measurement. decoding: this carrier is the one being decoded; tsLock: the transport stream flows.
DatvQuality datvQuality(double esN0Db, bool decoding, bool tsLock);
const char* datvQualityName(DatvQuality q);
inline double datvMarginDb(double esN0Db) { return esN0Db - kDatvThresholdDb; }

// ---- the LNB error remembered per LNB ------------------------------------------------------------------------------------------------------------
// Keyed by the nominal oscillator in kHz; stored as ppm of the oscillator, so it carries over between oscillator settings of the same LNB.
class LnbMemory {
public:
    void remember(double nominalLoHz, double ppm) { m_[(long long)std::llround(nominalLoHz / 1e3)] = ppm; }
    bool recall(double nominalLoHz, double& ppm) const {
        auto it = m_.find((long long)std::llround(nominalLoHz / 1e3));
        if (it == m_.end()) return false;
        ppm = it->second; return true;
    }
    std::string serialize() const;                 // "9750000:3.214;10600000:-1.5"
    void parse(const std::string& s);              // lenient: bad entries are skipped
    size_t size() const { return m_.size(); }
private:
    std::map<long long, double> m_;
};

} // namespace qo100
} // namespace dect2
