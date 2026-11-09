// IEEE 802.15.4 / Zigbee frame layers, receive only (no decryption, no keys): CRC-16, the 2.4 GHz spreading table, the MAC header (frame
// type, security bit, PAN ID, short and extended addresses, sequence number, frame version), MAC commands, the Zigbee beacon payload, the
// Zigbee NWK header and the APS header where the NWK payload is not encrypted, and the vendor of an extended address (a small OUI table).
#pragma once
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace dect2 {
namespace zigbee {

// Channels 11 - 26: 2405 + 5 (k - 11) MHz, 2 MHz wide
constexpr int kFirstChannel = 11, kLastChannel = 26;
inline double channelMhz(int ch) { return 2405.0 + 5.0 * (ch - 11); }
inline int channelOf(double mhz) {   // the channel whose centre lies within 2.5 MHz, 0 when none
    const int k = (int)((mhz - 2405.0) / 5.0 + (mhz >= 2405.0 ? 0.5 : -0.5));
    const int ch = 11 + k;
    return ch >= kFirstChannel && ch <= kLastChannel && std::fabs(mhz - channelMhz(ch)) < 2.5 ? ch : 0;
}

// Sub-GHz BPSK channels (IEEE 802.15.4-2006 table 2): channel 0 is 868.3 MHz (Europe, 300 kchip/s, 20 kbit/s), channels 1 - 10 are
// 906 + 2 (k - 1) MHz (North America, Australia, 600 kchip/s, 40 kbit/s). They are coded 100 (channel 0) and 101 - 110 (channels 1 - 10) so
// that one number names every channel of this mode (11 - 26 stay as they are, 0 is "unknown").
constexpr int kSubChannelBase = 100;
inline bool isSubGhzChannel(int code) { return code >= kSubChannelBase && code <= kSubChannelBase + 10; }
inline double subChannelMhz(int code) { return code == kSubChannelBase ? 868.3 : 906.0 + 2.0 * (code - kSubChannelBase - 1); }
inline int subChannelOf(double mhz) {   // the sub-GHz channel code whose centre lies within 0.6 MHz (channel 0) or 1 MHz, 0 when none
    if (std::fabs(mhz - 868.3) < 0.6) return kSubChannelBase;
    for (int k = 1; k <= 10; k++) if (std::fabs(mhz - subChannelMhz(kSubChannelBase + k)) < 1.0) return kSubChannelBase + k;
    return 0;
}
inline int channelCodeOf(double mhz) { return mhz < 1000.0 ? subChannelOf(mhz) : channelOf(mhz); }
inline double channelCodeMhz(int code) { return isSubGhzChannel(code) ? subChannelMhz(code) : channelMhz(code); }
inline std::string channelCodeName(int code) {   // "11", "26", "868 ch 0", "915 ch 4", "" for unknown
    if (code >= kFirstChannel && code <= kLastChannel) return std::to_string(code);
    if (code == kSubChannelBase) return "868 ch 0";
    if (isSubGhzChannel(code)) return "915 ch " + std::to_string(code - kSubChannelBase);
    return std::string();
}

// FCS: ITU-T CRC-16 (x^16 + x^12 + x^5 + 1), initial value 0, bits least significant first (the "Kermit" CRC: "123456789" gives 0x2189)
uint16_t crc16(const uint8_t* d, size_t n);

// The 16 near-orthogonal 32-chip sequences of the 2.4 GHz O-QPSK PHY (IEEE 802.15.4-2006 table 24): chip c0 is the most significant bit
extern const uint32_t kChipTable[16];

// MAC frame types
enum FrameType { Beacon = 0, Data = 1, Ack = 2, Command = 3, Multipurpose = 5, Fragment = 6, Extended = 7 };
const char* frameTypeName(int t);

struct Frame {
    bool ok = false;                  // the header could be read
    int type = 0;
    bool secured = false, pending = false, ackReq = false, panCompress = false, seqSuppressed = false, ieList = false;
    int destMode = 0, srcMode = 0;    // 0 none, 2 short, 3 extended
    int version = 0;                  // 0 and 1: 2003 / 2006, 2: 2015
    int seq = -1;
    bool hasDestPan = false, hasSrcPan = false;
    uint16_t destPan = 0, srcPan = 0;
    uint16_t destShort = 0, srcShort = 0;
    uint64_t destExt = 0, srcExt = 0;   // as the usual EUI-64 (the air carries it little endian)
    size_t payloadAt = 0, payloadLen = 0;   // within the PSDU, FCS excluded (after the auxiliary security header, the MIC taken off)
    int secLevel = -1, secKeyMode = 0, secKeyIndex = -1;   // MAC security: level (4 and above encrypt), key identifier mode, key index (key mode 1: Thread)
    uint32_t frameCounter = 0;
    // MAC command
    int command = -1;
    // beacon
    bool superframe = false, panCoordinator = false, assocPermit = false, coordinator = false;
    int beaconOrder = 15, superframeOrder = 15;
    bool zbBeacon = false;
    int protocolId = -1, stackProfile = 0, nwkVersion = 0;
    bool routerCapacity = false, endDeviceCapacity = false;
    int depth = 0, nwkUpdateId = 0;
    uint64_t extPanId = 0;
    // Zigbee NWK
    bool hasNwk = false, nwkSecured = false;
    int nwkType = 0, nwkProtocol = 0, nwkRadius = 0, nwkSeq = 0, nwkCommand = -1;
    bool nwkRoute = false, nwkMulticast = false, nwkSourceRoute = false;
    uint16_t nwkDest = 0, nwkSrc = 0;
    bool nwkHasDestExt = false, nwkHasSrcExt = false;
    uint64_t nwkDestExt = 0, nwkSrcExt = 0;
    // NWK source route subframe: the relay list (short addresses) as carried, index of the next relay. The frame walks source, list[last] ... list[0], destination.
    int nwkRelayIndex = -1;
    std::vector<uint16_t> nwkRelays;
    // Zigbee APS (only when the NWK payload is in the clear)
    bool hasAps = false, apsSecured = false;
    int apsType = 0, apsDelivery = 0, apsDestEp = -1, apsSrcEp = -1, apsCounter = -1;
    int apsCluster = -1, apsProfile = -1;
    int nwkAuxAt = -1;                // offset of the NWK auxiliary security header in the NWK frame
    bool decrypted = false;           // decrypted with a user-imported key
    bool encrypted = false;           // the payload is encrypted (MAC or NWK or APS security): shown as such, never decoded
    std::string info;                 // one line: what the frame is
};

// Reads a PSDU (FCS included; fcs says whether it matched). The header is decoded as far as it goes.
Frame parse(const uint8_t* psdu, size_t n);
// With a network key the user imported: decrypts the NWK payload of a secured frame and reads its APS header. True on a MIC match. See core/src/voicekeys/net_crypto.h.
bool decryptNwk(const uint8_t* psdu, size_t n, Frame& f, const uint8_t key[16], std::vector<uint8_t>& plain);
std::string addrText(uint64_t ext);          // "00:12:4B:00:01:02:03:04"
std::string shortText(uint16_t a);           // "0x1A2B"
const char* macCommandName(int c);
const char* nwkCommandName(int c);
const char* clusterName(int cluster, int profile);
const char* profileName(int profile);
const char* vendorOfExt(uint64_t ext);       // "" when the OUI is not in the table
// "src > dst" in the frame log: the addresses as the frame carries them ("-" none)
std::string srcText(const Frame& f);
std::string dstText(const Frame& f);

// Builds frames (for the test signal and the tests): the MAC header and payload, FCS added
struct MacBuild {
    int type = Data;
    int version = 0;
    bool secured = false, ackReq = false, pending = false, panCompress = true;
    int destMode = 2, srcMode = 2;
    uint8_t seq = 0;
    uint16_t destPan = 0x1A62, srcPan = 0x1A62;
    uint16_t destShort = 0, srcShort = 0;
    uint64_t destExt = 0, srcExt = 0;
    std::vector<uint8_t> payload;
};
std::vector<uint8_t> buildMac(const MacBuild& b);
// A Zigbee beacon payload (15 bytes)
std::vector<uint8_t> zbBeaconPayload(int stackProfile, int nwkVersion, bool router, bool endDevice, int depth, uint64_t extPan, int updateId);
// A Zigbee NWK frame (header, optional extended addresses) in front of a payload
struct NwkBuild {
    int type = 0, protocol = 2, radius = 30, seq = 0;
    bool secured = false, route = true;
    uint16_t dest = 0, src = 0;
    bool destExt = false, srcExt = false;
    uint64_t destExtAddr = 0, srcExtAddr = 0;
    std::vector<uint16_t> relays;   // a source route subframe (relay list as carried); relayIndex is the index of the next relay
    int relayIndex = 0;
    std::vector<uint8_t> payload;
};
std::vector<uint8_t> buildNwk(const NwkBuild& b);

} // namespace zigbee
} // namespace dect2
