// LoRaWAN regional channel plans (LoRa Alliance RP002-1.0.x "Regional Parameters": EU863-870, US902-928, AU915-928, AS923, EU433) as the lists of
// 125 / 250 / 500 kHz channels the monitor searches, and the MAC layer of a received PHY payload (TS001 L2 1.0.4 chapters 4 and 5, 1.1
// additions): MHDR, DevAddr, FCtrl, FCnt, FOpts MAC commands, FPort, MIC, join-request, join-accept and rejoin-request framing.
// Nothing is decrypted and no key is handled: payloads are shown as bytes, the MIC is shown and never checked.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace dect2 {
namespace lorawan {

enum Plan { EU868 = 0, US915, AU915, AS923_1, AS923_2, AS923_3, AS923_4, EU433, kPlans };

struct PlanChannel {
    double mhz = 0;
    double bwKhz = 125;
    bool uplink = true, downlink = true;   // which way the plan uses it (a gateway answers on the uplink channels in EU / AS / the RX1 window, on its own 500 kHz block in US / AU)
    bool dflt = false;                     // one of the channels every device must know (join channels of the plan)
    int subBand = 0;                       // US915 / AU915: 1..8 (eight channels and the 500 kHz channel that goes with them); 0 none
};

const char* planName(int plan);            // "EU868", "US915", ...
const char* planNote(int plan);            // one line: where, which channels
// All channels of a plan, ascending
std::vector<PlanChannel> planChannels(int plan);
// The channels to search for the given settings: sub-band 0 = all; includeDownlinkOnly drops the channels only gateways use (US / AU 500 kHz block)
std::vector<PlanChannel> selectChannels(int plan, int subBand, bool downlinks);
// The spreading factors a plan's data rates use on a bandwidth (DR table of RP002): 125 kHz SF7 - SF12 (US / AU: SF7 - SF10 uplink, SF7 - SF12 on downlinks), 250 kHz SF7, 500 kHz SF8 (up) / SF7 - SF12 (down)
void planSfRange(int plan, double bwKhz, bool downlink, int& sfMin, int& sfMax);
// FSK (DR7: 50 kbit/s GFSK, lorawan_fsk.h): whether the plan's data rate table has it (RP002: EU863-870, EU433 and AS923 do, US902-928 and
// AU915-928 do not). RP002 gives it no default channel: a gateway runs one FSK channel of the network's choosing.
bool planHasFsk(int plan);
// The FSK channel(s) networks use: EU868 868.8 MHz and AS923-1 924.8 MHz, as in The Things Network's frequency plans (EU_863_870, AS_923_925);
// empty for the other plans (no common channel)
std::vector<double> planFskChannelsMhz(int plan);

// ---- MAC layer ----
struct MacCommand {
    uint8_t cid = 0;
    std::string name;                      // "LinkCheckReq", "DevStatusAns", "CID 0x42"
    std::string detail;                    // short values ("battery 79 %, margin 7 dB"), may be empty
    std::vector<uint8_t> data;
};

struct Frame {
    bool ok = false;                       // parsed as a LoRaWAN PHY payload
    std::string why;                       // when not ok
    int mtype = -1;                        // 0 join request, 1 join accept, 2 unconfirmed up, 3 unconfirmed down, 4 confirmed up, 5 confirmed down, 6 rejoin request, 7 proprietary
    int major = 0;
    bool uplink = true, confirmed = false;
    // data frames
    uint32_t devAddr = 0;
    int addrType = -1;                     // DevAddr type 0..7 (leading ones), TS002 1.0.0 table
    uint32_t nwkId = 0;
    bool adr = false, adrAckReq = false, ack = false, classB = false, fpending = false;
    int foptsLen = 0;
    uint16_t fcnt = 0;                     // the 16 bits on the air (32-bit counters are not known without the session)
    int fport = -1;                        // -1: no port (no FRMPayload)
    size_t frmLen = 0;                     // encrypted payload bytes, never decrypted
    std::vector<MacCommand> mac;           // commands in FOpts (FPort 0 carries them encrypted in the payload: macInPayload)
    bool macInPayload = false;
    uint32_t mic = 0;                      // the 4 bytes as sent, first byte in the high bits
    // join / rejoin
    uint64_t joinEui = 0, devEui = 0;
    uint16_t devNonce = 0;                 // rejoin: RJcount
    int rejoinType = -1;
    uint32_t netId = 0;                    // rejoin type 0 / 2
    bool joinAcceptCfList = false;         // join-accept of 33 bytes (with a channel list)
    std::string flags;                     // "ADR, ACK"
    std::string summary;                   // one line for the table
};

const char* mtypeName(int mtype);
std::string macCommandName(uint8_t cid, bool uplink);
std::string eui(uint64_t v);               // "70B3D57ED0012345", most significant byte first
std::string hex(const uint8_t* p, size_t n);
// Parses a PHY payload. downlinkHint: the frame came on inverted IQ (a gateway), which settles the direction of MType 2 / 3 mix-ups only for display
Frame parse(const uint8_t* p, size_t n);
// The frame as bytes (for the test signal)
struct Build {
    int mtype = 2;
    uint32_t devAddr = 0;
    bool adr = false, adrAckReq = false, ack = false, fpending = false;
    uint16_t fcnt = 0;
    std::vector<uint8_t> fopts;
    int fport = -1;
    std::vector<uint8_t> frm;
    uint32_t mic = 0;
};
std::vector<uint8_t> build(const Build& b);
std::vector<uint8_t> buildJoinRequest(uint64_t joinEui, uint64_t devEui, uint16_t devNonce, uint32_t mic);
std::vector<uint8_t> buildJoinAccept(size_t totalLen, uint8_t seed);   // MHDR + opaque bytes (the join-accept is sent encrypted): 17 or 33 bytes

} // namespace lorawan
} // namespace dect2
