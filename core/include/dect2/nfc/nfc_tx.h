// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Phase 2: the ACTIVE NFC READER transmitter (ISO/IEC 14443 Type A). It builds a continuous 13.56 MHz carrier modulated with
// 100% ASK modified-Miller reader frames (REQA, anticollision CL1 / CL2, SELECT, READ for Type 2, GET_VERSION). The carrier never
// drops between frames (only the short Miller pauses), as a real reader keeps the tag powered.
//
// GATING. This generator makes samples only; it transmits nothing by itself. Like every transmitter in OnAir it runs solely through
// the TxSession / TX panel gate: the operator must Arm, confirm the licence / notice, and the radio must be tuned inside the
// 13.553-13.567 MHz ISM band. A low amplitude cap is applied here as a courtesy power limit. In the tests the session uses the
// dry-run null sink (makeNullTxSink), so nothing is keyed. See docs/NFC.md for the antenna / loop and isolation guide and the limits.
#pragma once
#include "dect2/tx/tx.h"
#include <memory>
#include <string>

namespace dect2 {

// The ISM band the 13.56 MHz reader must stay inside (Hz).
constexpr double kNfcIsmLoHz = 13.553e6;
constexpr double kNfcIsmHiHz = 13.567e6;

// True when hz is inside the 13.56 MHz ISM band (the band gate the TX panel should enforce for this mode).
bool nfcTxFrequencyAllowed(double hz);

// The active-reader generator. rateHz must cover the 847.5 kHz subcarrier of any answer (at least ~2.4 Msps); err is set and nullptr
// returned when the rate is too low. The waveform is a continuous carrier with a repeating reader command script.
std::unique_ptr<TxGenerator> makeNfcReaderTxGenerator(const TxRequest& r, double rateHz, std::string& err);

}  // namespace dect2
