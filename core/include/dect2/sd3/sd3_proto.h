// Layers above the demodulator for the SatDump tier 3 links, in both directions so the test signals are built with the code that checks them.
//
// Frame layouts, sync words, scramblers, the GRB and GVAR formats are taken from SatDump's plugins goes_support, gk2a_support, cubesat_support, spacex_support and
// resources/pipelines. Copyright (c) Aang23 and contributors, GPL-3.0. Written for OnAir; see THIRD_PARTY.md.
#pragma once
#include "dect2/sd3/sd3_tel.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace dect2::sd3 {

// ---- frames of a table link
// The frame (with its sync word, bytes = (frameBits + 7) / 8): randomiser and Reed-Solomon undone. corrected: symbols fixed; returns false when a word was not corrected.
bool decodeFrame(const Sd3Link& l, std::vector<uint8_t>& frame, int& corrected);
// the opposite: `frame` holds the sync word and the information bytes; check symbols are filled in and the randomiser applied
void encodeFrame(const Sd3Link& l, std::vector<uint8_t>& frame);
// bytes a table frame carries for data (without sync word and check symbols)
int infoBytes(const Sd3Link& l);
// scid / vcid of the frame after the sync word, -1 when it cannot be either a TM or an AOS header
void frameIds(const uint8_t* f, size_t n, int& scid, int& vcid);

// ---- AOS M_PDU -> space packets. `mpdu` data bytes at offset 12 (sync word 4, VCDU 6, FHP 2); one per virtual channel.
struct Packet {
    int apid = 0, seqFlag = 0, seq = 0;
    bool secHdr = false;
    std::vector<uint8_t> raw;        // the whole packet, 6 byte header first
};
class AosDemux {
public:
    explicit AosDemux(int mpdu, bool secExtends = false) : mpdu_(mpdu), secExtends_(secExtends) {}
    std::function<void(const Packet&)> onPacket;
    void reset() { buf_.clear(); have_ = false; lastCount_ = -1; }
    void push(const uint8_t* cadu);
    uint64_t dropped = 0;
private:
    void parse();
    int mpdu_;
    bool secExtends_;
    std::vector<uint8_t> buf_;
    bool have_ = false;
    int64_t lastCount_ = -1;
};
void buildAosFrame(int scid, int vcid, uint32_t counter, int mpdu, const std::vector<uint8_t>& stream, size_t& pos, int caduBytes, uint8_t* cadu, const std::vector<size_t>& starts);
uint32_t crc32Hdlc(const uint8_t* p, size_t n);
uint16_t crc16Ccitt(const uint8_t* p, size_t n, uint16_t init);        // 0x1021, MSB first, no final XOR

// ---- GOES-R GRB products
constexpr int kGrbCadu = 2048, kGrbMpdu = 2034;
int abiChannelOfApid(int apid, int& zone);     // the ABI channel of a picture APID, 0 otherwise; zone 0 full disk, 1 CONUS, 2 / 3 mesoscale 1 / 2
std::string abiProductName(int apid);
class GrbDecoder {
public:
    GrbDecoder();
    ~GrbDecoder();
    void reset();
    void pushCadu(const uint8_t cadu[kGrbCadu]);
    // a finished line (index, picture key = APID, 8-bit pixels); for the receiver's pictures
    std::function<void(int apid, int width, int height, int line, const uint8_t* px)> onLine;
    std::function<void(int apid, const std::string& what)> onProduct;           // "ABI ..." / "SUVI ..." / "GLM ..."
    std::function<void(const std::string& name, const std::vector<uint8_t>&)> onFile;   // metadata and text to save
    std::function<void(const Packet&)> onPacket;
    uint64_t crcBad = 0, packets = 0, payloads = 0, blocks = 0;
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};
// builds the packets of one image block / metadata payload (tests)
std::vector<std::vector<uint8_t>> grbImagePackets(int apid, uint16_t& seq, uint32_t seconds, int compression, const std::vector<uint8_t>& data, uint32_t leftX, uint32_t leftY, uint32_t rowOffset, uint32_t h, uint32_t w);
std::vector<std::vector<uint8_t>> grbGenericPackets(int apid, uint16_t& seq, uint32_t seconds, const std::vector<uint8_t>& data);

