// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// EchoLink wire formats: the directory (station list) server protocol over TCP and the RTP / RTCP packets peers exchange over UDP.
// Layouts follow SvxLink's echolib (EchoLinkDirectory.cpp, EchoLinkQso.cpp, rtpacket.cpp; GPL-2.0-or-later, Tobias Blomberg SM0SVX),
// which interoperates with the EchoLink network; see THIRD_PARTY.md. No byte here comes from a captured EchoLink session: the tests check
// these layouts against a local mock, never against the real servers.
//
// UDP, two ports per station (defaults 5198 audio, 5199 control):
//   control port: RTCP with the old "version 3" header bits (first byte 0xC0): an empty receiver report followed by
//                 SDES (CNAME "CALLSIGN", NAME "<call padded to 15><name>", EMAIL "CALLSIGN", PHONE "08:30", optional PRIV) or BYE
//                 ("jan2002"). SDES is both the connect request and the keep-alive (every 10 s; no SDES for 50 s = gone).
//   audio port:   RTP: 0xC0, payload type 3 (GSM 06.10), sequence number, timestamp 0, SSRC 0, then 4 GSM frames of 33 bytes (80 ms).
//                 Text packets start with "oNDATA": "oNDATA\r<station info>" (info / a conference's list of who is connected) or
//                 "oNDATA<CALL>><message>\r\n" (chat), both with a terminating NUL.
// TCP directory (port 5200), one command per connection:
//   log in / status:  "l" CALL 0xAC 0xAC PASSWORD "\r" ("ONLINE3.38(HH:MM)" | "BUSY3.40(HH:MM)" | "OFF-V3.40") "\r" LOCATION "\r"  -> "OK..."
//   station list:     "s" -> "@@@\n" COUNT "\n" then per entry four lines (callsign, "description [ON|BUSY HH:MM]", node id, IP)
//                     and "+++". An entry with callsign " " is a server message (e.g. "INCORRECT PASSWORD"), "." a separator.
#pragma once
#include "dect2/echolink/gsm610.h"
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace dect2 {
namespace echolink {

constexpr int kDefaultAudioPort = 5198;     // control = audio + 1
constexpr int kDirectoryPort = 5200;
constexpr int kFramesPerPacket = 4;         // 4 x 20 ms GSM frames per RTP packet
constexpr int kRtpHeaderBytes = 12;
constexpr int kAudioPacketBytes = kRtpHeaderBytes + kFramesPerPacket * kGsmFrameBytes;   // 144
constexpr uint8_t kPtGsm = 0x03;
constexpr uint8_t kPtSpeex = 0x96;          // used by some peers; refused here (OnAir sends and asks for GSM only)
constexpr int kKeepAliveMs = 10000;
constexpr int kConnectTimeoutMs = 50000;    // no SDES from the peer for this long: the connection is gone
constexpr int kConnectRetries = 5;
constexpr size_t kMaxLocation = 27;         // the directory shows at most 27 characters of the location
constexpr int kRegistrationRefreshMs = 5 * 60 * 1000;   // the log-in is repeated this often while online          // SDES sent while connecting (one per keep-alive period) before giving up

// ---- RTCP (control port) ----
// SDES: what a station sends to connect and to keep the connection. priv: optional PRIV item ("" = none).
std::vector<uint8_t> makeSdes(const std::string& call, const std::string& name, const std::string& priv = std::string());
std::vector<uint8_t> makeBye();
bool isRtcpBye(const uint8_t* p, size_t n);
bool isRtcpSdes(const uint8_t* p, size_t n);
// The text of SDES item `item` (1 CNAME, 2 NAME, 8 PRIV ...); false when the packet has no such item. Never reads past n.
bool sdesItem(const uint8_t* p, size_t n, int item, std::string& out);
// The NAME item "LY1ABC         Kris": the callsign (sanitised) and the name. False when no callsign is left.
bool splitSdesName(const std::string& nameItem, std::string& call, std::string& name);
// Letters, digits, - / * _ only, at most 32 characters (the remote station controls this text)
std::string sanitizeCallsign(const std::string& s);

// ---- RTP and text (audio port) ----
std::vector<uint8_t> makeAudioPacket(uint16_t seq, const uint8_t frames[kFramesPerPacket][kGsmFrameBytes]);
enum class AudioPortKind { Audio, Info, Chat, Unknown };
AudioPortKind classifyAudioPort(const uint8_t* p, size_t n);
// An RTP GSM packet: seq and up to 4 frames (frames points into p). False for a short packet or another payload type.
bool parseAudioPacket(const uint8_t* p, size_t n, uint16_t& seq, uint8_t& pt, const uint8_t*& frames, int& nframes);
std::vector<uint8_t> makeInfoPacket(const std::string& info);                        // '\n' sent as '\r'
std::vector<uint8_t> makeChatPacket(const std::string& call, const std::string& message);
bool parseInfoPacket(const uint8_t* p, size_t n, std::string& info);                 // '\r' returned as '\n'
bool parseChatPacket(const uint8_t* p, size_t n, std::string& text);                 // "CALL>message" ('\r' as '\n', trailing newline dropped)

// ---- directory (TCP) ----
enum class DirStatus { Online, Busy, Offline };
// hhmm: the local time "HH:MM" shown next to the station; location: the text shown in the list (CR / LF removed, cut to kMaxLocation)
std::string loginCommand(const std::string& call, const std::string& password, DirStatus st, const std::string& hhmm, const std::string& location);
// The same with the password replaced by "****", for logs and diagnostics
std::string loginCommandRedacted(const std::string& call, DirStatus st, const std::string& hhmm, const std::string& location);
inline const char* listCommand() { return "s"; }
// EchoLink callsigns: letters and digits with a digit, optional -L / -R suffix (links and repeaters), or *CONFERENCE*; upper case
bool validCallsign(const std::string& call);
std::string upperCall(const std::string& s);

enum class StationKind { User, Link, Repeater, Conference };
struct Station {
    std::string call, description, ip;
    std::string time;            // "HH:MM" the station reported ("" when none)
    int id = -1;                 // the node number
    bool busy = false, online = false;
    StationKind kind = StationKind::User;
};
StationKind stationKind(const std::string& call);
const char* stationKindName(StationKind k);    // "Station", "Link", "Repeater", "Conference"

// Reads the "s" answer as it arrives, in any chunk sizes. done() after "+++"; failed() on a malformed answer.
class StationListParser {
public:
    void reset();
    void feed(const char* p, size_t n);
    bool done() const { return st_ == Done; }
    bool failed() const { return st_ == Failed; }
    const std::vector<Station>& stations() const { return out_; }
    const std::string& message() const { return msg_; }     // server text lines (callsign " "), e.g. "INCORRECT PASSWORD"
    bool badPassword() const;
private:
    enum State { Start, Count, Call, Data, Id, Ip, End, Done, Failed };
    void line(const std::string& l);
    State st_ = Start;
    std::string buf_;
    int left_ = 0;
    Station cur_;
    std::vector<Station> out_;
    std::string msg_;
};

// Search: case-insensitive substring of the callsign, description or node number; kinds: bit mask of 1 << StationKind (0 = all)
std::vector<Station> searchStations(const std::vector<Station>& all, const std::string& query, unsigned kinds = 0);

} // namespace echolink
} // namespace dect2
