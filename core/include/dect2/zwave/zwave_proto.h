// Z-Wave frame layer (ITU-T G.9959 MAC, clean-room from the public specification): the regional channel tables, the MPDU layout of R1 / R2 / R3,
// the checksum (R1, R2: XOR) and CRC-16 (R3), and the names of command classes and commands. Receive only: S0 and S2 payloads are named as
// encrypted and never decoded.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace dect2 {
namespace zwave {

// The three FSK data rates: R1 9.6 kbit/s Manchester, R2 40 kbit/s NRZ, R3 100 kbit/s NRZ (GFSK)
constexpr int kRates = 3;
int bitRate(int rate);           // 1..3 -> 9600 / 40000 / 100000
int chipRate(int rate);          // symbols on the air: 19200 for R1 (Manchester), else the bit rate
const char* rateName(int rate);  // "R1 9.6k"
uint8_t sofByte(int rate);       // start-of-frame delimiter: 0xF0 (R1, R2), 0x5B (R3)
int maxLength(int rate);         // largest MPDU: 64 (R1, R2), 170 (R3)
int checkOctets(int rate);       // 1 (checksum) or 2 (CRC-16)

struct Chan { double mhz; int rate; };
struct Region { const char* id; const char* name; double centreMhz; std::vector<Chan> chans; };
const std::vector<Region>& regions();
const Region* region(const std::string& id);

uint8_t checksum8(const uint8_t* d, size_t n);   // XOR of the octets, seed 0xFF
uint16_t crc16(const uint8_t* d, size_t n);      // CRC-16 CCITT, polynomial 0x1021, seed 0x1D0F

enum HeaderType { kHdrSinglecast = 1, kHdrMulticast = 2, kHdrAck = 3, kHdrRouted = 8 };

struct Frame {
    uint32_t homeId = 0;
    int src = 0;
    int fc1 = 0, fc2 = 0;
    int len = 0;               // the Length field: the whole MPDU with its checksum
    int dst = 0;               // singlecast destination (255: broadcast); multicast: -1
    int headerType = 1;
    bool routed = false, ackReq = false, lowPower = false, speedMod = false;
    int beam = 0, seq = 0;
    std::vector<int> group;    // multicast destinations
    std::vector<uint8_t> payload;
    bool checkOk = false;
};

// S0 encapsulation with a network key the user imported. S0 uses a nonce the receiver sent earlier in a NONCE_REPORT (0x98 0x80), so this keeps the
// recent reports per node pair. Returns true when a 0x98 0x81 frame was authenticated and decrypted (plain: the payload after the sequence byte).
class S0Session {
public:
    void observe(const Frame& f);          // every frame: remembers nonce reports
    bool decrypt(const Frame& f, const uint8_t key[16], std::vector<uint8_t>& plain) const;
private:
    struct Rep { int src = -1, dst = -1; uint8_t nonce[8] = {}; };
    std::vector<Rep> rep_;
};

// The route header at the start of a routed frame's payload (clean-room from the public descriptions of the routed frame, checked by loopback
// only): octet 0 low nibble = type (0 routed frame, 1 routed ack, 2 routed error), octet 1 = repeater count (high nibble, at most 4) and the
// index of the hop this copy is on (low nibble: 0 the source is sending, count: the last repeater is sending), then the repeater node numbers
// in order from the source to the destination.
struct Route {
    bool valid = false;
    int type = 0, count = 0, hop = 0;
    std::vector<int> repeaters;
    size_t size = 0;           // octets the header takes
};
bool parseRoute(const std::vector<uint8_t>& payload, Route& r);
std::vector<uint8_t> buildRoute(int type, int hop, const std::vector<int>& repeaters);
// The nodes a routed frame walks through: source, repeaters, destination (empty when the frame is not routed or its route header is bad)
std::vector<int> routePath(const Frame& f, const Route& r);

// MPDU octets with the check octets at the end (n = Length): fills f; false when too short or the Length disagrees. f.checkOk tells the check.
bool parse(const uint8_t* d, size_t n, int rate, Frame& f);
// The MPDU for a frame (Length and the checksum / CRC added)
std::vector<uint8_t> build(const Frame& f, int rate);

const char* headerName(int type);
const char* ccName(int cc);                    // "SWITCH_BINARY", "" unknown
std::string cmdName(int cc, int cmd);          // "Set", "Report", "" unknown
// 0 not encrypted, 1 S0 encapsulation, 2 S2 encapsulation (what the payload carries is never decoded)
int encryption(const std::vector<uint8_t>& payload);
// One line for the payload: "SWITCH_BINARY Set on", "Security S0 encrypted (n octets)", "NOP"
std::string describe(const Frame& f);
std::string nodeText(int id);                  // "5", "all" for 255
std::string homeText(uint32_t id);             // "C0FFEE01"
std::string hex(const uint8_t* d, size_t n);

} // namespace zwave
} // namespace dect2
