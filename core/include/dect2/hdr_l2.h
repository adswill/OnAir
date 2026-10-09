// HD Radio (NRSC-5) Layer 2 and above, without the audio codec: the transfer frame header (PCI), the audio PDU headers and their packet
// locators, the HDLC-framed AAS packets (program service data, the station information guide, large objects), the fixed data
// subchannels, and the station information service (SIS) of the PIDS channel. The audio packets themselves are HDC, a patented codec
// that OnAir does not decode: they are counted and measured only.
//
// The decoder follows nrsc5 (GPL-3.0, github.com/theori-io/nrsc5: src/frame.c, src/pids.c, src/output.c); the encoder, used by the test
// signal, builds exactly what that decoder reads.
#pragma once
#include "hdr_tel.h"
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace dect2 { namespace hdr {

// ---- PCI code words (nrsc5 frame.c)
constexpr uint32_t kPciAudio = 0x38D8D3, kPciAudioOpp = 0xCE3634, kPciAudioFixed = 0xE3634C, kPciAudioFixedOpp = 0x8D8D33, kPciFixed = 0x3634CE;

// ---- MIME hashes (nrsc5 nrsc5.h)
constexpr uint32_t kMimePrimaryImage = 0xBE4B7536, kMimeStationLogo = 0xD9C72536, kMimeHdc = 0x4DC66C5A, kMimeText = 0xBB492AAC;
constexpr uint32_t kMimeJpeg = 0x1E653E9C, kMimePng = 0x4F328CA0, kMimeTtnTraffic = 0xFF8422D7, kMimeTtnWeather = 0xEF042E96;
const char* mimeName(uint32_t mime);        // "station logo", "JPEG", ... or "" when unknown

// ---- transfer frame geometry: where the PCI bits sit and how long the PDU is
struct FrameGeom { int lenBits; int start; int offset; int pciLen; };
bool frameGeom(int lenBits, FrameGeom& g);
int pduBytes(int lenBits);                  // bytes of the PDU in a transfer frame of that many bits
// PDU bytes + PCI -> the transfer frame bits in L1 order (before the scrambler); the reverse of frame_push() of nrsc5
void packTransfer(const std::vector<uint8_t>& pdu, uint32_t pci, int lenBits, uint8_t* bits);
// L1 bits -> PDU bytes and the received PCI word (24 bits, the missing low bits zero in AM P1)
void unpackTransfer(const uint8_t* bits, int lenBits, std::vector<uint8_t>& pdu, uint32_t& pci);
int matchPci(uint32_t pci, int pciLen, uint32_t& match);   // bit errors to the nearest code word (<= 4), -1 when none is near

// ---- encoder pieces (test signal)
void hdlcAppend(std::vector<uint8_t>& out, const std::vector<uint8_t>& frame);   // escaped frame and a closing flag
std::vector<uint8_t> aasFrame(uint16_t port, uint16_t seq, const std::vector<uint8_t>& payload);   // 0x21, port, seq, payload, FCS
std::vector<uint8_t> id3Tag(const std::string& title, const std::string& artist, const std::string& album, const std::string& genre, int xhdrLot, uint32_t xhdrMime);

struct SigService {
    bool audio = true;
    int number = 1;
    std::string name;
    struct Comp { bool audio; int id; int port; int type; uint32_t mime; int sdType; };
    std::vector<Comp> comps;
};
std::vector<uint8_t> sigTable(const std::vector<SigService>& services);

struct LotFile { int lot; std::string name; uint32_t mime; std::vector<uint8_t> bytes; };
// The fragments of a LOT file, each the payload of one AAS packet (the header rides on the first)
std::vector<std::vector<uint8_t>> lotFragments(const LotFile& f, int repeat);

struct AudioPduSpec {
    int codecMode = 0, streamId = 0, program = 0, progType = 0, access = 0;
    int pduSeq = 0, latency = 0, seq = 0;   // sequence numbers of the PDU and of its first packet
    std::vector<int> packetBytes;           // audio packet payload sizes (each gets its CRC byte)
    uint8_t fill = 0;                       // the packet payload (filler: there is no HDC encoder)
    int psdRoom = 0;                        // bytes for the PSD stream after the header (the PDU's la_location limits it)
};
int audioPduHeaderBytes(const AudioPduSpec& s);   // header, locators and header expansion
// Builds the PDU; takes up to psdRoom bytes from psd (the program's HDLC byte stream, padded with flags when short)
std::vector<uint8_t> audioPdu(const AudioPduSpec& s, std::vector<uint8_t>& psd);

// The fixed data region at the end of a transfer frame: subchannel bytes, the CCC bytes and the sync byte (nrsc5 process_fixed_data())
struct FixedSpec { int subLen = 0; int cccWidth = 10; };   // one subchannel of subLen bytes per frame; CCC width (even, 2..30)
// Appends to the PDU end: takes subLen bytes from sub (the stream of BBM-headed 255-byte blocks)
void fixedRegion(const FixedSpec& f, std::vector<uint8_t>& sub, std::vector<uint8_t>& out);
// Appends one fixed-subchannel block (BBM + 255 bytes of HDLC stream taken from hdlc, padded with flags)
void fixedBlock(std::vector<uint8_t>& hdlc, std::vector<uint8_t>& out);

// ---- SIS (station information service on the PIDS channel)
struct SisConfig {
    std::string callSign = "WONR";          // short name: 4 letters (A-Z and a few symbols)
    bool fmSuffix = false;                  // adds -FM
    std::string name;                       // universal short station name (up to 12 characters)
    std::string longName;                   // up to 56 characters
    std::string slogan;                     // up to 95 characters
    std::string message;                    // up to 190 characters
    std::string country = "US";
    int facilityId = 0;
    bool location = false;
    double lat = 0, lon = 0; int altM = 0;
    struct Asd { int program, type, access, soundExp; };
    struct Dsd { int type, mime, access; };
    std::vector<Asd> audio;
    std::vector<Dsd> data;
};
// Makes the PIDS frames: every call gives the next 80-bit frame (in the frame bit order of the decoder, CRC included)
class SisEncoder {
public:
    explicit SisEncoder(const SisConfig& c);
    void next(uint8_t* bits80);
private:
    struct Msg { int id; std::vector<uint8_t> bits; };
    std::vector<Msg> msgs_;
    size_t pos_ = 0;
};

// ---- the decoder of everything above L1
class L2Decoder {
public:
    L2Decoder();
    ~L2Decoder();
    void reset();                       // forget everything (a new station)
    void resetTransport();              // a new block sync: restart the HDLC streams and the fixed data sync, keep the station
    void setLog(std::function<void(const std::string&)> cb) { log_ = std::move(cb); }
    void setTime(double sec) { now_ = sec; }
    // descrambled bits from L1: a PIDS frame (80 bits) and a transfer frame of a logical channel (0 = P1, 1 = P3, 2 = P4)
    void pushPids(const uint8_t* bits);
    void pushTransfer(const uint8_t* bits, int lenBits, int channel);
    // the results: station, programs, data services, objects (the fields of HdrTelemetry from callSign on, plus the L2 counters)
    void fill(HdrTelemetry& t) const;
    bool lotBytes(int port, int lot, std::vector<uint8_t>& out) const;
    uint64_t pciOk(int channel = 0) const { return pciOk_[chan(channel)]; }     // transfer frames with a good / bad header, per logical channel
    uint64_t pciBad(int channel = 0) const { return pciBad_[chan(channel)]; }

private:
    struct Sis;
    struct Prog;
    struct Fixed;
    struct Lot { HdrLotInfo info; std::vector<std::vector<uint8_t>> frags; std::vector<uint8_t> bytes; };
    void frameProcess(std::vector<uint8_t>& buf, int channel, uint32_t pci);
    size_t fixedData(std::vector<uint8_t>& buf, int channel);
    void hdlcFeed(std::vector<uint8_t>& acc, bool& open, const uint8_t* p, size_t n, int kind, int channel);
    void aasFrame(std::vector<uint8_t>& f, int kind, int channel);
    void aasPacket(uint16_t port, uint16_t seq, const uint8_t* p, size_t n);
    void id3(int program, const uint8_t* p, size_t n);
    void sig(const uint8_t* p, size_t n);
    void lot(int port, const uint8_t* p, size_t n);
    void cccFrame(std::vector<uint8_t>& f, int channel);
    void sisDecode(const uint8_t* bits);
    Prog& prog(int n);
    void say(const std::string& s) { if (log_) log_(s); }

    std::function<void(const std::string&)> log_;
    double now_ = 0;
    std::unique_ptr<Sis> sis_;
    std::map<int, Prog> progs_;
    std::vector<HdrDataService> dsds_;            // from the SIS
    std::vector<SigService> sigServices_;
    std::vector<uint8_t> sigRaw_;
    std::map<std::pair<int, int>, Lot> lots_;     // (port, lot)
    uint64_t lotVersion_ = 0;
    std::map<int, std::pair<uint64_t, uint64_t>> portCount_;   // packets, bytes per port
    static int chan(int c) { return c < 0 ? 0 : c > 2 ? 2 : c; }
    std::unique_ptr<Fixed> fixed_[3];
    uint64_t pciOk_[3] = {0, 0, 0}, pciBad_[3] = {0, 0, 0}, pdus_ = 0, aasOk_ = 0, aasBad_ = 0, pidsOk_ = 0, pidsBad_ = 0;
    double lastPsd_ = -1e9, lastData_ = -1e9;
};

}} // namespace dect2::hdr
