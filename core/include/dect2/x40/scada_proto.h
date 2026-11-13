// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// SCADA radio telemetry (x40 variant Scada, docs/SCADA-4.1.md): the protocol side. Receive only: this file reads Modbus RTU and DNP3 frames
// out of byte bursts, pairs requests with responses, keeps the masters and outstations seen with the points they reported, and writes the
// CSV and JSON exports. There is nothing here that builds a frame to send. Frames it does not know (proprietary framings, DNP3 Secure
// Authentication) are kept as raw bytes and named as such; nothing is guessed.
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace dect2::scada {

uint16_t crcModbus(const uint8_t* p, size_t n);   // CRC-16/MODBUS: reflected 0x8005, init 0xFFFF; sent low byte first
uint16_t crcDnp(const uint8_t* p, size_t n);      // CRC-16/DNP: reflected 0x3D65, init 0, complemented; sent low byte first
uint16_t crcHdlc(const uint8_t* p, size_t n);     // CRC-16/X.25 (the HDLC FCS): reflected 0x1021, init 0xFFFF, complemented; low byte first

enum Proto : int { Modbus = 0, Dnp3 = 1, Unknown = 2 };

// One frame found in a burst of bytes
struct Found {
    int proto = Unknown;
    size_t start = 0, len = 0;
    bool crcOk = false;
    int badBlock = -1;               // DNP3: the first data block whose CRC failed (-1 none)
};
// Finds Modbus RTU and DNP3 frames in a burst, left to right. A Modbus frame needs a valid CRC, an address of 0 - 247, a known function
// code and the exact length that function gives; a frame of another function counts only when it is the whole burst. A DNP3 frame needs
// 0x05 0x64 and a valid header CRC; its data blocks may fail (crcOk false, badBlock set). When nothing is found and the whole burst has the
// exact length a Modbus header announces, it comes back once with crcOk false (a damaged Modbus frame).
std::vector<Found> findFrames(const uint8_t* p, size_t n);

// The Modbus lengths a frame starting with p (n bytes available) can have: requests and responses. Empty for an unknown function.
std::vector<size_t> modbusLengths(const uint8_t* p, size_t n);
bool modbusKnownFunction(int fc);
const char* modbusFunctionName(int fc);
const char* modbusExceptionName(int code);
const char* dnpFunctionName(int fc);
const char* dnpLinkFunctionName(bool primary, int fc);
std::string dnpIinText(int iin1, int iin2);
std::string hex(const uint8_t* p, size_t n, size_t maxBytes = 512);

struct Message {
    uint64_t id = 0;
    double timeSec = 0;              // stream time of the frame's last byte
    int64_t wallMs = 0;              // the computer's clock when it was decoded (ms since 1970, UTC)
    std::string link;                // the physical layer and framing it came on, e.g. "Bell 202 AFSK 1200 Bd, 8E1"
    int proto = Unknown;
    std::string from, to;            // "master", "slave 17", "master 1", "outstation 10", "?"
    std::string func, summary;
    int crc = -1;                    // 1 OK, 0 failed, -1 not applicable (unknown framing)
    int dir = 0;                     // 1 request (master to outstation), 2 response, 3 unsolicited, 0 other
    uint64_t pair = 0;               // the id of the message it answers or that answers it (0 none)
    double rttMs = -1;               // response time, on the response
    bool encrypted = false;          // DNP3 Secure Authentication: shown, not decoded
    std::vector<uint8_t> raw;
};

struct Point { std::string value; double timeSec = 0; };

struct Device {
    int proto = Modbus;
    int addr = -1;                   // Modbus: slave address (the master is -1); DNP3: link address
    std::string role;                // "master", "slave", "outstation"
    double firstSec = 0, lastSec = 0, lastPollSec = -1;
    int polls = 0, replies = 0, exceptions = 0, crcBad = 0, unanswered = 0, unsolicited = 0;
    double rttSumMs = 0; int rttN = 0;
    std::string lastIin;
    std::vector<int> peers;          // the addresses it talked to
    std::map<std::pair<int, int>, Point> points;   // (table, index) -> last value; tables: see tableName()
};
const char* tableName(int table);    // 0 coil, 1 discrete input, 3 input register, 4 holding register; 10 BI, 11 BO, 12 counter, 13 frozen counter, 14 AI, 15 AO, 16 double-bit input
std::string pointLabel(int proto, int table, int index);   // "40108 (holding 107)" or "AI 3"

// What the receiver gives the tracker: one frame (or one burst not recognised)
struct Frame {
    double timeSec = 0;
    std::string link;
    int proto = Unknown;
    bool crcOk = false;
    int badBlock = -1;
    std::vector<uint8_t> bytes;
    std::string note;                // for an unknown burst: how it was read (async bytes, bits)
};

class Tracker {
public:
    void clear();
    uint64_t add(const Frame& f);    // returns the message id (0 when nothing was added, e.g. a DNP3 segment that only starts a fragment)
    const std::deque<Message>& messages() const { return msgs_; }   // oldest first
    std::vector<Device> devices() const;
    const Message* find(uint64_t id) const;
    void relabel(uint64_t id, const std::string& link);   // the same frame came through a stricter framing: name that link
    std::string csv() const;
    std::string json() const;
    struct Counts { uint64_t modbus = 0, dnp3 = 0, unknown = 0, crcBad = 0, paired = 0, encrypted = 0; } counts;
    static constexpr size_t kMaxMessages = 5000, kMaxPoints = 4096;
private:
    struct PendingMb { uint64_t id = 0; int fc = 0; double t = 0; int start = 0, qty = 0; std::vector<uint8_t> bytes; };
    struct PendingDnp { uint64_t id = 0; double t = 0; int fc = 0; };
    std::deque<Message> msgs_;
    std::map<std::pair<int, int>, Device> dev_;
    std::map<int, PendingMb> mbPending_;                                    // by slave address
    std::map<std::pair<std::pair<int, int>, int>, PendingDnp> dnpApp_;      // (master, outstation), seq -> request
    std::map<std::pair<int, int>, PendingDnp> dnpLink_;                     // (src, dst) -> link request waiting for its reply
    std::map<std::pair<int, int>, std::vector<uint8_t>> dnpTransport_;     // (src, dst) -> application fragment so far
    std::map<std::pair<int, int>, int> dnpTseq_;
    uint64_t nextId_ = 1;
    Message& push(Message m);
    Device& device(int proto, int addr, const char* role, double t);
    void addModbus(const Frame& f, Message& m);
    void addDnp(const Frame& f, Message& m);
    void dnpApp(const std::vector<uint8_t>& a, int src, int dst, bool fromMaster, Message& m);
};

// The tracker of the running mode, shared with the screen (the decoder writes on the analysis thread, the screen copies under the lock).
struct Store {
    std::mutex mu;
    Tracker tr;
    std::atomic<uint64_t> version{0};
    std::string phy;                 // one line on the physical layer last heard
};
Store& store();

} // namespace dect2::scada
