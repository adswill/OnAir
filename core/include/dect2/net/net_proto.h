// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The pure protocol layers of the network code, over bytes and text with no socket: the remote-control API's HTTP / WebSocket side (SHA-1 and base64 for the handshake,
// frames, the request head, URL and query text, flat JSON bodies), the rtl_tcp, SpyServer and SDR++ server protocols (greetings, commands, the SDR++ menu draw list) and
// the list of network radios the settings keep. Implemented by the Rust crate onair_net
// through core/src/net/net_proto_rs.cpp. The callers (remote_api.cpp, source_rtltcp.cpp, source_spyserver.cpp, source_sdrpp.cpp, netradio.cpp) keep the sockets, the threads
// and the state; tests/net/test_net_proto_rs.cpp is the golden-vector test.
#pragma once
#include "dect2/net/remote_api.h"   // JsonVal
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace dect2::netp {

// ---------------------------------------------------------------- SHA-1, base64, WebSocket (RFC 3174, RFC 4648, RFC 6455)
std::string sha1(const std::string& data);                 // 20 raw bytes
std::string base64(const std::string& data);
std::string wsAcceptKey(const std::string& clientKey);     // base64(sha1(key + GUID)), 4.2.2
std::string wsFrame(int opcode, const std::string& payload);   // a server frame: FIN set, not masked
// The first client frame of the bytes received so far. kind: 0 need more bytes, 1 close the connection with `code` (1002 unmasked, 1003 fragmented / binary / reserved
// opcode, 1009 too big or a 64-bit length), 2 a whole frame (`used` bytes of the input, opcode, FIN, unmasked payload; at most 4096 bytes)
struct WsClient { int kind = 0, code = 0, op = 0; bool fin = false; size_t used = 0; std::string payload; };
WsClient wsParseClient(const std::string& in);

// ---------------------------------------------------------------- URL and header text
std::string urlDecode(const std::string& s);               // %XX and '+'
std::string queryParam(const std::string& q, const std::string& key);   // "" when there is none
std::string hostOnly(const std::string& h);                // "127.0.0.1:8765" -> "127.0.0.1", "[::1]:80" -> "[::1]"
bool loopbackName(const std::string& n);                   // localhost, [::1], 127.x.y.z
bool ctEqual(const std::string& a, const std::string& b);  // time does not depend on where they differ
std::string statusText(int code);                          // reason phrase of the status codes the API sends ("Error" for another)
bool parseFlatJson(const std::string& text, std::map<std::string, JsonVal>& out);   // one object, no nesting, at most 32 keys

// ---------------------------------------------------------------- HTTP request head
// Where the head ends in the bytes received: 0 not yet, 1 found (end = offset of the CR LF CR LF), 2 longer than 8192 bytes (431)
int httpScan(const std::string& buf, size_t& end);
struct HttpHead {
    std::string method, path, query;
    std::map<std::string, std::string> h;   // lower-case names; the last of a repeated one wins
    size_t contentLength = 0;
};
struct HttpError { int status = 0; std::string msg; };   // status 0: the head is fine
// The head without the empty line: request line, header fields (at most 48), Content-Length (at most 4096), chunked bodies refused, POST needs a length
HttpError httpHead(const std::string& head, HttpHead& out);

// ---------------------------------------------------------------- rtl_tcp
struct RtlGreeting { std::string magic; uint32_t tuner = 0, nGains = 0; bool other = false; };   // other: not an RTL-SDR
bool rtlGreeting(const unsigned char* hdr, size_t n, RtlGreeting& g);   // the 12 byte greeting; false: not an rtl_tcp server
std::vector<int> rtlTunerGains(int tuner);                 // tenths of a dB
std::string rtlTunerName(int tuner);
std::string rtlCommand(uint8_t cmd, uint32_t value);       // 5 bytes
bool splitHostPort(const std::string& addr, std::string& host, int& port);   // "host:port", "[::1]:1234"; the port 1 to 65535

// ---------------------------------------------------------------- SpyServer (protocol 2.0.1700)
void spyDeviceInfo(const unsigned char* b, uint32_t n, uint32_t out[12]);   // the 12 words of DEVICE_INFO (missing ones 0)
void spyClientSync(const unsigned char* b, uint32_t n, uint32_t out[9]);    // the 9 words of CLIENT_SYNC
std::string spyCommand(uint32_t type, const unsigned char* body, uint32_t n);
std::string spyHello();
std::string spySetSetting(uint32_t id, uint32_t value);
bool spyHeader(const unsigned char* h20, uint32_t& type, uint32_t& flags, uint32_t& size);   // false: the body would be over 1 MiB
std::vector<double> spyRates(uint32_t maxRate, uint32_t decStages, uint32_t minDec);       // the radio's full rate / 2^n, fastest first
std::string spyDevName(uint32_t deviceType);

// ---------------------------------------------------------------- SDR++ server
struct UiWidget {
    enum Kind { Combo, Checkbox, Radio, SliderInt, SliderFloat, InputInt, Text } kind = Text;
    std::string id, label;
    std::vector<std::string> items;
    int i = 0;
    bool b = false;
    double value = 0, minV = 0, maxV = 0, step = 0;
    bool disabled = false;
};
bool sdrppParseUi(const unsigned char* p, size_t n, std::vector<UiWidget>& out);   // the menu from the draw list; false: cut short or an unknown element
enum { kWordSr = 1, kWordBw = 2, kWordLna = 4, kWordVga = 8, kWordMix = 16, kWordMixer = 32, kWordIf = 64, kWordLin = 128, kWordSens = 256 };
unsigned sdrppIdFlags(const std::string& id);              // which of those words the control's id holds
std::string sdrppLabelFromId(const std::string& id);        // the readable name made from a control's id
double sdrppRateText(const std::string& text);             // "2.5MHz" -> Hz, 0 when it is not a rate
std::string sdrppCommand(uint32_t cmd, const unsigned char* arg, size_t n);   // a command packet
std::string sdrppUiAction(const std::string& id, uint8_t type, int32_t i, float f, bool b, bool sync);   // the argument of UI_ACTION (type 1 bool, 2 int, else float)
bool sdrppPacketHeader(const unsigned char* h8, uint32_t& type, uint32_t& bodySize);   // false: invalid type or size
bool sdrppErrorText(uint8_t code, std::string& text);       // false: no text for this code

// ---------------------------------------------------------------- network radios
struct Radio { uint32_t kind = 1; std::string host; int port = 0; std::string remote; };   // kind: 0 Pluto, 1 rtl_tcp, 2 SoapyRemote, 3 SpyServer, 4 HL2, 5 Red Pitaya, 6 SDR++, 7 nRSP-ST
std::vector<Radio> radiosFromString(const std::string& s);
std::string radiosToString(const std::vector<Radio>& v);
std::string cleanHost(const std::string& typed);           // no blanks or separators, no leading "ip:" / "tcp://"
std::string radioTypeName(uint32_t kind);
int radioDefaultPort(uint32_t kind);
std::string radioDefaultHost(uint32_t kind);
int radioPort(uint32_t kind, int port);                    // the port connected to

} // namespace dect2::netp
