// ACARS block layer of the Inmarsat Aero SU layer: the user data of one ISU (what the SUs of one ACARS block carry),
// its parity and block check, and builders for the test signal.
// Layout (ARINC 618 block as the P channel carries it, per the comment in JAERO's ParserISU::parse, aerol.cpp):
//   FF FF | SOH | mode | registration (7) | TAK | label (2) | block id | STX text... | ETX or ETB | BCS lo, hi | DEL
// Every byte from the mode to the ETX/ETB has odd parity (bit 7 is the parity bit); the BCS is CRC-16/KERMIT
// (reflected 0x1021, init 0) over the raw bytes from the mode to the ETX/ETB.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace dect2 {

uint16_t aeroAcarsCrc(const uint8_t* p, size_t n);          // CRC-16/KERMIT, check value of "123456789" is 0x2189
uint8_t aeroAddOddParity(uint8_t c);                        // c & 0x7F with bit 7 set so the byte has an odd number of ones
std::string aeroAcarsLabelText(const std::string& label);   // "" when the label is not known

struct AeroAcarsBlock {
    std::string mode;                   // one character
    std::string registration;           // as sent: 7 characters, padded with '.' in front
    uint8_t tak = 0x15;                 // technical acknowledgement character (0x15 = NAK = none)
    std::string label;                  // two characters; a DEL as second character shows as 'd'
    uint8_t blockId = 0;
    std::string text;                   // 7-bit text, DEL shown as "<DEL>"
    bool hasText = false;
    bool moreToCome = false;            // ended with ETB: the message continues in the next block
    bool parityOk = false;
    bool crcOk = false;
};

// Parses the user data of one ISU as an ACARS block. False when it does not have the block's shape.
bool aeroParseAcarsBlock(const std::vector<uint8_t>& userData, AeroAcarsBlock& out);

// Builds the user data of one block (the exact bytes after the ISU's own framing). `text` up to 220 characters.
std::vector<uint8_t> aeroBuildAcarsBlock(const AeroAcarsBlock& b);

} // namespace dect2
