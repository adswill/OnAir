// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// FM receiver telemetry, shared by the receiver and the UI.
#pragma once
#include "dect2/common/ring.h"
#include "fm_rds.h"
#include "rds_tmc.h"
#include "fm_subcarrier.h"
#include <cstdint>
#include <string>
#include <vector>

namespace dect2 {

struct FmTelemetry {
    uint64_t seq = 0;
    int state = 0;              // 0 searching (no carrier), 1 tracking (a weak carrier), 2 locked (a clean carrier)
    double cfoHz = 0;           // tuning error: the mean frequency of the carrier
    float snrDb = 0;            // audio signal-to-noise ratio, from the noise above the multiplex (0 - 60)
    float devKhz = 0;           // peak frequency deviation over the last moment (75 kHz = 100 % modulation)
    bool carrier = false;       // a constant-envelope carrier is in the channel (noise alone is not)
    float levelDbfs = -120;     // power of the selected channel after filtering
    float multipathPct = 0;     // ripple of the channel's level over the frequencies the modulation sweeps (an echo), %
    bool equaliser = false;     // the multipath equaliser is in use (multipathPct above about 6 %)
    float neighbourDb = 0, neighbourKhz = 0;   // the neighbouring station being cancelled: its level over this one and its offset (0: none)
    uint64_t clicksFixed = 0;   // discriminator clicks repaired since the start (a weak station)
    bool stereo = false;        // the 19 kHz pilot is locked and the stereo decoder is on
    float stereoPct = 0;        // how much of the stereo difference is in the sound: 100 full stereo, lower = blended towards mono (a weak station)
    bool squelched = false;     // the squelch (setSquelch) holds the sound muted
    float pilotPct = 0;         // pilot injection in % of 75 kHz deviation (a healthy station: 8 - 10)
    // ITU-R BS.412 (smart5/mpx412.h): the multiplex power integrated over 60 s in dBr (0 dBr = a sine of 19 kHz peak deviation; the usual
    // limit is +0 dBr), the window being filled, and the peak-deviation points of the same minute
    bool mpx412Valid = false;   // a whole minute has been measured
    float mpxPowerDbr = -99, mpxRunningDbr = -99;
    float devP50Khz = 0, devP99Khz = 0, devP999Khz = 0, over75Pct = 0;
    FmSubcarrierStatus sub;     // RDS2 and SCA subcarriers
    bool rdsSync = false;       // RDS block synchronisation
    float rdsBlockOkPct = 0;    // share of RDS blocks with a good checkword, recent
    uint64_t rdsGroups = 0;     // RDS groups decoded since start

    // RDS contents (empty until received)
    std::string psName;         // programme service name, 8 characters
    std::string radioText;      // radio text, up to 64 characters
    bool radioTextComplete = false;   // every segment of the radio text received (not only a part of it)
    int ptyCode = -1;           // programme type number 0..31 (-1 until a group came)
    std::string ptyText;        // programme type (the RBDS names when rbds)
    bool rbds = false;          // the RBDS (US) meanings are in use, by the setting or Auto from the tuned frequency
    std::string callSign;       // US call letters from the PI code (RBDS only, empty when the PI holds none)
    bool trafficAlert = false;  // TA: a traffic announcement is on air
    bool trafficProgram = false;// TP: the station carries traffic announcements
    bool music = false;         // M/S flag: music (true) or speech (false)
    TmcSystem tmc;              // RDS-TMC announcement (group 3A); tmc.seen = the station carries TMC
    std::vector<TmcMessage> tmcMessages;   // traffic messages not yet expired, newest first
    int piCode = 0;             // programme identification, 0 = unknown
    RdsInfo rds;                // the rest of RDS: AF, PTYN, ECC, PIN, clock, ODA, RT+, eRT, EON, the name being collected, block counts

    std::vector<float> mpxDb;   // spectrum of the multiplex, 0 .. mpxMaxHz, dB relative to full deviation
    float mpxMaxHz = 0;
    std::vector<cf32> rdsConst; // recent RDS symbols after the matched filter (BPSK on the real axis)
};

} // namespace dect2
