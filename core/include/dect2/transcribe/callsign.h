// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Callsigns in spoken text: "Lufthansa one two three alpha" > DLH123A, "november one two three alpha bravo" > N123AB, "Speedbird 4 7 2" > BAW472,
// "hotel bravo alpha lima charlie" > HBALC. Whisper writes numbers as digits or as words, so both are read. Pure text rules (no radio).
// The keys are normalised as acars_log.h does (upper case, no dashes, no leading zeros of the flight number), so they compare with the
// ADS-B callsign and registration directly.
#pragma once
#include <string>
#include <vector>

namespace dect2 {

struct CallsignHit {
    enum Kind { Flight, Reg } kind = Flight;
    std::string key;        // "DLH123", "N123AB", "HBALC"
    std::string spoken;     // what was read: "Lufthansa one two three alpha"
    size_t firstWord = 0;   // index of its first word in the text (words are runs of letters and digits)
    size_t lastWord = 0;    // and of its last one
    size_t nWords = 0;      // words in the text
};

// Every callsign in the text, in order of appearance. Telephony designators are the airlines' radio names (Lufthansa, Speedbird, Ryanair ...);
// a three letter ICAO designator followed by a number ("DLH 123") is read as it is.
std::vector<CallsignHit> findCallsigns(const std::string& text);

// The ICAO designator of a telephony name ("speedbird" > "BAW", "wizz air" > "WZZ"), "" when it is not in the table. Lower case input.
std::string telephonyToIcao(const std::string& name);
size_t telephonyCount();

// A short role guess for an aviation voice message: "ATC" when it starts with the aircraft's callsign (the controller calls the aircraft),
// "Aircraft" when it ends with it (a read-back signs off with it), "" otherwise.
std::string guessSpeaker(const std::vector<CallsignHit>& hits);

}  // namespace dect2
