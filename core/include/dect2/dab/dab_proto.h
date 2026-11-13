// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The pure parsing layer of the DAB data decoders (ETSI EN 300 401, TS 101 756, EN 301 234, TS 102 563, TS 102 980, TS 102 979), written as plain structs and
// functions, implemented by the Rust crate rust/onair_dab (dab_proto_rs.cpp):
// the FIB walk and FIG parsers behind the ensemble tables and labels, MOT time / header / data group / directory, the PAD split of DAB+ access units and MP2
// frames, dynamic label framing and DL Plus, the Journaline data group and TOC check, and the packet mode headers. Ensemble tables, label segment stores,
// data group / MOT object assembly and the sub-channel protection tables stay in their classes (dab_rx.cpp, dab_mot.cpp, dab_pad.cpp, dab_journaline.cpp).
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace dect2::dabp {

uint16_t crc16(const uint8_t* p, size_t n);   // CRC-CCITT, start 0xFFFF, result inverted (FIB, data groups, packets, PAD)

// ---- FIB and FIG (EN 300 401 clause 5.2 and 8)
struct FigRef { int type = 0; uint32_t off = 0, len = 0; };           // a FIG of a FIB: its data field (header byte first)
std::vector<FigRef> fibWalk(const uint8_t* fib, size_t n, int& used);   // fib: the 30 data bytes; used: bytes taken (at most 30); length 0 FIGs are skipped
struct FigHead { int cn = 0, oe = 0, pd = 0, ext = 0; };
FigHead figHead(int type, const uint8_t* d, size_t n);                   // as the receiver files the FIG (FIG 0 / 1 / 2 / 5)
bool fig0Eid(const uint8_t* d, size_t n, int& eid);                      // FIG 0/0
struct Fig01Sub { int id = 0, start = 0; bool eep = false, uepTable = false; int option = 0, level = 0, size = 0, uepIndex = 0; };   // FIG 0/1 as sent
std::vector<Fig01Sub> fig0Subchannels(const uint8_t* d, size_t n);
struct Fig03Packet { int scid = 0; bool dg = false; int dscty = 0, subId = 0, address = 0; };   // FIG 0/3 (dg: data groups are used)
std::vector<Fig03Packet> fig0Packet(const uint8_t* d, size_t n);
struct Fig02Comp { int tmid = 0, ascty = -1, dscty = -1, subId = -1, scid = -1; bool primary = true; };
struct Fig02Service { uint32_t sid = 0; std::vector<Fig02Comp> comps; };
std::vector<Fig02Service> fig0Services(const uint8_t* d, size_t n);      // FIG 0/2
struct Fig013App { uint32_t sid = 0; int scids = 0, index = 0, type = 0; uint32_t off = 0, len = 0; };   // off / len: the application data in the FIG data field
std::vector<Fig013App> fig0UserApps(const uint8_t* d, size_t n);         // FIG 0/13
struct Fig017Pty { uint32_t sid = 0; bool sd = false, primary = false; int lang = -1, pty = -1; };
std::vector<Fig017Pty> fig0Pty(const uint8_t* d, size_t n);              // FIG 0/17
struct Fig018Support { uint32_t sid = 0, asu = 0; std::vector<int> clusters; };
std::vector<Fig018Support> fig0AnnSupport(const uint8_t* d, size_t n);   // FIG 0/18
struct Fig019Switch { int cluster = 0; uint32_t asw = 0; bool newFlag = false, region = false; int subId = 0; };
std::vector<Fig019Switch> fig0AnnSwitch(const uint8_t* d, size_t n);     // FIG 0/19
struct Fig010Time { uint32_t mjd = 0; int hour = 0, minute = 0, second = 0; int64_t utc = 0; };   // utc: 0 when the MJD is not after 1970-01-01
bool fig0Time(const uint8_t* d, size_t n, Fig010Time& t);                // FIG 0/10
struct Fig1Label { int ext = 0, charset = 0, scids = 0; uint32_t sid = 0, off = 0; };            // off: the 16 label bytes in the FIG data field
bool fig1(const uint8_t* d, size_t n, Fig1Label& l);                     // false: a label the receiver does not follow, or too short
struct Fig2Seg { int ext = 0, toggle = 0, seg = 0, rfu = 0, scids = 0; uint32_t sid = 0; bool ucs2 = false; int total = 0; bool valid = false; uint32_t textOff = 0, textLen = 0; };
bool fig2(const uint8_t* d, size_t n, Fig2Seg& s);                       // valid false: segment 0 with no text after its header fields

// ---- MOT (EN 301 234)
struct MotTime { bool valid = false, now = false, longForm = false; int mjd = 0, hour = 0, minute = 0, second = 0, ms = 0; };
size_t motParseTime(const uint8_t* d, size_t n, MotTime& t);             // bytes used, 0 when too short
std::string motTimeText(const MotTime& t);                               // "now", "-", "2026-10-06 12:00[:00]"
struct Range { uint32_t off = 0, len = 0; };
struct MotParamRef { int id = 0; Range data; };
struct MotHeader {
    uint64_t bodySize = 0, headerSize = 0; int contentType = 0, subtype = 0;
    std::vector<MotParamRef> params;
    bool hasName = false, hasSlide = false, hasTrigger = false, hasExpire = false;
    int categoryId = -1, slideId = -1;
    Range name, category, clickUrl, altUrl;      // byte ranges of the header (name: without the character set byte)
    std::string caption;                         // ContentDescription, clean UTF-8
    MotTime trigger, expire;
};
bool motParseHeader(const uint8_t* h, size_t n, MotHeader& o);           // false: shorter than 7 bytes
std::string motContentTypeName(int type, int subtype);
struct MotGroup { enum Status { Ignored = 0, Bad = 1, Scrambled = 2, Compressed = 3, Segment = 4 }; int status = Ignored; bool last = false; int type = 0, segNo = 0, tid = 0; uint32_t off = 0, len = 0; };
MotGroup motParseGroup(const uint8_t* d, size_t n);                      // an MSC data group of the MOT types (3..7): CRC, headers, segmentation header
struct DirEntry { int tid = 0; uint32_t off = 0, size = 0; };
bool motParseDirectory(const uint8_t* d, size_t n, std::vector<DirEntry>& out);   // false: shorter than 13 bytes

// ---- PAD (EN 300 401 clause 7.4, TS 102 563, TS 102 980)
struct PadBytes { std::vector<uint8_t> xpad; uint8_t fpad[2] = {0, 0}; };   // the X-PAD bytes in the order sent
bool padFromAu(const uint8_t* au, size_t n, PadBytes& out);                // the data stream element of a DAB+ access unit
bool padFromMp2(const uint8_t* f, size_t n, int& lastCont, PadBytes& out);   // an MP2 frame; lastCont is kept between frames
struct PadSub { int type = 0; uint32_t off = 0, len = 0; };
std::vector<PadSub> padSplit(const uint8_t* xp, size_t n, const uint8_t fpad[2], int& lastType);   // the application subfields of one X-PAD
int padDgLength(const uint8_t* d, size_t n, int& len);                    // data group length indicator: 0 too short, 1 bad CRC, 2 good
struct DlsStep { int action = 0; uint32_t segLen = 0; };                  // 0 keep collecting, 1 stop, 2 stop with a segment of segLen bytes (no CRC)
DlsStep dlsStep(const uint8_t* d, size_t n);
struct DlSegment { int toggle = 0; bool first = false, last = false, cmd = false; int code = 0, segNo = 0, charset = 0; };
DlSegment dlSegment(const uint8_t* d, size_t n);
struct DlpTag { int type = 0, start = 0, len = 0; };
bool dlPlusTags(const uint8_t* d, size_t n, bool& running, std::vector<DlpTag>& out);   // false when too short

// ---- Journaline framing and packet mode (TS 102 979, EN 300 401 clause 5.3.2, ES 201 980 clause 6.6)
struct JlGroup { int status = 1; int type = 0; uint32_t fieldLen = 0; };    // status 0 accepted, 1 rejected, 2 wrong CRC; the field starts at byte 2
JlGroup jlDataGroup(const uint8_t* d, size_t n);
struct JlTocEntry { uint32_t id = 0, desc = 0; };
struct JlTocInfo { int revision = 0, blockCount = 0, blockIndex = 0, objectCount = 0, timeoutMin = 0; };
bool jlParseToc(const uint8_t* d, size_t n, JlTocInfo& info, std::vector<JlTocEntry>& entries);
bool jlUserAppData(const uint8_t* d, size_t n, int& version, int& extHeader, int& entry);
struct DabPacket { bool crcOk = false, first = false, last = false, cmd = false; int ci = 0, addr = 0; uint32_t ul = 0; };
DabPacket dabPacket(const uint8_t* p, size_t n);
struct DrmPacket { bool crcOk = false, first = false, last = false, lenBad = false; int id = 0, ci = 0; uint32_t n = 0, at = 0; };
DrmPacket drmPacket(const uint8_t* p, size_t n);

} // namespace dect2::dabp
