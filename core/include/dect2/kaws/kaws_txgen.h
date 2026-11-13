// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// KAWS as a transmit mode: the TX session's generator. The signal is the audio of an upper-sideband transmitter as complex IQ: audio frequency = offset
// from the dial frequency (the 2.4 kHz of KAWS sit at 300 .. 2700 Hz). The mode is simulation-only: it has never been on the air.
// A chat message or a beacon is cut into blocks (kaws_frame.h) and sent in the UTC slots of the profile (kaws_sched.h): the caller's (even) slots, the
// burst starting 1.5 s after the slot start, so the computer clock must be UTC-accurate (NTP). The transmission is one-way: no ack is waited for, so the
// whole message is sent `kaws.repeat` times (a receiver that has the message ignores the repeat).
// Options: opt "kaws.kind" (0 chat, 1 short beacon, 2 full beacon; default 0), "kaws.profile" (0 .. 5, default 3 = Chat), "kaws.repeat" (1 .. 9, default 2),
// "kaws.eirp" (dBm announced in a beacon, default 30), "kaws.align" (1 wait for the next slot, default; 0 the first burst starts at once),
// "kaws.msgid" (0 .. 15, default from the clock), "kaws.utc" (a test clock, seconds since the epoch; 0 = the system clock).
// The callsign is 1 to 9 characters of A-Z 0-9 - / . (lower case is raised; empty is the demo identity ONAIR); grid is 6 characters (empty: AA00AA);
// text is the chat text (up to 100 characters), dest the recipient callsign (empty: CQ); lat / lon go into a full beacon.
// The profiles 6 and 7 (voice) need a speech codec that the user installs: none is loaded, so they are refused with that sentence.
// The generator is finite: finished() is true after the last burst. Nothing about mode, frequency or power is limited here.
#pragma once
#include "dect2/tx/tx.h"
#include <memory>
#include <string>
#include <vector>

namespace dect2 {

// nullptr and err (plain words) when the callsign, the profile, the options or the rate (below 12 kHz) cannot be used
std::unique_ptr<TxGenerator> makeKawsTxGenerator(const TxRequest& r, double rateHz, std::string& err);

// The bits of the message of a request, the callsign and the profile; false and err when they cannot be used.
struct KawsTxPlan {
    std::vector<uint8_t> message;   // one bit per byte
    std::string callsign;
    int profile = 3, kind = 0, passes = 2, msgId = 0;
    int blocks = 0, slotsPerPass = 0;
};
bool kawsFromRequest(const TxRequest& r, KawsTxPlan& plan, std::string& err);

// Airtime of the whole transmission in seconds from the first slot to the end of the last burst (the slots in between are counted, a wait for the
// first slot is not).
double kawsAirtimeSeconds(const KawsTxPlan& plan);

// 12 kHz real audio (the output of kaws::toAudio12k) as the complex upper-sideband signal: the Hilbert transform is the imaginary part.
std::vector<cf32> kawsAnalytic(const std::vector<float>& audio);

} // namespace dect2
