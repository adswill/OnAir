// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The pure parsing layer of the MPEG transport-stream analyser (ts.cpp): packet header, section CRC-32 (ISO/IEC 13818-1), PAT / PMT / SDT / NIT / EIT / TDT
// section parsers (ETSI EN 300 468) and the ATSC virtual channel table (A/65). Text fields come back as byte ranges (Span) of the section: the character
// set conversion (dvbText, iconv) and every table of state stay in ts.cpp. These functions are implemented by the Rust crate
// rust/onair_ts (ts_proto_rs.cpp).
#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace dect2::tsp {

struct Span { uint32_t off = 0, len = 0; };   // a byte range of the section

uint32_t crc32(const uint8_t* p, int n);   // MPEG-2 section CRC-32; 0 over a section that includes its own CRC

struct PacketHeader { int pid = 0; bool tei = false, pusi = false; int scr = 0, afc = 0, cc = 0, payloadOff = -1; };
// false: not a packet (n < 4 or no 0x47 sync). payloadOff -1: no payload, or the adaptation field fills the packet.
bool packetHeader(const uint8_t* p, int n, PacketHeader& h);
// The 188 byte PAT packet of a one-program stream (cc: continuity counter, low 4 bits used).
void patPacket(int tsid, int sid, int pmtPid, int cc, uint8_t out[188]);

struct PatEntry { int prog = 0, pid = 0; };
bool parsePat(const uint8_t* s, int len, int& tsid, std::vector<PatEntry>& out);   // false: len < 8; program 0 (network PID) is skipped

struct PmtSub { bool teletext = false; std::string lang; int type = 0, page = 0, ancPage = 0; };
struct PmtStream { int pid = 0, streamType = 0; std::string kind, codec, lang; std::vector<PmtSub> subs; };   // kind: video audio subtitles teletext data
struct Pmt { int prog = 0, pcrPid = 0; bool ca = false; std::vector<PmtStream> streams; };
bool parsePmt(const uint8_t* s, int len, Pmt& out);   // false: len < 12

struct SdtService { int sid = 0; bool freeCa = false, hasType = false, hasProvider = false, hasName = false; int type = 0; Span provider, name; };
bool parseSdt(const uint8_t* s, int len, int& tsid, int& onid, std::vector<SdtService>& out);   // false: len < 11

struct NitLcn { int sid = 0, lcn = 0; };
struct Nit { bool hasName = false; Span name; std::vector<NitLcn> lcn; };
bool parseNit(const uint8_t* s, int len, Nit& out);   // false: len < 10

struct Tvct { std::string name; int major = 0, minor = 0, prog = 0, stype = 0; };
bool parseTvct(const uint8_t* s, int len, int& tsid, std::vector<Tvct>& out);   // false: len < 14

struct Tdt { int64_t unixSec = 0; std::string text; };   // text: "YYYY-MM-DD hh:mm:ss UTC"
bool parseTdt(const uint8_t* s, int len, Tdt& out);   // false: len < 8

struct EitEvent {
    int eventId = 0; int64_t start = 0; int duration = 0, running = 0, genre = -1;
    std::string lang;                      // 3 bytes, empty when there was no short event descriptor
    bool hasTitle = false, hasText = false;
    Span title, text;
    uint16_t extMask = 0;                  // bit n: ext[n] present (descriptor_number n; a later descriptor with the same number replaces it)
    std::array<Span, 16> ext{};
};
struct Eit { int sid = 0, secNum = 0; bool legacy = false, hasName = false; Span nowNextName; std::vector<EitEvent> events; };
bool parseEit(const uint8_t* s, int len, Eit& out);   // false: len < 18; events with no start time or no duration are left out

} // namespace dect2::tsp
