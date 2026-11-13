// COSPAS-SARSAT 406 MHz distress beacon message layer (first-generation beacons, specification C/S T.001, public document).
// A burst is 24 sync bits (15 bit-sync ones, 9 frame-sync bits) and a 112 bit (short) or 144 bit (long) message. Bits are numbered from 1 as in the
// specification; the arrays here hold bit n at index n-1. The first protected field is bits 25-85 with the 21 bit BCH(82,61) check in 86-106; a long
// message has a second field of bits 107-132 with the 12 bit BCH(38,26) check in 133-144.
// Clean-room from the public description of the format; the two generator polynomials were checked to divide x^127+1 and x^63+1.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace dect2::sarsat {

constexpr int kShortBits = 112, kLongBits = 144, kSyncBits = 24;
constexpr double kBitRate = 400.0;
constexpr double kDefaultMhz = 406.028;

// frame sync (bits 16-24): normal operation and self-test
constexpr uint16_t kFrameSyncNormal = 0x02F;   // 000101111
constexpr uint16_t kFrameSyncTest = 0x0D0;     // 011010000

struct Beacon {
    bool valid = false;          // the first BCH check passed (after correcting up to 3 bits)
    bool longMsg = false;
    bool userProto = false;      // bit 26: user protocols (1) or location protocols (0)
    bool selfTest = false;       // the self-test frame sync
    int mid = 0;                 // bits 27-36: country code (a maritime identification digit set)
    std::string country;
    int protoCode = 0;           // bits 37-40 (location) or 37-39 (user)
    std::string protoName;
    std::string beaconType;      // EPIRB, ELT, PLB, ship security ... or "?"
    std::string hexId;           // bits 26-85 as 15 hex digits
    std::string idText;          // the identification the protocol carries, in words
    bool hasPos = false;
    bool fine = false;           // the 4 second offsets of the long message were added
    double lat = 0, lon = 0;
    double posPrecisionDeg = 0;  // half of the cell the position can lie in
    bool homing = false;         // a 121.5 MHz homing transmitter is fitted (bit 112, RLS bit 108, user protocols bits 84-85 = 01)
    bool bch2Present = false, bch2Ok = false;
    int corrected = 0;           // bits the BCH codes repaired
    float repairMargin = 0;      // 4.0 soft decoding: how much closer the codeword is than the next one (mean |soft| units; 99: no other codeword found)
    float repairCost = 0;        // 4.0: soft confidence of the repaired bits, in units of the message's mean confidence (0: nothing repaired)
    std::string rawHex;          // all message bits (sync included) as hex, after the BCH repair
    std::string rxHex;           // the same bits as they were received, before the BCH repair (same as rawHex when nothing was repaired)
    std::vector<int> fixedBits;  // the 1-based bit numbers the BCH codes repaired (the bit numbering of the specification, sync = bits 1-24)
    // second-generation beacons (C/S T.018, see sgb_proto.h): hexId then holds the 23 hex ID
    bool second = false;
    int tac = 0, serial = 0;
    bool rls = false, testProto = false;
    int rotId = -1;              // bits 155-158, the rotating field of that burst (its content is not decoded)
};

// the sync bits as the specification gives them, followed by nothing: bits 1-24
std::vector<uint8_t> syncBits(bool selfTest);

// Checks and (up to 3 / 2 bit errors) repairs bits[0..n) in place, then fills `out`. n is 112 or 144; the sync bits are not checked here.
bool decode(std::vector<uint8_t>& bits, Beacon& out);
// Only the BCH-1 repair of decode (bits 25-106 in place): the number of bits repaired, -1 when the word cannot be repaired.
int bch1Correct(std::vector<uint8_t>& bits);

struct Spec {
    bool longMsg = true;
    bool selfTest = false;
    bool userProto = false;
    int mid = 245;               // Netherlands
    int protoCode = 2;           // location protocols: 4 bits; user protocols: 3 bits
    uint32_t ident = 0;          // location protocols: bits 41-64 (24 bits)
    uint64_t userBits = 0;       // user protocols: bits 40-85 (46 bits)
    bool hasPos = true;
    double lat = 0, lon = 0;
    bool fine = true;            // long message: the 4 second offsets
    bool homing = false;
};
// The whole burst message: sync bits, data, both BCH checks.
std::vector<uint8_t> encode(const Spec& s);

// text packed as the 6 bit modified Baudot of the user protocols (n characters)
uint64_t baudotPack(const std::string& s, int n);
std::string baudotUnpack(uint64_t v, int n);
// the 5 bit shortened Baudot of the aircraft operator designator (letters only)
std::string baudot5Unpack(uint64_t v, int n);

std::string countryOfMid(int mid);
std::string protocolName(bool user, int code, std::string* type);
std::string bitsToHex(const std::vector<uint8_t>& bits, int first, int last);   // bits first..last (1-based, inclusive), padded with zeros on the right to whole hex digits

// BCH helpers (exposed for the tests)
uint32_t bchRemainder(const std::vector<uint8_t>& bits, int first, int last, uint32_t gen, int degree);

} // namespace dect2::sarsat
