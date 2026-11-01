// GOES-R series HRIT (GOES-16 / 18 / 19, 1694.1 MHz), the layers above the CADU (both directions: the test signal is built with the
// same code it is checked by). The frame layer (sync word, randomiser, Reed-Solomon (255,223) interleave 4) is meteor_proto.h's.
//
// VCDU (892 bytes): 6-byte primary header (version, spacecraft ID, virtual channel, 24-bit counter, signalling), then the M_PDU: a
// 2-byte header with the 11-bit first header pointer (2047: no packet starts here) and 884 bytes of packet zone. Virtual channel 63 is
// fill. CP_PDUs (CCSDS space packets, APID 2047 idle) run across the zones of a virtual channel; their user data end in a CRC-16.
// A file (TP_PDU / S_PDU) is the user data of the packets of one APID from a "first" to a "last" segment flag: it starts with a 10-byte
// transport header (file counter, length in bits) and then the LRIT / HRIT file: a primary header (type 0: file type, header length,
// data length in bits), secondary headers (1 image structure, 2 navigation, 3 data function, 4 annotation, 5 time stamp, 6 ancillary
// text, 7 key, 128 segment identification, 129 NOAA product, 130 header structure, 131 Rice compression) and the data field.
// Images compressed with Rice (CCSDS 121, ccsds.h) send one scan line a packet after the header packet; JPEG and GIF products are a
// whole picture file, EMWIN text is plain or ZIP. The segments of a full disk or mesoscale image (header 128) are put together here.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace dect2::goes {

constexpr int kVcduBytes = 892, kZoneBytes = 884, kFillVcid = 63, kIdleApid = 2047;
constexpr int kTpHeader = 10;

// ---- file headers
struct Headers {
    bool ok = false;
    int fileType = -1;                 // 0 image, 1 GTS message, 2 alphanumeric text, 3 encryption key, 130 DCS, 214 EMWIN, ...
    uint32_t headerLength = 0;         // all headers, bytes
    uint64_t dataBits = 0;             // the data field, bits
    bool hasImage = false;
    int bpp = 0, columns = 0, lines = 0, compression = 0;   // image structure: compression 0 none, 1 lossless (Rice), 2 lossy
    bool hasNav = false;
    std::string projection;
    int32_t cfac = 0, lfac = 0, coff = 0, loff = 0;
    std::string annotation;            // usually the product's file name
    bool hasTime = false;
    int days = 0;                      // CCSDS day segmented time: days since 1958-01-01 and ms of the day
    uint32_t msOfDay = 0;
    std::string ancillary, headerText;
    bool hasSeg = false;
    int imageId = 0, segSeq = 0, startCol = 0, startLine = 0, maxSeg = 0, maxCol = 0, maxRow = 0;
    bool hasNoaa = false;
    int productId = 0, subId = 0, parameter = 0, noaaCompression = 0;   // NOAA compression: 0 none, 1 Rice, 2 JPEG, 5 GIF, 10 ZIP
    bool hasRice = false;
    int riceFlags = 0, ricePpb = 0, riceLines = 0;
    int keyNo = 0;                     // GK-2A key header (7): index of the DES key the data field is encrypted with, 0 = plain
    std::string obsTime;               // GK-2A header 131: image observation time
};
// false when the primary header is missing or the records do not fit. dialect 1: the GK-2A (KMA) records: 7 key header, 128 image
// segment (sequence, total, first line), 129 .. 132 text
bool parseHeaders(const uint8_t* p, size_t n, Headers& h, int dialect = 0);
// the header records of h (the primary header's lengths filled in from dataBits and the records)
std::vector<uint8_t> buildHeaders(const Headers& h);
// unix seconds of the time stamp
double headerTime(const Headers& h);

enum Kind { KFullDisk = 0, KMeso, KConus, KImage, KEmwin, KAdmin, KDcs, KText, KOther, KNumKinds };
const char* kindName(int k);
int classify(const Headers& h);
const char* compressionName(const Headers& h);   // "Rice", "JPEG", ...

