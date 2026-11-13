// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// KAAL as a transmit mode: the TX session's generator. Speech comes from TxRequest::pullAudio (48 kHz stereo: the microphone, a WAV/MP3 file, a tone
// or silence, chosen by the session), is brought to 16 kHz mono and goes to kaal::VoiceTx, whose IQ is made at the radio's rate.
// An empty callsign or grid is the demo identity ONAIR / AA00aa, like the other modes.
// Station identity (the beacon slice): TxRequest::callsign, grid, lat, lon, text (100 characters at most), opt "kaal.powerDbm" (-40 .. 87.5) and
// opt "kaal.beacon" (1 = send the beacon slice, 0 = voice only). Nothing about mode, frequency or power is limited here.
// opt "kaal.bitrate": the voice codec bitrate 12, 16 (default), 24 or 32 kbit/s. 12 and 16 are exactly real time (one 160-symbol frame per 20 ms
// of speech); 24 and 32 run at 16 ksym/s (about 20 kHz wide) and are real time too.
// The callsign (up to 6 characters of " A-Z0-9-/." plus one of the suffix classes /P /M ..., see kaal_sweepid.h) is also carried in the sweep;
// a longer callsign is only sent in the beacon.
// opt "kaal.frame": 3 (default) = the v3 stream above; 4 = frame version 4 (docs/KAAL-NEXT.md): 16 ksym/s, 20 kHz, four slots, chirp sync, the beacon and
// the text in the control channel and the data slots (any text length). opt "kaal.v4voice" (0 .. 4, default 1) slots carry the microphone with opt
// "kaal.v4profile" (1 = 8.0, 2 = 6.0, 3 = 4.8, 4 = 3.2 kbit/s), coded with KAAL voice codec 4 (kaal_slots.h); "kaal.v4robust" 1 = data as QPSK.
#pragma once
#include "dect2/tx/tx.h"
#include <memory>
#include <string>

namespace dect2 {

// nullptr and err (plain words) when the beacon fields are not usable or the rate is below 48 kHz
std::unique_ptr<TxGenerator> makeKaalTxGenerator(const TxRequest& r, double rateHz, std::string& err);

// The beacon fields of a request as VoiceTx wants them; false and err when the callsign (up to 9 of " 0-9A-Z/-", lower case is raised) or the
// grid (6 characters, e.g. KO26ab) cannot be sent.
bool kaalBeaconFromRequest(const TxRequest& r, std::string& callsign, std::string& grid, double& powerDbm, std::string& text, std::string& err);

} // namespace dect2
