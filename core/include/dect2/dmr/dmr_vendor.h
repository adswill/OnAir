// DMR beyond the ETSI grants: Motorola Capacity Plus and Connect Plus CSBKs (layouts read from DSD-FME dmr_csbk.c, whose notes credit OP25 and Eric Cottrell;
// partly reverse-engineered by those projects), and the Motorola IP services carried as data: ARS (UDP port 4005) and text messages, TMS (UDP port 4007).
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace dect2 {
namespace dmr {

const char* vendorCsbkName(int fid, int opcode);    // "" when not one of these

// Capacity Plus channel status (FID 0x10, opcode 0x3E): the blocks of one transmission are accumulated per slot; on the last block the talkgroups on air are returned.
struct CapPlusGrant { int lsn = 0; int tg = 0; };   // logical slot number 1 .. 16 and its 8-bit group
class CapPlus {
public:
    // data: the 8 bytes after opcode and FID. True when this block completed a table: 'out' holds the groups on air, 'rest' the rest channel.
    bool feed(const uint8_t data[8], std::vector<CapPlusGrant>& out, int& rest);
private:
    uint8_t bits_[2][64] = {};
    int blocks_[2] = {0, 0};
};

// Connect Plus voice grant (0x03), data grant (0x06), slot termination (0x0C), neighbours (0x01). True when decoded.
struct ConPlus { int opcode = 0; uint32_t src = 0, dst = 0; int lcn = -1, slot = 0; int neighbours[5] = {}; };
bool conPlusParse(int opcode, const uint8_t data[8], ConPlus& out);

// A Motorola service in an IPv4 / UDP datagram. False when 'data' is not one. text: the radio name (ARS) or the message (TMS).
struct IpService { int port = 0; bool ack = false; std::string kind, text; };
bool ipService(const uint8_t* data, size_t n, IpService& out);

} // namespace dmr
} // namespace dect2
