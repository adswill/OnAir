// Inmarsat-C TDM packets: parsing of a decoded 640 byte frame into system information and EGC messages, and the matching builders
// that the test signal generator uses.
//
// Sources (facts only): packet descriptors, lengths, field positions, the check bytes, service and address codes, satellite and station
// tables from the open decoder "inmarsatc" (inmarsatc_parser.cpp) and cross-checked against stdcdec (stdc_decode.c); the bulletin board
// packet of the NCS carries the frame number (times 8.64 s) and the channel type. What is NOT confirmed against a specification:
//  - the layout of the EGC address bytes (area numbers, positions): they are shown as hex, not decoded;
//  - the meaning of the first address byte (read here as satellite and station, like the other packets): marked tentative;
//  - how the second EGC descriptor (0xB2) numbers its packets: parts are ordered by packet number, then 0xB1 before 0xB2.
#pragma once
#include "inmc_code.h"
#include "inmc_tel.h"
#include <cstdint>
#include <string>
#include <vector>

namespace dect2 {
namespace inmc {

std::string satName(int sat);
std::string lesName(int sat, int les);          // "" when not in the table
std::string serviceName(int code);
int serviceKind(int code);                      // 0 other or system, 1 SafetyNET, 2 FleetNET
const char* priorityName(int p);
int addressLength(int code);                    // bytes of address after the header, the first one included
const char* descriptorName(uint8_t d);
std::string servicesText(unsigned mask);
std::string frameTimeText(uint32_t frameNo);
std::string ita2ToText(const std::vector<uint8_t>& codes);
std::vector<uint8_t> textToIta2(const std::string& text);   // for the test signal
std::string decodeText(int presentation, const std::vector<uint8_t>& bytes);
std::string utcText(int64_t unixSec);

// ---- builders (each returns the packet with its check bytes) ----
struct BulletinBoard {
    int networkVersion = 1;
    uint16_t frameNo = 0;
    int signallingChannel = 0;
    int count = 0;               // coded in the high nibble as count / 2
    int channelType = 1;         // 1 NCS
    int local = 0;
    int sat = 3, les = 44;       // IOR, the NCS
    uint8_t status = 0x70;       // operational, in service, clear
    uint16_t services = 0xE001;
    int randomInterval = 8;
};
std::vector<uint8_t> buildBulletinBoard(const BulletinBoard& b);
std::vector<uint8_t> buildSignalling(uint8_t servicesByte, double uplinkMhz, const uint8_t slots28[28]);

struct EgcPacket {
    uint8_t desc = 0xB1;
    uint8_t service = 0x31;
    bool continuation = false;
    int priority = 1;
    int repetition = 0;
    uint16_t msgId = 0;
    int packetNo = 1;
    int presentation = 0;
    std::vector<uint8_t> address;     // exactly addressLength(service) bytes
    std::vector<uint8_t> payload;
};
std::vector<uint8_t> buildEgc(const EgcPacket& p);

// newest first (by InmcMessage::order), at most maxCount, and the text of older ones cut so that the report stays under about 100 kB
void trimMessages(std::vector<InmcMessage>& v, size_t maxCount, size_t budget);

// ---- parser ----
class FrameParser {
public:
    FrameParser();
    void reset();
    // Parses one decoded frame (640 bytes). Returns true when the frame starts with a bulletin board packet with a good check.
    bool parseFrame(const uint8_t* frame, int64_t rxUnix);
    // Copies what the parser knows into the telemetry (ncs, messages, packet counts); the channel members are the caller's.
    void fill(InmcTelemetry& t) const;
    uint32_t lastFrameNo() const { return frameNo_; }
    bool haveBulletinBoard() const { return ncs_.valid; }

private:
    struct Slot {
        InmcMessage msg;
        std::vector<std::pair<int, std::vector<uint8_t>>> parts;   // key = packetNo * 2 + (descriptor == 0xB2)
        int lastKey = -1;
        uint64_t order = 0;
    };
    void egc(const uint8_t* p, int len, int64_t rxUnix);
    void finish(Slot& s);
    InmcNcsInfo ncs_;
    std::vector<Slot> slots_;
    uint32_t pktOk_[256], pktBad_[256];
    uint64_t packetsOk_ = 0, packetsBad_ = 0;
    uint32_t messageCount_ = 0, frameNo_ = 0;
    std::vector<uint8_t> mfp_;       // multiframe packet being collected (0xBD, 0xBE)
    size_t mfpTotal_ = 0;
    uint32_t curFrame_ = 0;
};

} // namespace inmc
} // namespace dect2
