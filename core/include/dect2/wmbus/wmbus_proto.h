// Wireless M-Bus (EN 13757-4) link layer and application data records, written from the public standard (clean room, no ported code).
// Frame format A (blocks of 16 data bytes with a CRC each, first block 10 bytes) and B (two blocks, L counts the CRC), the CRC (poly 0x3D65, init 0,
// result inverted), 3-out-of-6 and Manchester chip coding, the manufacturer code, the device type, the DIF / VIF data records of an unencrypted
// application layer. An encrypted payload (AES, configuration word or extended link layer) is reported as encrypted and never decoded.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace dect2::wmbus {

// Physical modes the receiver and the test signal know. Rates and sync words are those of the standard as the author remembers them; N and R2 are not yet
// checked against a real signal.
enum class Phy { S1 = 0, T1, C1A, C1B, R2, N1, Count };
const char* phyName(Phy p);

uint16_t crc16(const uint8_t* d, size_t n);                  // EN 13757-4: poly 0x3D65, init 0, final xor 0xFFFF
int encode36(unsigned nibble);                                // 6 chips of the 3-out-of-6 code for a nibble
int decode36(unsigned six);                                   // the nibble, or -1 for a code word that is not one

// the bytes of the frame as sent (with the CRCs) for the logical frame L C M A CI data (given without L); formatB: the second block layout
std::vector<uint8_t> buildFrame(const std::vector<uint8_t>& body, bool formatB);
// number of bytes on air for a length field L
size_t airLength(uint8_t L, bool formatB);

struct Record {
    int64_t storage = 0;      // up to 41 bits: 1 in the DIF, 4 in each of up to 10 DIFE (EN 13757-3)
    int dif = 0, tariff = 0, subunit = 0, func = 0;   // function: 0 instantaneous, 1 maximum, 2 minimum, 3 error state
    std::string quantity;     // "Volume", "Energy", ...
    std::string value;        // with its unit: "1234.5 m3"
    std::string note;         // "storage 1, max" and the like
    double number = 0;
    bool numeric = false;
};

struct Frame {
    bool crcOk = false;
    bool formatB = false;
    uint8_t L = 0, C = 0, ci = 0, version = 0, type = 0;
    uint16_t mfrCode = 0;
    std::string mfr;          // three letters
    std::string id;           // 8 digits
    std::string typeName, cName, ciName;
    bool shortHeader = false, longHeader = false;
    uint8_t acc = 0, status = 0;
    uint16_t config = 0;
    bool encrypted = false;   // the payload is encrypted: shown as such, never decoded
    bool decrypted = false;   // decrypted with a user key
    std::string statusText;
    std::vector<uint8_t> data;       // L C M A CI ... without the CRCs
    std::vector<uint8_t> payload;    // after the CI and the header (the application data)
    std::vector<Record> records;
    bool hasManufacturerData = false;
    std::string error;
    std::string hex() const;
};

// Check the CRCs and strip them; fills the link layer fields and, for an unencrypted payload, the records. n is what was received (>= airLength).
bool parseFrame(const uint8_t* b, size_t n, bool formatB, Frame& f);

// With an AES key the user imported: decrypts TPL security mode 5 (AES-CBC, IV = M, A, 8 x ACC; the first plaintext bytes are 2F 2F) and reads the records. True when it decrypted.
bool decryptFrame(Frame& f, const uint8_t key[16]);

std::string mfrCodeToString(uint16_t code);
uint16_t mfrStringToCode(const char* three);
const char* deviceTypeName(uint8_t t);
std::string bcdId(const uint8_t* four);            // little-endian BCD to 8 digits

// the data records of an application payload (DIF/VIF); returns false when it ends in the middle of a record
bool parseRecords(const uint8_t* d, size_t n, std::vector<Record>& out, bool& manufacturerData);


// ------------------------------------------------------------ chip level and channel plan (shared by the receiver and the test signal)
enum class Coding { Manchester, ThreeOfSix, Nrz };
struct PhySpec { Phy phy; double chipRate; double devHz; Coding coding; uint16_t sync; };
const PhySpec& phySpec(Phy p);
// the chips on air (1 = upper frequency) for the bytes of a frame: preamble, sync word, [C-mode format marker], coded bytes
std::vector<uint8_t> encodeChips(Phy p, const std::vector<uint8_t>& air, int preamblePairs = 16);

struct ChannelDef {
    double freqHz;
    bool wide;              // 100 / 32.8 kchip/s channels (400 kHz working rate) against the narrow 12.5 / 60 kHz ones (48 kHz)
    double fsOut, lpfHz;    // working rate and channel filter after the mixer
    std::vector<Phy> phys;  // what is tried on this channel
    std::string label;
};
// the channels that lie inside a capture centred at centreHz with this sample rate (0 centre: the 868 MHz plan)
std::vector<ChannelDef> channelPlan(double centreHz, double sampleRate);

} // namespace dect2::wmbus
