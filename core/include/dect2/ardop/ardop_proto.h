// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// ARDOP (Amateur Radio Digital Open Protocol, Rick Muething KN6KB) frame layer: the frame type table, the type parity, the CRC-16, the
// Reed-Solomon code, the six-bit character packing of call signs, and the frame bodies of the 4FSK frames (ID, connect request, ping,
// connect acknowledge, ping acknowledge, ACK / NAK, 4FSK data). Clean-room from the ARDOP specification (frame type table, appendix B)
// and the published behaviour of the open implementations; the code is new. Monitor use only: nothing here opens a connection.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace dect2 {
namespace ardop {

enum Kind { KUnknown = 0, KAck, KNak, KBreak, KIdle, KDisc, KEnd, KConRejBusy, KConRejBw, KId, KConReq, KConAck, KPing, KPingAck, KData };

struct FrameDef {
    int code = 0;
    Kind kind = KUnknown;
    std::string name;       // "ConReq500M", "4FSK.500.100.E", "DataACK"
    const char* mod = "";   // "4FSK", "4PSK", "8PSK", "16QAM" for data frames
    int carriers = 0, baud = 0, bandwidth = 0;   // bandwidth: Hz class the frame belongs to (200, 500, 1000, 2000)
    int dataBytes = 0, rsBytes = 0;              // per carrier
    bool odd = false;       // data frames: the even / odd toggle
    bool fsk = false;       // 4FSK at 50 or 100 baud: the body can be read by this receiver
};

// false: not a frame type of the table
bool frameDef(int code, FrameDef& out);
// ACK / NAK quality 38 .. 100 (steps of 2) carried in the type
int ackQuality(int code);

// The parity symbol sent after each frame type byte (both bytes use the parity of the first)
uint8_t typeParity(uint8_t type);

// CRC-16 as ARDOP computes it (seed FFFF, x^16 + x^12 + x^5 + 1, data bits shifted in at the bottom, no final augmentation)
uint16_t crc16(const uint8_t* d, size_t n);

// Reed-Solomon over GF(256) (polynomial 0x11D, roots alpha^1 .. alpha^rs), shortened, systematic: the parity bytes follow the data
void rsAppend(std::vector<uint8_t>& block, int rs);          // block holds the data; the parity is appended
// block = data + parity. Returns the number of corrected bytes, or -1 when uncorrectable
int rsCorrect(uint8_t* block, int len, int rs);

// Six-bit packing of eight characters (space .. underscore, lower case folded up) in six bytes
std::array<uint8_t, 6> packed6(const std::string& s);
std::string unpacked6(const uint8_t* b);                      // eight characters, trailing spaces removed

// ---- frame bodies (what follows the two type bytes); sid = session ID (0xFF unconnected)
std::vector<uint8_t> makeIdFrame(const std::string& call, const std::string& locator);
std::vector<uint8_t> makeCallFrame(int code, const std::string& from, const std::string& to);   // ConReq (0x31 .. 0x38) and Ping (0x3E)
std::vector<uint8_t> makeConAck(int code, int leaderMs, uint8_t sid);
std::vector<uint8_t> makePingAck(int snrDb, int quality);
std::vector<uint8_t> makeControl(int code, uint8_t sid);                // BREAK, IDLE, DISC, END, ConRejBusy, ConRejBW, ACK, NAK (no body)
// one carrier of a 4FSK data frame: count, data (zero padded), CRC, RS. false: not a 4FSK data frame or payload too long
bool makeDataBlock(int code, const std::vector<uint8_t>& payload, std::vector<uint8_t>& block);
// the whole frame as bytes: [type][type ^ sid][body...]
std::vector<uint8_t> makeFrameBytes(int code, uint8_t sid, const std::vector<uint8_t>& body);

// ---- reading
struct Parsed {
    int code = 0;
    uint8_t sid = 0xFF;
    bool bodyOk = false;        // the body was complete and passed its check (RS / CRC / repetition)
    int rsFixed = 0;
    std::string from, to, locator;
    int leaderMs = -1;          // ConAck
    int snrDb = -1000, quality = -1;   // PingAck; ACK / NAK carry quality in the type
    std::vector<uint8_t> payload;
};
// number of body bytes (after the two type bytes) that follow a frame of this type on the same carrier, 0 for control frames; -1 when not readable
int bodyLength(int code);
// body = bodyLength(code) bytes
bool parseBody(int code, const uint8_t* body, size_t n, Parsed& out);

} // namespace ardop
} // namespace dect2
