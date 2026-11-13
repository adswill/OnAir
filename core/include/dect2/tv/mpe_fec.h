// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// DVB-H data link layer (ETSI EN 301 192 clauses 7 and 9): multiprotocol encapsulation (MPE sections, table_id 0x3E) carrying IP
// datagrams, time slicing (the real-time parameters: delta-t to the next burst, the table and frame boundaries, the address) and MPE-FEC
// (sections 0x78 carrying the columns of an RS(255,191) code over the rows of the MPE-FEC frame).
//
// MpeDemux takes transport stream packets of any PIDs, finds the elementary streams that carry MPE or MPE-FEC sections by themselves,
// rebuilds each MPE-FEC frame (application data table + RS data table, with every byte marked reliable or not by the section CRCs),
// corrects the rows with erasure decoding, and hands out the IP datagrams. Streams without MPE-FEC give their datagrams straight from
// the sections. MpeEncoder is the transmit side (the test generator).
#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <vector>

namespace dect2 {
namespace mpe {

// ---- RS(255,191, t = 32): field x^8 + x^4 + x^3 + x^2 + 1, generator roots alpha^0 .. alpha^63 (9.5.1)
void rsEncode255(const uint8_t* in191, uint8_t* parity64);
// Corrects word[255] in place; erasure positions (0..254) count once each, errors twice: 2 e + s <= 64. Returns the number of bytes
// changed, or -1 when the word cannot be corrected.
int rsDecode255(uint8_t* word, const int* erasures, int nErasures);

struct RtParams {
    int deltaT = 0;              // 12 bits: time slicing: to the next burst in 10 ms; MPE-FEC only: the frame counter
    bool tableBoundary = true, frameBoundary = false;
    int address = 0;             // 18 bits: byte position in the MPE-FEC frame table
};
inline int rowsOfFrameSize(int frameSize) { return frameSize >= 0 && frameSize <= 3 ? 256 * (frameSize + 1) : 0; }

struct Stats {
    uint64_t sections = 0, crcErrors = 0, fecSections = 0, frames = 0;
    uint64_t rowsClean = 0, rowsCorrected = 0, rowsFailed = 0, bytesCorrected = 0;
    uint64_t datagrams = 0, datagramsBad = 0, bursts = 0;
    int pid = -1;                // the last stream heard (-1: none yet)
    int rows = 0;                // MPE-FEC frame rows (0: no MPE-FEC seen)
    int lastDeltaTms = -1;       // time slicing: the last delta-t in ms
    bool timeSlicing = false, mpeFec = false;
};

class MpeDemux {
public:
    // datagram, its length, whether every byte of it is known good, and the PID it came on
    using DatagramFn = std::function<void(const uint8_t* dgram, size_t len, bool reliable, int pid)>;
    MpeDemux();
    ~MpeDemux();
    void setDatagramCallback(DatagramFn cb) { cb_ = std::move(cb); }
    void feed(const uint8_t* packets, size_t count);   // 188-byte packets (transport_error_indicator honoured)
    void flush();                                       // finishes a frame still open (end of a recording)
    const Stats& stats() const { return st_; }
    void reset();
    // The MAC_address_4..1 bytes hold the real-time parameters (time slicing or MPE-FEC in use: the TPS bits s48 / s49 of DVB-H say so).
    // Without the hint they are taken as such once an MPE-FEC section has been seen on the stream.
    void setRealTimeParams(bool on) { rtHint_ = on; }
private:
    bool rtHint_ = false;
    struct Stream;
    std::map<int, std::unique_ptr<Stream>> streams_;
    Stats st_;
    DatagramFn cb_;
    void section(Stream& s, const uint8_t* sec, size_t len);
    void finishFrame(Stream& s);
    void emitTable(Stream& s, const std::vector<uint8_t>& adt, const std::vector<uint8_t>& ok, size_t used);
};

// ---- transmit side
struct EncoderConfig {
    int pid = 0x200;
    int rows = 256;               // 256, 512, 768, 1024
    bool mpeFec = true;
    bool timeSlicing = true;
    int punctured = 0;            // RS columns not sent (0..63)
    int deltaTms = 1000;          // time slicing: the time to the next burst
    uint8_t mac[6] = {0x01, 0x00, 0x5E, 0x00, 0x00, 0x01};
};
class MpeEncoder {
public:
    explicit MpeEncoder(const EncoderConfig& c) : c_(c) {}
    // One MPE-FEC frame (one burst) of datagrams (they must fit the application data table) as transport packets appended to out.
    // Returns false when they do not fit.
    bool frame(const std::vector<std::vector<uint8_t>>& datagrams, std::vector<uint8_t>& out);
private:
    EncoderConfig c_;
    int cc_ = 0, frameNo_ = 0;
    void packetise(const std::vector<uint8_t>& sec, std::vector<uint8_t>& out);
};

// a test IPv4/UDP datagram of `len` bytes (len >= 28) whose payload follows from seq (checkTestDatagram recognises it)
std::vector<uint8_t> testDatagram(uint32_t seq, size_t len);
bool checkTestDatagram(const uint8_t* d, size_t len, uint32_t* seq);

} // namespace mpe
} // namespace dect2
