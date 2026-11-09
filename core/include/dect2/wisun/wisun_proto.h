// Wi-SUN FAN (Field Area Network: smart meters, street lights) over the IEEE 802.15.4g SUN FSK PHY, receive only (no keys, no decryption):
// the PHY header, PN9 data whitening and the 2- and 4-octet FCS; the 802.15.4-2015 MAC header (frame version 2, PAN ID rules, the auxiliary
// security header); the Wi-SUN header IEs (UTT, BT, FC, RSL, EA, LBC, ...) and, where the frame is not secured (PAN advertisements and their
// solicits, EAPOL), the Wi-SUN payload IEs (unicast and broadcast schedules, PAN, network name, PAN version, PHY operating modes) and the MPX IE
// with an EAPOL / EAP summary. Secured frames (PAN configuration, data) carry their payload IEs and payload encrypted: shown as such.
// The channel plans of the 902 - 928 MHz regions (Australia / New Zealand and North America).
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace dect2 {
namespace wisun {

// ---------------------------------------------------------------- PHY (802.15.4g SUN FSK)

// Start-of-frame delimiters (phyMRFSKSFD = 0), sent most significant bit first after the 0101... preamble
constexpr uint16_t kSfdUncoded = 0x904E, kSfdCoded = 0x6F4E;

// The 2-octet PHR, sent bit 15 first: mode switch, two reserved bits, FCS type (1: 2-octet FCS), data whitening, an 11-bit PSDU length (FCS included)
struct Phr {
    bool modeSwitch = false;
    bool fcs2 = false;
    bool whitened = true;
    int length = 0;
};
Phr parsePhr(uint16_t v);
uint16_t buildPhr(const Phr& p);

// PN9 data whitening of the PSDU (x^9 + x^5 + 1, all ones at the start of the PSDU), applied to the bits as sent (each octet least significant
// bit first): self-inverse. The sequence starts f0 0e cd f6 c2 19 12 75 as octets.
void whiten(uint8_t* d, size_t n);

// The 4-octet FCS: the 802.3 CRC-32 ("123456789" gives 0xCBF43926), stored least significant octet first
uint32_t crc32(const uint8_t* d, size_t n);
// The 2-octet FCS: ITU-T CRC-16, as the 2.4 GHz PHY's (zigbee::crc16), stored least significant octet first
uint16_t crc16(const uint8_t* d, size_t n);
// psdu: n octets with the FCS at the end
bool fcsOk(const uint8_t* psdu, size_t n, bool fcs2);
// Appends the FCS
void addFcs(std::vector<uint8_t>& mpdu, bool fcs2);

// ---------------------------------------------------------------- channel plans

// A band plan: one region's band and regulatory domain (the Wi-SUN frequency band table: band, region, regulatory domain value, channel
// spacings and the number of channels at each), with the channel plans it allows
struct BandPlan {
    const char* id;            // "ANZ", "NA", "EU863", "JP", ...
    const char* name;          // "Australia / New Zealand"
    int regDomain;             // the Wi-SUN regulatory domain value (0x08)
    double loMhz, hiMhz;       // the band
    double gapLoMhz, gapHiMhz; // a part of the band without channels (Brazil: 907.5 - 915 MHz), 0 none
};
const std::vector<BandPlan>& bandPlans();
const BandPlan* bandPlan(const std::string& id);    // nullptr: no such plan
// The plan a receiver tuned here most likely wants (the user can pick another): ANZ from 914.5 MHz up to 929, North America below,
// Europe at 863 - 876, China at 470 - 510 and 779 - 787, the worldwide 2.4 GHz band
const BandPlan& bandPlanFor(double mhz);

struct PhyRate {
    int bitRate = 0;           // bit/s, 0 none
    double modIndex = 0.5;
};

// One channel spacing of a band plan. The first channel and the FSK PHY modes are not in the band table: ch0 follows the Wi-SUN PHY
// specification where it is known (902.2 / 902.4 / 902.6 MHz in North America, 915.2 / 915.4 in ANZ, 920.6 / 920.9 / 920.8 in Japan, ...)
// and the band's edge plus one spacing elsewhere; the rates are the usual FSK modes of each spacing (100 kHz: 50 kbit/s, h 0.5; 200 kHz:
// 50 kbit/s, h 1, or 100 kbit/s, h 0.5 in the 100 / 200 kHz bands of Europe and India; 400 kHz: 100 and 150 kbit/s, h 0.5; 600 kHz:
// 200 and 300 kbit/s, which this receiver does not demodulate).
struct ChannelPlan {
    const char* region;        // the band plan's id
    int opClass;               // 1, 2, 3 in the order of the band table's spacings (the regulatory operating class where the two agree)
    double ch0Mhz;
    double spacingKhz;
    int channels;
    PhyRate rates[2];          // the FSK PHY modes the plan carries
};
const std::vector<ChannelPlan>& channelPlans();
// The channel plans of a band plan
std::vector<ChannelPlan> plansOf(const std::string& region);
// The nearest channel of the plan to a frequency (-1 when outside it by more than half a spacing, or in the band plan's gap)
int channelOf(const ChannelPlan& p, double mhz);
inline double channelMhz(const ChannelPlan& p, int ch) { return p.ch0Mhz + ch * p.spacingKhz / 1000.0; }
// The channel exists: inside the plan and not in the band plan's gap
bool channelUsable(const ChannelPlan& p, int ch);

const char* regDomainName(int code);   // Wi-SUN regulatory domain codes (0x08: Australia / New Zealand)

// ---------------------------------------------------------------- MAC and Wi-SUN IEs

enum FrameType { Beacon = 0, Data = 1, Ack = 2, Command = 3, Multipurpose = 5, Fragment = 6, Extended = 7 };
const char* frameTypeName(int t);

// Wi-SUN frame types (the UTT-IE)
enum Utt { PA = 0, PAS = 1, PC = 2, PCS = 3, DataFrame = 4, AckFrame = 5, Eapol = 6, LPA = 7, LPAS = 8, LPC = 9, LPCS = 10, ExtData = 11 };
const char* uttName(int t);          // "PAN Advert", ...
const char* uttShort(int t);         // "PA", "PAS", ...
constexpr int kUttTypes = 12;

// A unicast (US-IE) or broadcast (BS-IE) schedule
struct Schedule {
    bool present = false;
    uint32_t bcastIntervalMs = 0;     // BS-IE only
    int bsi = -1;                     // BS-IE: broadcast schedule identifier
    int dwellMs = 0;
    int clockDrift = 0, timingAccuracy = 0;
    int chPlan = 0;                   // 0 regulatory domain and operating class, 1 explicit, 2 regulatory domain and channel plan ID
    int chFunction = 0;               // 0 fixed, 1 TR51CF, 2 DH1CF, 3 vendor
    int excluded = 0;                 // 0 none, 1 ranges, 2 bit mask
    int regDomain = -1, opClass = -1, chPlanId = -1;
    double ch0Mhz = 0;                // explicit plan
    int spacingCode = -1;             // explicit plan: 0 200 kHz, 1 400 kHz, 2 600 kHz, 3 100 kHz
    int numChannels = 0;              // explicit plan
    int fixedChannel = -1;            // fixed channel function
    int excludedCount = 0;            // channels excluded (ranges: their size; mask: bits set)
};
const char* chFunctionName(int f);
std::string scheduleText(const Schedule& s);

// With a MAC key the user imported (the GAK): AES-CCM* of a secured frame. Header IEs stay in the clear, payload IEs and payload are decrypted. True on a MIC match.
struct Frame;
bool decryptFrame(const uint8_t* mpdu, size_t n, const Frame& f, const uint8_t key[16], std::vector<uint8_t>& plain);

struct Frame {
    bool ok = false;                  // the header could be read
    int type = 0, version = 0;
    bool secured = false, pending = false, ackReq = false, panCompress = false, seqSuppressed = false, ieList = false;
    int destMode = 0, srcMode = 0;    // 0 none, 2 short, 3 extended
    int seq = -1;
    bool hasDestPan = false, hasSrcPan = false;
    uint16_t destPan = 0, srcPan = 0;
    uint16_t destShort = 0, srcShort = 0;
    uint64_t destExt = 0, srcExt = 0;   // as the usual EUI-64 (the air carries it little endian)
    // auxiliary security header
    int secLevel = -1, keyIdMode = -1, keyIndex = -1;
    uint32_t frameCounter = 0;
    // Wi-SUN header IEs
    int utt = -1;                     // frame type (Utt), -1 no UTT-IE
    uint32_t ufsi = 0;                // unicast fractional sequence interval
    bool hasBt = false;
    int bsn = 0;                      // broadcast slot number
    uint32_t bio = 0;                 // broadcast interval offset (ms)
    int rslDbm = -999;                // received signal level the sender reports for the frame it answers (RSL-IE), -999 none
    bool hasFc = false;
    int txFlow = 0, rxFlow = 0;
    uint64_t eaEui = 0;               // EA-IE: the authenticator's EUI-64
    bool hasLbc = false;
    uint32_t lfnBcastMs = 0;          // LBC-IE: LFN broadcast interval
    // payload IEs (frames that are not secured)
    Schedule us, bs;
    bool hasPanIe = false;
    int panSize = -1, routingCost = -1, tpsVersion = -1;
    bool useParentBs = false, rplRouting = false;
    std::string netName;              // "" when no NETNAME-IE
    bool hasNetName = false;
    int panVersion = -1;
    bool hasGtkHash = false;
    int lfnVersion = -1;
    std::vector<int> phyModes;        // POM-IE: the PHY operating mode IDs the node supports
    // MPX IE and its payload
    int mpxId = -1;                   // multiplex ID (0xA0ED: 6LoWPAN), -1 none
    int kmpId = -1;                   // KMP ID (1: IEEE 802.1X / EAPOL)
    int eapolType = -1;               // EAPOL packet type (0 EAP packet, 3 key)
    int eapCode = -1, eapType = -1;   // EAP: 1 request, 2 response, 3 success, 4 failure; type 1 identity, 13 TLS
    std::string ies;                  // the IEs in order, short names: "UTT BT LBC HT1"
    size_t payloadAt = 0, payloadLen = 0;   // within the MPDU (FCS excluded)
    bool encrypted = false;           // secured: payload IEs and payload encrypted (shown as such, never decoded)
    std::string info;                 // one line: what the frame is
};

// Reads an MPDU (FCS excluded). The header is decoded as far as it goes.
Frame parse(const uint8_t* mpdu, size_t n);
std::string srcText(const Frame& f);
std::string dstText(const Frame& f);
std::string euiText(uint64_t e);      // "18:DF:C1:FF:FE:8B:0D:4D"

// ---------------------------------------------------------------- building frames (test signal, tests)

struct Build {
    int utt = DataFrame;
    bool secured = false;
    int keyIndex = 1;
    uint32_t frameCounter = 0;
    int seq = -1;                     // -1: suppressed
    uint64_t src = 0, dst = 0;        // 0: none
    bool srcPanPresent = false;       // a broadcast frame with the PAN ID (dst none, src extended)
    uint16_t pan = 0xFFFF;
    bool ackReq = false;
    uint32_t ufsi = 0;
    bool bt = false;
    int bsn = 0;
    uint32_t bio = 0;
    int rslDbm = -999;
    uint64_t ea = 0;
    bool lbc = false;
    uint32_t lfnBcastMs = 60000;
    // payload IEs (in the clear: not secured)
    Schedule us, bs;
    bool panIe = false;
    int panSize = 0, routingCost = 0, tpsVersion = 1;
    bool useParentBs = true;
    std::string netName;              // "" none
    int panVersion = -1;
    bool gtkHash = false;
    std::vector<int> phyModes;
    int mpxId = -1, kmpId = -1;       // an MPX IE (KMP when kmpId >= 0)
    std::vector<uint8_t> payload;     // after the IEs (secured: the ciphertext, MIC included)
};
std::vector<uint8_t> buildMpdu(const Build& b);

} // namespace wisun
} // namespace dect2
