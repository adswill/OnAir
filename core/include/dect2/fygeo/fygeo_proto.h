// FengYun geostationary downlinks: the formats above the radio layer (both directions: the test signals are made with the same code the
// receiver uses to read them).
//
// FY-4A / FY-4B LRIT and FY-4A HRIT-II/III (DVB-S2, QPSK, L-band): the baseband frames carry MPEG transport stream packets; on one PID a
// payload of about 1064 bytes holds a 40 byte IP/UDP style header and then a 1024 byte CADU (1ACFFC1D, then a CCSDS AOS VCDU of 1020 bytes:
// 6 byte header, 2 byte M_PDU header, 1012 bytes of packet zone; no Reed-Solomon, the DVB-S2 LDPC/BCH has done the error correction).
// CP_PDUs and files are the LRIT transport of goes_proto.h (the same layers, the packet zone is 1012 bytes). The file headers are FY-4's
// dialect (this file): primary header, image information (type 1, 32 bytes with satellite and instrument name, channel and segment numbers),
// navigation (type 2), annotation (4: the product's file name), time stamp (5), key (7: non-zero = encrypted). Images are JPEG 2000
// code streams (OpenJPEG), one file for each segment of a picture.
//
// FY-2 S-VISSR (BPSK 660 ksym/s, differential NRZ-M, no error correction): a frame is a 10000 bit PN preamble (15 bit LFSR, x^15 + x^14 + 1)
// and 354848 bits (44356 bytes) scrambled by the continuation of the same PN sequence (odd bytes inverted). A frame is one scan line of
// IR1 - IR3 (8 bit), IR4 (10 bit) and four lines of VIS (6 bit); the line counter is at bytes 67 and 68.
// The S-VISSR and FY-4 layouts follow SatDump (GPL-3.0, Aang23 and contributors), see THIRD_PARTY.md.
#pragma once
#include "dect2/goes/goes_proto.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace dect2::fygeo {

// ---------------------------------------------------------------- FY-4 file headers
constexpr int kZoneBytes = 1012;                 // packet zone of a FY-4 VCDU (VCDU 1020 bytes, CADU 1024)
constexpr uint32_t kAsm = 0x1ACFFC1Du;
constexpr size_t kCaduBytes = 1024;

struct Fy4Headers {
    bool ok = false;
    int fileType = -1;                 // 0 image, others text / data
    uint32_t headerLength = 0;
    uint64_t dataBits = 0;
    bool hasImage = false;
    std::string satName, instName;     // "FY-4A", "AGRI"
    int bpp = 0, columns = 0, lines = 0;   // of one segment
    int compression = 0;               // image information: 2 = JPEG 2000 on the FY-4 links
    int channel = 0, totalSeg = 0, segNo = 0, segPos = 0, segLine = 0;   // segLine: first line of this segment in the picture
    int algo = 0, lossless = 0, level = 0;
    bool hasNav = false;
    std::string projection;
    float cfac = 0, lfac = 0, coff = 0, loff = 0;
    std::string annotation;            // the file name of the product
    bool hasTime = false;
    int days = 0;
    uint32_t msOfDay = 0;
    bool hasKey = false;
    uint32_t key = 0;                  // non-zero: encrypted
};
bool parseFy4Headers(const uint8_t* p, size_t n, Fy4Headers& h);
std::vector<uint8_t> buildFy4Headers(const Fy4Headers& h);   // dataBits is taken from h
double fy4HeaderTime(const Fy4Headers& h);                   // unix seconds of the time stamp (0: none)

// The pieces of a FY-4 file name  FY4A-_AGRI--_N_DISK_1047E_L1-_FDI-_MULT_NOM_20230101000000_20230101001459_4000M_V0001.HDF
struct Fy4Name {
    bool ok = false;
    std::string sat;                   // "FY-4A"
    std::string instrument, region;    // "AGRI", "DISK"
    std::string channel;               // field 7 ("C012", "MULT")
    double startSec = 0;               // unix seconds of field 9
    std::string group;                 // sat/region/start: the segments of one picture have the same
};
Fy4Name parseFy4Name(const std::string& annotation);

