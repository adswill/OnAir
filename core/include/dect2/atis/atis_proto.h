// ATIS, the Automatic Transmitter Identification System of the European inland waterways (Rhine, Danube, Rhone ...): ETSI EN 300 698-1 annex B,
// ITU-R Report M.1159. It is not AIS. A VHF radio sends it by itself after the operator lets go of the push-to-talk key, on the same channel as
// the voice: 1200 baud FSK, 1300 Hz = Y (1) and 2100 Hz = B (0) on a phase-modulated carrier, in the DSC code (ITU-R M.493, ten bits per symbol:
// seven information bits and the count of B elements). The sequence is a 20 bit dot pattern, the DSC phasing (6 x 125 in the DX places, 8 x 111 .. 104 in
// the RX places), format specifier 121 (twice), five symbols holding the ten digit identity, end of sequence 127, the check symbol (even parity of
// format, identity and end of sequence), every symbol sent again five places later. The time diversity, the symbols and the check are the DSC
// ones, so the DSC decoder (marine_dsc.h) finds and repairs the frame; this file reads and builds the contents.
//
// The identity is Z MID X1 X2 X3 X4 X5 X6: Z = 9, MID = the country digits (ITU Radio Regulations appendix 42), X1 X2 = the second letter of the
// call sign (01 = A), X3 .. X6 = the number of the call sign; the first letter of the call sign is the country. Many radios are programmed with
// 9 + the MMSI instead; the nine digits after the 9 are then an MMSI, which the ship name table can name.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace dect2 {
namespace atis {

constexpr int kFormatSpecifier = 121;
constexpr int kEndOfSequence = 127;

struct Id {
    bool valid = false;          // five symbols, each a pair of decimal digits
    std::string digits;          // ten digits, '~' for a digit that could not be read
    bool partial = false;        // some digit unreadable
    bool standardZ = false;      // starts with 9
    int mid = 0;                 // the three digits after the 9 (0 when unreadable)
    std::string country;         // "" when the MID is not allocated
    std::string mmsi;            // the nine digits after the 9 (the MMSI form some radios use)
    std::string callsign;        // the call sign the digits spell ("PC8075"), '?' for the letters that are not known: "?C8075"
    std::string firstLetterNote; // how sure the first letter is: "" sure, "likely" from the country, "unknown"
};

// The identity from the information symbols of a call: body[0] = format specifier, body[1..5] = the identity. Fails (valid = false) when the
// format is not 121 or fewer than five symbols arrived.
Id idFromSymbols(const std::vector<int>& body);
Id idFromDigits(const std::string& ten);                    // for tests and the UI
// "PC8075" and MID 244 -> "9244038075". "" when the call sign is not letter, letter, four digits (a one letter prefix call sign needs one of that country too).
std::string idFromCallsign(const std::string& callsign, int mid);
std::string idFromMmsi(const std::string& mmsi9);           // "207072225" -> "9207072225" ("" when not nine digits)
// The first letter(s) of the call signs of a country, "" when more than one is possible and nothing is known: 244 -> "P"
std::string callsignFirstLetter(int mid);

// The symbols (format specifier and five identity symbols) of an identity; empty when digits are not ten decimal digits
std::vector<int> bodyFromDigits(const std::string& ten);
// The bits of a whole ATIS burst, one entry per bit, 1 = Y (1300 Hz)
std::vector<uint8_t> frameBits(const std::string& ten, int dotBits = 20);
// The DX-place check symbol the standard worked out as an example: 92 44 03 80 75 -> 110
int checkSymbol(const std::string& ten);

} // namespace atis
} // namespace dect2
