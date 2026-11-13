// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// One receiver object for the 4.0 signal modes (docs/X40-4.0.md): which decoder runs is set from the active standard, as the avroad receiver does.
// Receive only. Written to the engine's contract: configure() once the sample rate is known, feed() from the analysis thread,
// telemetry() and the setters from the interface thread.
#pragma once
#include "dect2/engine/mode_tuning.h"
#include "dect2/common/ring.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

// every active standard of the receiver (882 - 897 and, for the third block, 753 - 760), for switch statements
#define DECT2_X40_ACTIVE_CASES case 753: case 754: case 755: case 756: case 757: case 758: case 759: case 760: case 761: case 762: case 763: case 764: case 765: case 766: case 767: case 768: case 769: case 770: case 771: case 772: case 773: case 774: case 775: case 776: case 777: case 778: case 779: case 780: case 781: case 782: case 783: case 786: case 863: case 371: case 372: case 373: case 374: case 375: case 376: case 331: case 882: case 883: case 884: case 885: case 886: case 887: case 888: case 889: case 890: case 891: case 892: case 893: case 894: case 895: case 896: case 897:

namespace dect2 {

struct X40Telemetry;

namespace x40 {
enum Variant : int { RcPpm, Uvb76, Graves, Jove, Beacon, Em4100, UhfRfid, Qi, FmMon, AmMon, PlNoise, Radar, GnssJam, Passband, VlfMsk,
               Occupancy, Evm, FreqAcc, PttLog, Intermod, Spur, DopplerLog, HopAnalyser, Birdie, Burst, Fading, InputHealth, PhaseNoise, SsbTest, Sinad, FskAnalyser, YFactor, CwKeying, NoiseFloor, NfmDev, Afsk, OokPulse, TxStability, OccBw, Papr, SpecKurt, Glitch,   // third block (docs/X40-4.0.md, feat40-modes3): families 752 - 759
               Marker,   // feat40-modes4: families 779 and up
               FskModem,   // feat40-modes4: families 780 and up
               SsrInt,   // feat40-modes4: families 781 and up
               Dcr,      // feat40-dcr: family 782 (Japan digital convenience radio, ARIB STD-T98 vol. 3), docs/DCR-4.0.md
               Xpa, Xpa2, Crowd36, F01, F06, Gw,   // feat40-rivet: the HF modes taken over from Rivet, families 370 - 375 (docs/RIVET-MODES-4.0.md)
               Gsc,   // feat41-gsc: Golay paging, family 330, docs/GSC-4.1.md
               Stl,   // feat41-stl: STL link (analogue studio-to-transmitter links), family 785, docs/STL-4.1.md
               Scada, // feat41-scada: SCADA radio telemetry (Modbus RTU, DNP3), family 862, docs/SCADA-4.1.md
               kNumVariants };
// variants 0 - 14: family 881 + v, active standard 882 + v, engine standard 883 + v, poll slot 881 + v.
// variants 15 and up (third block): family 752 + (v - 15), active standard family + 1, engine standard family + 2, poll slot = family. kNumFamilies = 900 and kPollSeqSlots = 901 stay.
// the Rivet block (variants Xpa and up): family 370 + (v - Xpa), the same layout.
constexpr int kFamilyBase = 880, kFirstBlock = 15, kThirdFamily = 752, kRivetFirst = Xpa, kRivetFamily = 370, kGscFamily = 330, kStlFirst = Stl, kStlFamily = 785, kScadaFamily = 862;   // Stl: its own number, family 785; Scada: its own number, family 862 (active 863, engine 864, poll slot 862) (783 / 784 are the active and engine standards of Dcr)
constexpr int familyOfVariant(int v) { return v == Scada ? kScadaFamily : v == Gsc ? kGscFamily : v < kFirstBlock ? kFamilyBase + 1 + v : v < kRivetFirst ? kThirdFamily + (v - kFirstBlock) : v < (int)Gsc ? kRivetFamily + (v - kRivetFirst) : kStlFamily + (v - kStlFirst); }
constexpr int activeOfVariant(int v) { return familyOfVariant(v) + 1; }
constexpr int variantOfActive(int active) {
    return active >= kFamilyBase + 2 && active <= kFamilyBase + 1 + kFirstBlock ? active - kFamilyBase - 2
         : active >= kThirdFamily + 1 && active <= kThirdFamily + (int)kRivetFirst - kFirstBlock ? kFirstBlock + active - kThirdFamily - 1
         : active >= kRivetFamily + 1 && active <= kRivetFamily + (int)Gw + 1 - kRivetFirst ? kRivetFirst + active - kRivetFamily - 1
         : active == kGscFamily + 1 ? (int)Gsc
         : active == kScadaFamily + 1 ? (int)Scada
         : active == kStlFamily + 1 ? (int)Stl : -1;
}
const char* variantName(int v);
} // namespace x40

class X40Receiver {
public:
    X40Receiver();
    ~X40Receiver();
    void setVariant(int v);              // before configure()
    int variant() const;
    void configure(double inputRateHz);
    void setSignalOffset(double hz);
    void setCenterHz(double hz);
    bool ready() const;
    void reset();                        // the demodulators; the decoded list stays
    void feed(const cf32* x, size_t n);
    bool telemetry(X40Telemetry& out, uint64_t lastSeq);
    void clearResults();
    void clearResults(int variant);
    void setLogCallback(std::function<void(const std::string&)> cb);
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

ModeTuning x40Tuning(int variant);

} // namespace dect2
