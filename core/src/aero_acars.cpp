// ACARS block parsing and building for the Aero SU layer. Layout and parity rule: JAERO aerol.cpp, ParserISU::parse
// (the example bytes in its comment are a real captured block and are used as a known-answer test).
// Block check: CRC-16/KERMIT as in acarsdec's update_crc, verified against that same example (BCS 93 AB).
#include "dect2/aero_acars.h"

#include <cstring>

namespace dect2 {

namespace {
const uint8_t kSoh = 0x01, kStx = 0x02, kEtx = 0x03, kEtb = 0x17, kDel = 0x7F;

bool oddParity(uint8_t c) {
    c ^= c >> 4; c ^= c >> 2; c ^= c >> 1;
    return (c & 1) != 0;
}
char printable(uint8_t c) { return (c >= 0x20 && c < 0x7F) ? char(c) : '?'; }
} // namespace

uint16_t aeroAcarsCrc(const uint8_t* p, size_t n) {
    uint16_t crc = 0;
    for (size_t i = 0; i < n; i++) {
        crc ^= p[i];
        for (int k = 0; k < 8; k++) crc = (crc & 1) ? uint16_t((crc >> 1) ^ 0x8408) : uint16_t(crc >> 1);
    }
    return crc;
}

uint8_t aeroAddOddParity(uint8_t c) {
    c &= 0x7F;
    return oddParity(c) ? c : uint8_t(c | 0x80);
}

std::string aeroAcarsLabelText(const std::string& label) {
    // _d, Q0, SQ and :; per the Wavecom ACARS decoder help; H1, 5Z, SA, A6, B6 per ARINC 620/622 usage as seen in
    // JAERO and airframes.io output. Unknown labels give "".
    if (label == "_d") return "General response";
    if (label == "Q0") return "Link test";
    if (label == "SQ") return "Ground station squitter";
    if (label == ":;") return "Data transceiver auto tune";
    if (label == "H1") return "Message to/from terminal";
    if (label == "5Z") return "Airline designated message";
    if (label == "SA") return "Media advisory";
    if (label == "A6") return "ADS-C uplink";
    if (label == "B6") return "ADS-C downlink";
    return "";
}

bool aeroParseAcarsBlock(const std::vector<uint8_t>& u, AeroAcarsBlock& out) {
    out = AeroAcarsBlock();
    // shortest block: header to STX/ETX at 15, BCS, DEL
    if (u.size() < 19) return false;
    if (u[0] != 0xFF || u[1] != 0xFF) return false;
    if (u[2] != kSoh) return false;
    const uint8_t at15 = u[15];
    if (at15 != kStx && at15 != 0x83 && at15 != 0x97) return false;
    const size_t n = u.size();
    const uint8_t endc = u[n - 4];
    const uint8_t endcLow = endc & 0x7F;
    if (endcLow != kEtx && endcLow != kEtb) return false;
    if (at15 != kStx && n != 19) return false;                   // no text: ETX sits right after the block id
    out.hasText = (at15 == kStx);
    out.moreToCome = (endcLow == kEtb);

    out.parityOk = true;
    for (size_t i = 3; i <= n - 4; i++)
        if (!oddParity(u[i])) out.parityOk = false;
    const uint16_t bcs = uint16_t(u[n - 3] | (u[n - 2] << 8));
    out.crcOk = aeroAcarsCrc(&u[3], n - 3 - 3) == bcs;           // bytes 3 .. n-4 inclusive

    out.mode.assign(1, printable(u[3] & 0x7F));
    for (int i = 4; i < 11; i++) out.registration += printable(u[i] & 0x7F);
    out.tak = u[11] & 0x7F;
    for (int i = 12; i < 14; i++) {
        const uint8_t c = u[i] & 0x7F;
        out.label += (c == 0x7F) ? 'd' : printable(c);
    }
    out.blockId = u[14] & 0x7F;
    if (out.hasText) {
        for (size_t i = 16; i + 4 < n; i++) {
            const uint8_t c = u[i] & 0x7F;
            if (c == kDel) out.text += "<DEL>";
            else if (c >= 0x20 || c == '\r' || c == '\n') out.text += char(c);
            else out.text += '?';
        }
    }
    return true;
}

std::vector<uint8_t> aeroBuildAcarsBlock(const AeroAcarsBlock& b) {
    std::vector<uint8_t> u;
    u.push_back(0xFF);
    u.push_back(0xFF);
    u.push_back(kSoh);
    const size_t crcStart = u.size();
    u.push_back(aeroAddOddParity(b.mode.empty() ? '2' : uint8_t(b.mode[0])));
    // registration: 7 characters, '.' fill in front
    std::string reg = b.registration;
    if (reg.size() > 7) reg.resize(7);
    reg.insert(0, 7 - reg.size(), '.');
    for (char c : reg) u.push_back(aeroAddOddParity(uint8_t(c)));
    u.push_back(aeroAddOddParity(b.tak));
    std::string lab = b.label;
    lab.resize(2, ' ');
    u.push_back(aeroAddOddParity(uint8_t(lab[0])));
    u.push_back(aeroAddOddParity(lab[0] == '_' && lab[1] == 'd' ? kDel : uint8_t(lab[1])));
    u.push_back(aeroAddOddParity(b.blockId ? b.blockId : '0'));
    const uint8_t endc = aeroAddOddParity(b.moreToCome ? kEtb : kEtx);
    if (b.hasText || !b.text.empty()) {
        u.push_back(aeroAddOddParity(kStx));
        size_t n = b.text.size() > 220 ? 220 : b.text.size();
        for (size_t i = 0; i < n; i++) u.push_back(aeroAddOddParity(uint8_t(b.text[i])));
    }
    u.push_back(endc);
    const uint16_t bcs = aeroAcarsCrc(&u[crcStart], u.size() - crcStart);
    u.push_back(uint8_t(bcs & 0xFF));
    u.push_back(uint8_t(bcs >> 8));
    u.push_back(kDel);
    return u;
}

} // namespace dect2