// ---------------------------------------------------------------- JPEG 2000 (OpenJPEG)
// Decodes a JPEG 2000 code stream (or JP2 file) of one component: pixels as 16 bit, the component's own bit depth in bpp. false: damaged.
bool j2kDecode(const uint8_t* d, size_t n, int& w, int& h, int& bpp, std::vector<uint16_t>& px);
// Encodes one component, lossless (reversible 5/3, layers 1) when quality <= 0, else lossy at a compression ratio of `quality`.
std::vector<uint8_t> j2kEncode(const uint16_t* px, int w, int h, int bpp, int quality = 0);

// ---------------------------------------------------------------- the CADU in the transport stream
// Wraps a CADU (1024 bytes, ASM first) as the DVB-S2 stream carries it: 40 byte header then the CADU, split over 188 byte TS packets of `pid`
// (PUSI on the first, pointer field 0). `cc` is the continuity counter, advanced. Appends 188 byte packets to `out`.
void caduToTs(const uint8_t* cadu, int pid, uint8_t& cc, std::vector<uint8_t>& out);
// TS packets in, CADUs out (any PID whose reassembled payload holds the ASM in its first 128 bytes). One instance for a stream.
class TsCaduExtractor {
public:
    // cb(cadu, pid) for every CADU found; returns how many
    size_t push(const uint8_t* packets, size_t nPackets, const std::function<void(const uint8_t* cadu, int pid)>& cb);
    uint64_t packets = 0, caduCount = 0, tsErrors = 0;
    std::vector<std::pair<int, uint64_t>> pids() const;   // every PID seen and its packets, busiest first
private:
    struct Pid { std::vector<uint8_t> cur; bool active = false; uint64_t count = 0; };
    std::vector<Pid> pid_ = std::vector<Pid>(8192);
};

// ---------------------------------------------------------------- S-VISSR
constexpr int kSvissrFrameBytes = 44356, kSvissrFrameBits = 354848, kSvissrPreamble = 10000;
constexpr int kSvissrIrWidth = 2291, kSvissrVisWidth = 9160, kSvissrMaxLines = 2501;
constexpr uint64_t kSvissrMarker = 0b0100101110111011101110011001100110010101010101010111111111111111ull;
constexpr uint16_t kSvissrPnSeed = 0b011001110011111;

// The scrambling of a frame: byte i is XORed with table[i], odd bytes then complemented. Self-inverse in the sense of svissrScramble(svissrDescramble(x)) = x.
const std::vector<uint8_t>& svissrPnBytes();                         // the 44356 byte table
void svissrDescramble(uint8_t* frame);                               // received frame (44356 bytes) -> clear frame
void svissrScramble(uint8_t* frame);                                 // clear frame -> what is sent
// the PN sequence bits, preamble first: the 10000 bit preamble ends with the sync window kSvissrMarker
std::vector<uint8_t> svissrPreambleBits();

// The picture data of a clear frame. Writes one scan line into the channel buffers (8 bit): ir[0..3] each kSvissrIrWidth bytes, vis 4 lines of
// kSvissrVisWidth bytes. Returns the line counter.
int svissrLineCounter(const uint8_t* frame);
void svissrUnpack(const uint8_t* frame, uint8_t* ir1, uint8_t* ir2, uint8_t* ir3, uint8_t* ir4, uint8_t* vis4);
// The other direction: puts the lines into an otherwise empty frame (spare area zero) and sets the counter.
void svissrPack(uint8_t* frame, int lineCounter, const uint8_t* ir1, const uint8_t* ir2, const uint8_t* ir3, const uint8_t* ir4, const uint8_t* vis4);
// The spare area of the documentation sector (frame bytes 2114 to 2292, 179 bytes sent as zero): how many of its bits are set in a clear frame
int svissrSpareErrors(const uint8_t* frame, int* totalBits = nullptr);

} // namespace dect2::fygeo
