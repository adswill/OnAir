// CDR multiplex (GY/T 268.2-2013 "Digital audio broadcasting in FM band, part 2: multiplexing"): the control multiplex frame that the
// service description channel carries (clause 6: the service multiplex configuration table and the network information table) and the
// service multiplex frame of the service data channel (clause 7: multiplex sub-frames with an audio section and a data section).
// Writers for the test signal, parsers for the receiver. All CRCs as in Annex C. Bytes are sent most significant bit first.
//
// Not in GY/T 268.2 (and so not decoded): service names and programme types (they belong to the electronic service guide, table 0x03),
// the meaning of the audio algorithm codes and the payload formats of the data units ("later technical documents").
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace dect2::cdr {

// ---------------------------------------------------------------- clause 6

struct SmctEntry {               // one service multiplex frame identifier (Table 3)
    int smfId = 1;
    bool hier = false;           // the service data uses hierarchical modulation
    bool high = false;           // ... and this multiplex frame is the high protection one
    int txMode = 0xF;            // which of the four logical frames of a super frame carry it (bit 3 = first)
    std::vector<uint16_t> services;   // service identifier of every multiplex sub-frame
};
struct Smct {
    int segNo = 0, segCount = 1, update = 0;
    std::vector<SmctEntry> frames;
};
struct NitNeighbour { uint64_t id = 0; std::vector<uint32_t> freqs; };
struct Nit {
    int segNo = 0, segCount = 1, update = 0;
    std::string country = "CHN";             // three letters (GB/T 2659)
    uint64_t networkId = 0;                  // 36 bits, 0..31 reserved
    std::vector<uint32_t> freqs;             // centre frequencies in 10 Hz
    std::vector<uint8_t> name;               // network name, coded as in GB/T 28161 Annex A
    std::vector<NitNeighbour> neighbours;
};
constexpr int kTableSmct = 0x01, kTableNit = 0x02, kTableEsg = 0x03;

std::vector<uint8_t> smctBytes(const Smct& t);
std::vector<uint8_t> nitBytes(const Nit& t);
// Header (lengths, CRC-8) followed by the tables
std::vector<uint8_t> controlFrameBytes(const std::vector<std::vector<uint8_t>>& tables);

struct ControlFrame {
    bool headerOk = false;
    std::vector<int> tableLens;
    int tablesOk = 0, tablesBad = 0;
    bool haveSmct = false, haveNit = false;
    Smct smct;
    Nit nit;
    std::vector<int> otherTables;            // ids of tables that are not decoded (ESG ...)
};
bool parseControlFrame(const uint8_t* d, size_t n, ControlFrame& out);   // false: the header CRC failed
bool parseSmct(const uint8_t* d, size_t n, Smct& out);                   // false: CRC or syntax
bool parseNit(const uint8_t* d, size_t n, Nit& out);

// ---------------------------------------------------------------- clause 7

struct AudioStreamDesc {         // the extension area of a sub-frame header (Table 6)
    int algo = 0;                // audio algorithm type: the codes are left to later technical documents
    int rate100 = -1;            // bit rate in 100 bit/s (-1: not sent)
    int sampleRateCode = -1;     // Table 9 (-1: not sent)
    int channelsCode = 0;        // Table 8: 1 mono, 2 two channels, 3 5.1
    std::string language;        // three letters (GB/T 4880.2), empty when not sent
};
struct AudioUnit { int stream = 0; int relTime = 0; std::vector<uint8_t> data; };
struct DataUnit { int type = 0; std::vector<uint8_t> data; };
struct MuxSubFrame {
    bool hasStartTime = true;
    uint32_t startTime = 0;      // 1/22500 s
    bool mode1 = true;           // encapsulation: segment mode (otherwise block mode)
    bool hasAudio = false, hasData = false, hasExt = false;
    std::vector<AudioStreamDesc> streams;
    std::vector<AudioUnit> audio;
    std::vector<DataUnit> data;
};
std::vector<uint8_t> subFrameBytes(const MuxSubFrame& s);    // writes segment mode (mode 1)

struct ServiceFrameHeader {
    int version = 1, emergency = 0, smfId = 1, nitUpdate = 0, smctUpdate = 0, esgUpdate = 0;
    std::vector<int> subLens;
};
// The service multiplex frame of one logical frame: header, sub-frames, padding with ones up to `capacity` bytes
std::vector<uint8_t> serviceFrameBytes(ServiceFrameHeader h, const std::vector<std::vector<uint8_t>>& subs, size_t capacity);

struct ParsedSubFrame {
    bool headerOk = false, audioOk = false, dataOk = false;
    int len = 0;
    MuxSubFrame sf;
};
struct ServiceFrame {
    bool headerOk = false;
    ServiceFrameHeader h;
    std::vector<ParsedSubFrame> subs;
};
bool parseServiceFrame(const uint8_t* d, size_t n, ServiceFrame& out);   // false: the header CRC failed

// helpers for the interface
const char* dataUnitTypeText(int type);      // Table 12
int sampleRateHz(int code);                  // Table 9 (0 unknown)
const char* channelsText(int code);          // Table 8
std::string printableText(const std::vector<uint8_t>& d);   // UTF-8 text if the bytes are printable UTF-8, else empty
std::string nameText(const std::vector<uint8_t>& d);        // a name field: printable ASCII as is, other bytes as \xNN

} // namespace dect2::cdr
