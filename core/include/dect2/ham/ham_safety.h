// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The safety layer every ham transmission passes, whatever the radio (a CAT rig, an SDR): the country band plan, the duty-cycle limits per mode and
// the TX audit log. The gate itself is hamTxAllowed (ham.h): licence confirmed, Arm, callsign, locator, inside an amateur band (and inside the
// country's own bands when HamTxCheck::country is set). Nothing here keys a radio.
#pragma once
#include <string>
#include <vector>

namespace dect2::ham {

// ---- the country band plan (data/bandplans/builtin.json: the IARU / national allocations per country) ----
struct HamPlanBand { std::string name; double loHz = 0, hiHz = 0; };
// The amateur bands of a country (ISO 3166 code "LT", "US", case-insensitive) or band-plan id ("eu", "us"): the plan's "ham" bands, each cut to the
// union table hamBands() so a plan can only narrow what is allowed. Empty when the country is not known or its plan lists no amateur band.
std::vector<HamPlanBand> hamCountryBands(const std::string& country);
bool hamCountryKnown(const std::string& country);        // a built-in plan exists for it
std::string hamCountryPlanName(const std::string& country);   // "Lithuania", "" when unknown
// dialHz + audioHz .. + bwHz wholly inside one of the country's amateur bands. why: the reason when false.
bool hamSignalInCountryBand(const std::string& country, double dialHz, double audioHz, double bwHz, std::string& why);

// ---- duty-cycle limits (the transmitter's own protection and a guard against a runaway sequencer) ----
struct HamDutyLimit {
    double windowSec = 600;      // the sliding window
    double maxFraction = 0.5;    // at most this part of the window on air
    double maxBurstSec = 200;    // one burst never longer than this
};
// mode: the hfdig numbers of ham.h (FT8 0, FT4 1, FT2 2, WSPR 3, JT65 4, JT9 5, JT4 10); -1 = the keyboard / audio modes.
// The slot modes: 60 % of 10 minutes (one slot in two is about 42 % for FT8, 46 % for WSPR), the burst at most one slot; keyboard: 50 % of 10 min, 200 s.
HamDutyLimit hamDutyLimit(int mode);
// Seconds on air inside the window ending at now, from (start, seconds) pairs.
double hamDutyUsed(const std::vector<std::pair<double, double>>& log, double now, double windowSec);

// ---- the TX audit log: what was sent when, through which path; never credentials or keys (only the on-air text, which is public) ----
struct TxAuditEntry {
    double utc = 0;              // when the attempt started
    std::string path;            // "rig", "sdr", "test"
    std::string mode;            // "FT8", "PSK31", "audio"
    std::string result;          // "sent", "halted", "refused", "failed"
    std::string reason;          // why it was refused / failed ("" when sent)
    std::string message;         // the on-air text (control characters removed, at most 160 characters)
    double dialHz = 0, audioHz = 0, bwHz = 0, seconds = 0;
    std::string call;            // the operator's callsign (it is sent on air anyway)
};
// The file the log is appended to (one line per attempt; "" = memory only, the default). The directory must exist.
void txAuditSetFile(const std::string& path);
std::string txAuditFile();
void txAuditAdd(const TxAuditEntry& e);
std::vector<TxAuditEntry> txAuditRecent(size_t max = 200);   // newest last
void txAuditClear();                                         // the in-memory list (tests)
std::string txAuditLine(const TxAuditEntry& e);              // "2026-10-22T12:00:15Z sdr FT8 sent 14074000+1500 Hz 50 Hz 12.6 s LY1ABC \"CQ LY1ABC KO24\""
std::string txAuditClean(const std::string& text);           // the message as the log keeps it

}  // namespace dect2::ham
