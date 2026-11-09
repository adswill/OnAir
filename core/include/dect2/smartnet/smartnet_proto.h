// Motorola Type II trunking (SmartNet, SmartZone): the outbound control channel word (OSW), the band plans that turn its channel numbers into frequencies, and the
// message layer that reads one to three OSWs as a grant, a site broadcast and so on.
//
// The channel is 3600 bit/s FSK. One OSW is 84 bits: the 8 sync bits 0xAC, then 76 bits read column-wise (19 rows of 4) from 38 (information, parity) pairs, where
// parity[i] = information[i] ^ information[i - 1]. The 38 information bits are 27 data bits (16 address, 1 group flag, 10 command), 10 check bits and one flush bit.
// Layout, CRC, masks and the meaning of the messages follow OP25 (boatbod, GPL-3, gr-op25_repeater rx_smartnet and tk_smartnet.py) and trunk-recorder (GPL-3);
// credited in THIRD_PARTY.md. No Motorola document describes this publicly: these are the open source decoders' findings.
#pragma once
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace dect2::smartnet {

constexpr double kBaud = 3600.0;
constexpr int kFrameBits = 84, kSyncBits = 8, kPayloadBits = 76, kInfoBits = 38, kDataBits = 27, kCrcBits = 10;
constexpr uint32_t kSync = 0xAC;

struct Osw {
    int addr = 0;        // 16 bits, unmasked
    bool group = false;
    int cmd = 0;         // 10 bits, unmasked
    bool operator==(const Osw& o) const { return addr == o.addr && group == o.group && cmd == o.cmd; }
};

int crc(const uint8_t* info27);                                  // the 10 check bits of 27 data bits
void encodePayload(const Osw& o, uint8_t out[kPayloadBits]);     // 76 bits, in the order they are sent
// false when the check bits do not match. corrected: information bits the parity repaired.
bool decodePayload(const uint8_t bits[kPayloadBits], Osw& o, int* corrected = nullptr);
std::vector<uint8_t> frameBits(const Osw& o);                    // the 84 bits, sync first

// ------------------------------------------------------------------------------------------------ band plans

enum class Band { Auto, Domestic800, Rebanded800, Splinter800, International800, InternationalSplinter800, Us900, Custom };
const char* bandName(Band b);

// A Custom (OBT, VHF / UHF) plan: receive channels bp .. in three ranges, like OP25's bp_base / bp_mid / bp_high. Channels below baseOffset are transmit channels.
struct CustomPlan {
    double baseHz = 0, baseSpacingHz = 12500;
    int baseOffset = 380;
    double midHz = 0, midSpacingHz = 0;       // 0: same as the range below
    int midOffset = 760;
    double highHz = 0, highSpacingHz = 0;
    int highOffset = 760;
};

// The receive frequency of a channel number in Hz, 0 when the number is not a channel of the plan. Auto is not a plan: it returns 0.
double channelHz(Band b, const CustomPlan& c, int chan);
inline bool isChannel(Band b, const CustomPlan& c, int chan) { return channelHz(b, c, chan) > 0; }
inline bool isCustomTx(Band b, const CustomPlan& c, int chan) { return b == Band::Custom && chan >= 0 && chan < c.baseOffset; }
// The band for a control channel frequency: the plan in which channel number chan is at tunedHz (within 7 kHz). Auto when no plan fits; chan -1: from the frequency range alone.
Band bandFromControl(int chan, double tunedHz);

// ------------------------------------------------------------------------------------------------ messages

enum class Kind {
    None, Idle, GroupGrant, GroupUpdate, PrivateGrant, InterconnectGrant, ControlChannel, AltControl, AdjacentControl, SiteInfo, AdjacentSite, AltSite, SystemStatus, SystemBeacon,
    GroupBusy, EmergencyBusy, PrivateBusy, Regroup, Patch, PatchCancel, Affiliation, Deaffiliation, EmergencyPtt, StatusAck, EmergencyAck, MessageAck, PrivateRing, RingAck,
    CallAlert, CallAlertAck, Denial, DateTime, Amss, Roaming, InterconnectReject, AffiliationRequest, Diagnostic, ExtendedFunction, Unknown
};

struct Msg {
    Kind kind = Kind::None;
    int tg = -1;                  // talkgroup with the 4 call option bits cleared (addr & 0xfff0); -1 none
    int options = 0;              // the 4 call option bits
    int src = -1, dst = -1;
    int system = -1, site = -1;   // site: 1-based
    int band = -1, features = -1; // of a SiteInfo
    int chan = -1;
    double freqHz = 0;
    bool digital = false, group = true, encrypted = false, emergency = false, patch = false, multiselect = false;
    bool typeII = true;           // of a SystemStatus opcode 1
    int opcode = -1, data = -1;
    int year = 0, month = 0, day = 0, dow = 0, hour = 0, minute = 0;
    int words = 1;                // OSWs this message used
    bool afterBad = false;        // read right after a bad OSW, so an Unknown may be the tail of a lost message
};

std::string describe(const Msg& m);
const char* siteBandName(int band);          // the band field of a SiteInfo
std::string featuresText(int features);      // the features field of a SiteInfo
bool isWideArea(int features);               // the site belongs to a network: SmartZone

// Reassembles the OSW stream into messages. Keeps a small queue because a message spans up to three OSWs and a lone idle can sit in the middle of one.
class Parser {
public:
    void setBand(Band b, const CustomPlan& c = CustomPlan());
    Band band() const { return band_; }
    void feed(const Osw& o);                 // one good OSW
    void bad();                              // an OSW was lost: the message it belonged to is not assembled across the gap
    bool next(Msg& out);                     // the next complete message; false when more OSWs are needed
    void clear() { q_.clear(); }
private:
    static constexpr int kBadCmd = 0xFFE;
    static constexpr size_t kNeed = 5;       // three OSWs of a message, plus up to two idles inside it
    Band band_ = Band::Domestic800;
    CustomPlan custom_;
    std::deque<Osw> q_;
    double hz(int chan) const { return channelHz(band_, custom_, chan); }
    bool chan(int c) const { return hz(c) > 0; }
    Osw pop() { Osw o = q_.front(); q_.pop_front(); return o; }
    void options(Msg& m, int addr) const;
    bool parseOne(Msg& m);
    bool parse308(Msg& m, const Osw& first);
    bool parse321(Msg& m, const Osw& first);
    bool parseObt(Msg& m, const Osw& first);
    void parseExtended(Msg& m, const Osw& first, const Osw& second);
};

} // namespace dect2::smartnet
