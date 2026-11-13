// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// KAIT as a transmit mode: the TX session's generator. The picture is TxRequest::file (a PNG or PPM file; empty: the built-in demo picture), shrunk to fit 1920x1080
// and coded to the layers of the profile TxRequest::variant (0 balanced, 1 robust, 2 quality, 3 direct); the burst is TxRequest::callsign (up to 9 characters of
// A-Z 0-9 / - .; empty: ONAIR). It sends one 10 s burst, then 2 s of silence (opt "kait.gap" 0.5 .. 120 s changes the silence), and repeats while the session loops. Peak amplitude 0.9 at the output.
#pragma once
#include "dect2/tx/tx.h"
#include <memory>
#include <string>

namespace dect2 {

// nullptr and err (plain words) when the callsign, the picture or the rate (below 400 ksps) is not usable
std::unique_ptr<TxGenerator> makeKaitTxGenerator(const TxRequest& r, double rateHz, std::string& err);

} // namespace dect2
