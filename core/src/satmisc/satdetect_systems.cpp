// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The table of satellite systems handled by detection only (see satdetect_live.h). Every frequency here comes from a public source named in docs/SAT-MISC-3.8.md;
// a system's note says plainly what is not known. Family numbers: engine standard - 2.
#include "dect2/satmisc/satdetect_live.h"

namespace dect2 {
namespace satmisc {

namespace {
const SdChannelDef kZombie[] = {{145.970, "AO-7 VHF beacon"}, {435.106, "AO-7 UHF beacon"}, {136.650, "Transit 5B-5"}, {237.000, "LES-1"}, {236.750, "LES-5"}, {2272.500, "IMAGE"}};
const SdChannelDef kUhfSat[] = {{250.650, "UFO voice"}, {252.150, "FLTSATCOM"}};
const SdChannelDef kTdrs[] = {{2106.400, "TDRS MA forward"}};
const SdChannelDef kCss[] = {{2224.075, "Tiangong / Shenzhou TT&C"}, {2224.065, "Shenzhou beacon"}, {2232.220, "Tiangong telemetry"}, {2250.720, "Tiangong telemetry"}, {145.985, "Tiangong amateur"}, {436.510, "Tiangong amateur"}};
const SdChannelDef kTsikada[] = {{150.000, "Tsikada / Parus 150 MHz"}, {400.000, "Tsikada / Parus 400 MHz"}};
const SdChannelDef kIss[] = {{145.800, "FM voice / SSTV"}, {145.825, "APRS packet"}, {437.800, "Voice repeater"}, {437.550, "UHF simplex"}};
}

const std::vector<SatSystem>& satSystems() {
    static const std::vector<SatSystem> t = {
        {552, "zombie", "Zombie satellites", 145.970, 100, 3000, 1e6, SdMethod::Carrier, 8, 12, 0, 0, kZombie, 6,
         "Defunct satellites reported still transmitting. A carrier is matched by frequency and Doppler. LES-5 and IMAGE orbit too high for the predictor: their carriers are shown without a Doppler check.", true},
        {553, "whatsat", "What satellite is this?", 437.500, 30, 6000, 1e6, SdMethod::Carrier, 8, 12, 0, 0, nullptr, 0,
         "Follows every carrier in the capture and names the satellite whose predicted Doppler curve fits (near-earth orbits only). Needs elements and your location.", true},
        {554, "uhfsatcom", "UHF SATCOM skimmer", 255.000, 225, 400, 10e6, SdMethod::Channel, 1000, 8, 0, 5000, kUhfSat, 2,
         "Skims the 240 - 270 MHz military satellite downlinks for active channels (5 and 25 kHz). Energy and width only: no decoding.", false},
        {555, "gmr1", "Thuraya GMR-1 channels", 1535.000, 1525, 1559, 10e6, SdMethod::Channel, 2000, 8, 0, 31250, nullptr, 0,
         "Thuraya's GMR-1 downlink, 1525 - 1559 MHz. The standard is public (ETSI TS 101 376) but no decoder is implemented: active 31.25 kHz channels only.", false},
        {556, "starlink", "Starlink detector", 11325.000, 10700, 12700, 20e6, SdMethod::FrameGap, 0, 8, 750, 0, nullptr, 0,
         "Starlink's Ku downlink has no public specification. The 750 Hz frame rate is from published reverse engineering (Humphreys et al.). Detects the frame gap only; needs an LNB and a dish.", false},
        {557, "oneweb", "OneWeb detector", 11075.000, 10700, 12700, 20e6, SdMethod::RepeatSeq, 0, 8, 1000, 0, nullptr, 0,
         "OneWeb has no public specification. The 1 ms repeating sequence is from published reverse engineering (Komodromos and Humphreys). Detects that repetition only; needs an LNB and a dish.", false},
        {558, "tdrs", "TDRS carriers", 2106.400, 2025, 2300, 2e6, SdMethod::Carrier, 30, 10, 0, 0, kTdrs, 1,
         "NASA TDRS forward links. The band is public (NTIA); the data is spread and not decoded. Reception from the ground has not been verified here.", false},
        {559, "css", "Tiangong / Shenzhou carriers", 2224.075, 130, 2300, 1e6, SdMethod::Carrier, 10, 10, 0, 0, kCss, 6,
         "Chinese crewed spacecraft. Carrier frequencies as reported by SatNOGS and Orbital Focus; the telemetry format is not public, so carriers only.", true},
        {560, "tsikada", "Tsikada / Parus", 150.000, 140, 410, 1e6, SdMethod::Carrier, 8, 10, 0, 0, kTsikada, 2,
         "Russian Transit-type navigation satellites, nominally 150 and 400 MHz. The data format is not public; carriers and Doppler only.", true},
        {561, "gonets", "Gonets", 388.500, 380, 395, 4e6, SdMethod::Channel, 500, 8, 0, 25000, nullptr, 0,
         "Russian store-and-forward messaging, 387 - 390 MHz downlink. No public protocol description was found: bursts and channels only.", false},
        {562, "tiantong", "Tiantong signalling", 2185.000, 2170, 2200, 10e6, SdMethod::Channel, 2000, 8, 0, 25000, nullptr, 0,
         "Chinese S-band mobile satellite. No public air interface and the band is the ITU allocation, not a confirmed plan: channel energy only.", false},
        {564, "issdown", "Space station", 145.800, 130, 3000, 1e6, SdMethod::Carrier, 10, 10, 0, 0, kIss, 4,
         "Every ISS downlink: FM voice and SSTV on 145.800, APRS on 145.825, the repeater on 437.800. Shows which is on; only one is usually active at a time.", true},
    };
    return t;
}

double issNextCentreMhz(double currentMhz) { return currentMhz < 300 ? 437.800 : 145.8125; }

const SatSystem* satSystem(int stdMode) {
    for (const auto& s : satSystems()) if (s.stdMode == stdMode) return &s;
    return nullptr;
}

} // namespace satmisc
} // namespace dect2
