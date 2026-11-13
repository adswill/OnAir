// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// KAFT as a transmit mode: the TX session's generator. The file (TxRequest::file, up to 256 MiB; empty is a short demo text file) is sent one-way: no
// reply is waited for. Each pass is OPEN, START, the data bursts (OPEN and START again before every 8th burst, so that a receiver can join in the middle),
// FINISH; the whole file is sent `kaft.repeat` times, so a receiver that missed blocks gets them on the next pass, and one that has the file ignores
// the repeat. The file is sent raw (no compression codec is built in) at one fixed profile (there is no return link for the ladder to follow).
// Options: opt "kaft.profile" (0 .. 9, default 2 = QPSK 1/2), opt "kaft.width" (0 = 1 MHz, 1 = 500 kHz, 2 = 200 kHz, default 2), opt "kaft.repeat"
// (1 .. 9 passes, default 2), opt "kaft.session" (1 .. 65000, default: from the clock; a receiver tells two transfers of one file apart by it). The callsign is up to 6 characters of A-Z 0-9 / - . (lower case is raised); empty is the demo identity ONAIR.
// The rate must carry the width: at least 1.25 Msps for 1 MHz, 625 ksps for 500 kHz, 250 ksps for 200 kHz.
// The generator is finite: finished() is true after the last pass and a session ends then. Nothing about mode, frequency or power is limited here.
#pragma once
#include "dect2/tx/tx.h"
#include <memory>
#include <string>
#include <vector>

namespace dect2 {

// nullptr and err (plain words) when the callsign, profile, width, file or rate cannot be used
std::unique_ptr<TxGenerator> makeKaftTxGenerator(const TxRequest& r, double rateHz, std::string& err);

// The file name (without path), bytes, callsign and options of a request; false and err when they cannot be used.
bool kaftFromRequest(const TxRequest& r, std::string& fileName, std::vector<uint8_t>& data, std::string& callsign, int& profile, int& width, int& passes, std::string& err);

// The lowest sample rate that carries a width (Hz)
double kaftMinRateHz(int width);

// About how long the whole transmission takes, in seconds (control frames, data frames, gaps; all passes)
double kaftAirtimeSeconds(size_t fileBytes, int profile, int width, int passes);

} // namespace dect2
