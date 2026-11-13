// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// KAPP as a transmit mode: the TX session's generator. The message (TxRequest::text, or the bytes of TxRequest::file when one is chosen, up to 8 MiB) is cut
// into the selective-repeat blocks of kapp_arq.h and sent as bursts at one fixed profile. The transmission is one-way: no ACK is waited for, so the whole
// message is sent `kapp.repeat` times (a receiver that missed a block gets it on the next pass; one that has the message already ignores the repeat).
// Options: opt "kapp.profile" (0 .. 8, default 2 = QPSK 1/2), opt "kapp.repeat" (1 .. 9 passes, default 2). The callsign is up to 7 characters of
// A-Z 0-9 and "/" (lower case is raised); empty is the demo identity ONAIR. An empty message is a short demo text.
// The generator is finite: finished() is true after the last pass, and a session ends then. Nothing about mode, frequency or power is limited here.
#pragma once
#include "dect2/tx/tx.h"
#include <memory>
#include <string>
#include <vector>

namespace dect2 {

// nullptr and err (plain words) when the callsign, the profile, the file or the rate (below 125 kHz) cannot be used
std::unique_ptr<TxGenerator> makeKappTxGenerator(const TxRequest& r, double rateHz, std::string& err);

// The message, callsign, profile and passes of a request; false and err when they cannot be used.
bool kappFromRequest(const TxRequest& r, std::vector<uint8_t>& msg, std::string& callsign, int& profile, int& passes, std::string& err);

// Airtime of the whole transmission in seconds (ramps, preambles and headers included, gaps between bursts included)
double kappAirtimeSeconds(size_t messageBytes, int profile, int passes);

} // namespace dect2