// ---- products: what the interface lists and shows
struct Product {
    uint64_t id = 0;                    // in the order they started
    int kind = KOther;
    int vcid = 0, apid = 0;
    Headers hdr;                        // of the first file (the first segment)
    std::string name;                   // the annotation, or a made-up name
    double startSec = 0;                // signal time it started
    mutable std::mutex mu;              // the fields below change while it grows (the decoder thread): hold it to read them
    // images: grey, 8 bits a pixel (wider samples shifted down), w x h; rows not received stay 0
    int w = 0, h = 0;
    std::vector<uint8_t> pix;
    std::vector<uint8_t> rowDone;       // 1 for each row received
    int rowsDone = 0;
    int segsDone = 0, segsTotal = 0;
    // other products: the data field (a JPEG / GIF picture, the text, a ZIP file); and the text to show (ZIP unpacked)
    std::vector<uint8_t> data;
    std::string text;
    std::string innerName;              // the file inside a ZIP
    bool complete = false;
    bool errors = false;                // packets lost or CRC errors
    uint64_t version = 0;               // grows with every change
    uint64_t bytes = 0;                 // file bytes received
    // GK-2A: files that could not be turned into pictures
    int keyNo = 0;                      // the key this product's data are encrypted with (0 plain)
    bool needKey = false;               // encrypted and the key is not loaded
    std::string note;                   // why a picture is missing (shown in the product list)
    int segsLocked = 0;                 // segments received encrypted and still waiting for their key
    int channelIdx = -1;                // GK-2A: 0 .. 15 for VI004 .. IR133, -1 not a channel image
};

// ---- the transport layer and file assembly: corrected VCDUs in, products out
class Assembler {
public:
    Assembler();
    ~Assembler();
    void reset();                         // everything partial is dropped; the products stay
    void clear();                         // the products too
    void push(const uint8_t* vcdu, double nowSec = 0);
    std::function<void(const std::string&)> onLog;
    std::function<void(const std::vector<uint8_t>& file, int vcid, int apid)> onFile;   // every whole LRIT file (headers and data)
    // products, newest first (at most maxProducts are kept)
    std::vector<std::shared_ptr<Product>> products() const;
    void adopt(std::vector<std::shared_ptr<Product>> v);   // products from the results file (newest first): they replace the list
    void setMaxProducts(size_t n);
    // 0 GOES / Elektro-L (HRIT as CGMS), 1 GK-2A (KMA xRIT: JPEG 2000 pictures, DES, headers 7 / 128 of the KMA kind). Before the first frame.
    void setDialect(int d);
    // DES keys by key number (GK-2A); a file with a key number that is not here stays encrypted and says so
    void setKeys(const std::vector<std::pair<int, std::array<uint8_t, 8>>>& keys);
    // counters
    uint64_t vcdus = 0, fillVcdus = 0, vcduGaps = 0, packets = 0, crcErrors = 0, files = 0, fileErrors = 0;
    int scid = -1;
    uint64_t perVcid[64] = {};
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

// KMA key file as xrit-rx reads it (EncryptionKeyMessage.bin once decrypted for the station): 2 byte count, then (2 byte index, 8 byte key)
// each. Also accepts lines of "index:hexkey" text. Empty when nothing parses.
std::vector<std::pair<int, std::array<uint8_t, 8>>> parseKeyFile(const std::vector<uint8_t>& data);
// the channel number (0 .. 15) a GK-2A file name names (IMG_FD_143_IR105_...), -1 when it is not an image name
int gk2aChannelIndex(const std::string& annotation);
const char* gk2aChannelName(int idx);       // "VI004" ... "IR133"

// unpacks the first file of a ZIP archive (stored or deflated): false when it is not one
bool unzipFirst(const std::vector<uint8_t>& zip, std::vector<uint8_t>& out, std::string& name);

// ---- the other direction (test signals)
// A file split into CP_PDUs: one packet of transport header and LRIT headers, then (Rice) one packet a scan line, or the data field in
// pieces of at most `maxChunk` bytes. Each packet: user data (no CRC).
struct FileOut {
    int apid = 0;
    std::vector<std::vector<uint8_t>> chunks;
};
// Builds the packet stream of a virtual channel into VCDUs: whole files at a time, the last zone padded with an idle packet.
class Mux {
public:
    explicit Mux(int scid = 0) : scid_(scid) {}
    void addFile(int vcid, const FileOut& f);
    bool next(uint8_t vcdu[kVcduBytes]);            // false when no data are waiting (then send fill)
    void fill(uint8_t vcdu[kVcduBytes]);            // a fill VCDU (virtual channel 63)
private:
    struct Vc { std::vector<uint8_t> q; std::vector<size_t> starts; uint32_t counter = 0; };
    void flushVc(Vc& v);
    int scid_;
    Vc vc_[64];
    int seq_[2048] = {};
    std::vector<int> order_;                          // virtual channels with data, in the order they were filled
    uint32_t fillCounter_ = 0;
};
// A whole file as the generator makes it: headers (h's lengths filled in) and the data field.
std::vector<uint8_t> makeFile(Headers h, const std::vector<uint8_t>& dataField);

} // namespace dect2::goes
