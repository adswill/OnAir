// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// KAVB as a transmit mode: the TX session's generator. The payload packets of the container layer (kavb_av.h) go through the Packetiser (FEC groups) and the
// PHY transmitter (kavb_phy.h), and the 4 samples per symbol stream is resampled to the radio's rate.
// Payload source: a file (TxRequest::file; its bytes are sent as Data packets of stream 1, up to 64 MiB, so a receiver that writes the payload to a file gets the
// file back when nothing was lost) or, without a file, a synthetic test stream (video and audio sized units from SyntheticAv, about `kavb.seconds` of media time).
// No AV1 or Opus encoder is built in, so a camera or a screen is not a source. The station ID is sent as a Text packet "KAVB de <callsign>" at the start and about
// twice a second (the spec's beacon slice is not built; this is OnAir's own stand-in that the receiver understands).
// Options: opt "kavb.profile" (0 .. 3 = A .. D, default 0), "kavb.fec" (FEC group profile 0 .. 7, default 1 = 14 + 2), "kavb.seconds" (synthetic stream, 1 .. 600,
// default 5). The callsign is up to 7 characters of A-Z 0-9 and "/" (lower case is raised); empty is ONAIR.
// The generator is finite: finished() is true after the last frame. Nothing about mode, frequency or power is limited here.
#pragma once
#include "dect2/tx/tx.h"
#include <memory>
#include <string>
#include <vector>

namespace dect2 {

// nullptr and err (plain words) when the callsign, profile, FEC profile, file or rate (below 1.6 x the symbol rate of the profile) cannot be used
std::unique_ptr<TxGenerator> makeKavbTxGenerator(const TxRequest& r, double rateHz, std::string& err);

struct KavbTxPlan {
    std::string callsign;
    int profile = 0, fec = 1;
    double seconds = 5;                 // synthetic stream only
    std::vector<uint8_t> file;          // empty: synthetic
};
bool kavbFromRequest(const TxRequest& r, KavbTxPlan& plan, std::string& err);

// Symbol rate of a profile (A 700, B 1050, C and D 1400 ksym/s) and the lowest radio rate the generator accepts for it.
double kavbSymRate(int profile);
double kavbMinRate(int profile);

// Airtime in seconds (an estimate from the bit budget of the profile and FEC profile; the synthetic stream is about 80% of the video budget plus 96 kbit/s audio).
double kavbAirtimeSeconds(size_t fileBytes, int profile, int fec, double syntheticSeconds);

} // namespace dect2