// ---- GK-2A UHRIT
constexpr int kUhritCadu = 2048;
class UhritDecoder {
public:
    UhritDecoder();
    ~UhritDecoder();
    void reset();
    void pushCadu(const uint8_t cadu[kUhritCadu]);
    // a complete xRIT file (primary header first)
    std::function<void(const std::vector<uint8_t>&, int vcid, int apid)> onFile;
    std::function<void(const Packet&)> onPacket;
    uint64_t crcBad = 0, frameCrcBad = 0, packets = 0, files = 0;
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};
std::string xritAnnotation(const std::vector<uint8_t>& file);        // the file name record, "" when absent
void buildUhritCadu(int scid, int vcid, uint32_t counter, const std::vector<uint8_t>& stream, size_t& pos, uint8_t cadu[kUhritCadu], const std::vector<size_t>& starts);

// ---- byte-aligned sync search for the CADUs inside BBFRAME data fields
class CaduStream {
public:
    void configure(int caduBytes) { n_ = caduBytes; buf_.clear(); }
    void reset() { buf_.clear(); }
    void push(const uint8_t* p, size_t n, const std::function<void(const uint8_t*)>& out);
private:
    std::vector<uint8_t> buf_;
    int n_ = 2048;
};

// ---- GVAR
constexpr int kGvarBytes = 32786, kGvarVisWidth = 20944, kGvarIrWidth = 5236, kGvarLines = 1354;
constexpr uint64_t kGvarSync = 0x1BE7D01FBF80FFFEULL;   // from gvar_deframer.h
struct GvarBlock {
    int id = -1, wordCount = 0, wordSize = 0, count = 0;
    bool crcOk = false;
};
void gvarScramble(uint8_t* frame);                                   // the same in both directions, byte 8 on
GvarBlock gvarHeader(const uint8_t* frame);                          // the triple header at 8, 38, 68: the CRC-good copy, else a majority vote
int gvarCounter(const uint8_t* frame);                               // the line counter of a picture block, -1 when above the full disk
int gvarIrWords(const uint8_t* frame);                               // the words of one IR line
void gvarVisPixels(const uint8_t* frame, uint16_t* px);              // kGvarVisWidth pixels, 10 bit
void gvarIrPixels(const uint8_t* frame, int words, uint16_t* a0, uint16_t* a1, uint16_t* b0, uint16_t* b1); // two lines of each of the two channels
void gvarBuildFrame(int blockId, int counter, const std::function<uint16_t(int ch, int y, int x)>& pixel, int scid, uint8_t* frame);

// ---- UHF cubesats
constexpr int kGeoscanData = 66;
bool geoscanFrame(uint8_t* data);                                    // descramble and CRC of the 66 bytes after the sync word; false: CRC failed
void geoscanBuild(const uint8_t* payload /* 64 */, uint8_t* out /* 66 */);
constexpr int kLucky7Bits = 312;
bool lucky7Frame(uint8_t* bytes /* 39, sync first */);               // descramble and CRC, payload 35 bytes at offset 2; false: CRC failed
void lucky7Build(const uint8_t* payload /* 35 */, uint8_t* out /* 39 */);
const char* lucky7Kind(const uint8_t* payload);
class SpinoDeframer {
public:
    std::function<void(const std::vector<uint8_t>&)> onFrame;
    void reset() { sr_ = 0; in_ = false; bit_ = 0; f_.clear(); }
    void push(int bit);
    uint64_t bad = 0;
private:
    uint32_t sr_ = 0;
    bool in_ = false;
    int bit_ = 0;
    uint8_t by_ = 0;
    std::vector<uint8_t> f_;
};
std::vector<uint8_t> spinoBuild(const std::vector<uint8_t>& head16, const std::vector<uint8_t>& body);

} // namespace dect2::sd3
