// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// KAVB video / audio layer (spec section 6): the container that carries AV1 and Opus payload packets over the PHY frames, with FEC groups and a bit budget.
//
// No codec lives here: video and audio enter as opaque byte units through the PayloadSource / PayloadSink interfaces (an AV1 encoder, an Opus encoder or, in the
// tests, synthetic bytes). Layers, bottom up:
//   * PHY frame = 4 LDPC blocks of blockPayloadBytes (178; 118 robust), each with its own CRC-16 -> a block is a "cell" (lost when its CRC fails);
//   * cell = 1 byte "offset of the first packet that starts in this cell" (0xFF = none) + (cellBytes - 1) bytes of stream;
//   * FEC group = G data cells + P parity cells (Reed-Solomon erasure code over GF(256), Cauchy matrix; any G of the G + P cells restore the group; the erasure code is Rust: rust/onair_kris/src/erasure.rs). N = G + P is a power
//     of two times 4... (4, 8, 16, 32, 64) so a group is a whole number of frames and the 16-bit frame sequence number wraps on a group boundary;
//   * packets: [type:4 | stream:4][seq:16][len:16][payload][CRC-32 over header + payload] (KAAL section 6.2), packed one after the other into the data cells of a group;
//     a packet never crosses a group; the rest of a group is filled with 0xFF (type 15 stream 15 = fill);
//   * the FEC profile (3 bits) is in the PHY frame header, so a receiver needs no setting.
// Fragmentation of a codec unit (a video temporal unit larger than one packet) is done by Fragmenter / Reassembler: the payload starts with a flag byte.
#pragma once
#include "dect2/kavb/kavb_phy.h"
#include "dect2/kavb/kavb_rx.h"
#include <algorithm>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <vector>

namespace dect2::kavb {

enum class PktType : uint8_t { Video = 0, Audio = 1, Text = 2, Picture = 3, Data = 4, Fill = 15 };

struct AvPacket {
    PktType type = PktType::Data;
    uint8_t stream = 0;            // 0..15
    uint16_t seq = 0;              // per stream, wraps
    std::vector<uint8_t> payload;
};
constexpr int kPacketOverhead = 5 + 4;   // header 5 bytes + CRC-32
std::vector<uint8_t> serializePacket(const AvPacket& p);
// Parses one packet at the start of d (n bytes). Returns the bytes used (> 0), 0 when more bytes are needed, -1 when invalid (bad type, length or CRC).
int parsePacket(const uint8_t* d, size_t n, AvPacket& out);

// ---- FEC groups -----------------------------------------------------------------------------------------------------------------------------------------------------
struct FecProfile { int data; int parity; };   // G, P
constexpr int kNumFecProfiles = 8;
// 0: none (4 + 0), 1: 14 + 2, 2: 12 + 4, 3: 28 + 4, 4: 24 + 8, 5: 6 + 2, 6: 56 + 8, 7: 48 + 16.
FecProfile fecProfile(int idx);
// parity cells for G data cells (all the same size). Returns P cells.
std::vector<std::vector<uint8_t>> fecEncode(int fecIdx, const std::vector<std::vector<uint8_t>>& dataCells);
// cells = N entries, an empty vector = missing. Restores the missing data cells when at least G cells are present. Returns true when every data cell is present afterwards.
bool fecRecover(int fecIdx, std::vector<std::vector<uint8_t>>& cells);

// ---- payload interface ----------------------------------------------------------------------------------------------------------------------------------------
class PayloadSource {   // an AV1 / Opus encoder side, or a test generator
public:
    virtual ~PayloadSource() = default;
    // Next packet to send, false when there is none right now.
    virtual bool next(AvPacket& out) = 0;
};
class PayloadSink {     // a decoder side
public:
    virtual ~PayloadSink() = default;
    virtual void onPacket(const AvPacket& p) = 0;
};

// Splits a codec unit into packets of at most maxPayload bytes; the first byte of each payload is a flag byte (bit 0 first fragment, bit 1 last fragment, bit 2 key
// frame / independent unit); the first fragment continues with a 4-byte presentation time in milliseconds (big endian).
std::vector<AvPacket> fragmentUnit(PktType type, uint8_t stream, uint16_t& seq, uint32_t ptsMs, bool key, const std::vector<uint8_t>& unit, size_t maxPayload);
class Reassembler {   // joins the fragments of one stream; a gap in the sequence drops the unit in progress
public:
    struct Unit { uint32_t ptsMs; bool key; std::vector<uint8_t> bytes; };
    bool push(const AvPacket& p, Unit& out);   // true when a unit completed
    int dropped() const { return dropped_; }
private:
    bool open_ = false;
    uint16_t nextSeq_ = 0;
    Unit cur_;
    int dropped_ = 0;
};

// ---- packetiser / depacketiser -----------------------------------------------------------------------------------------------------------------------------
class Packetiser {
public:
    Packetiser(const Profile& p, int fecIdx);
    // The largest payload that fits one group.
    size_t maxPayload() const;
    // False when the payload is larger than maxPayload().
    bool push(const AvPacket& p);
    // Frame payloads (framePayloadBytes each) that are ready: full groups only, or the open group as well when flush is true (padded with fill).
    std::vector<std::vector<uint8_t>> takeFrames(bool flush);
    int fecIdx() const { return fec_; }
    long framesEmitted() const { return framesOut_; }
private:
    void closeGroup();
    Profile p_;
    int fec_;
    std::vector<uint8_t> gbuf_;          // stream bytes of the open group
    std::vector<int> cellStart_;         // per cell: offset (within the cell) of the first packet starting in it, -1
    size_t fill_ = 0;
    std::deque<std::vector<uint8_t>> frames_;
    std::vector<std::vector<uint8_t>> pendingCells_;
    long framesOut_ = 0;
};

class Depacketiser {
public:
    struct Stats { long groupsOk = 0, groupsRecovered = 0, groupsLost = 0, cellsMissing = 0, packets = 0, packetsDropped = 0; };
    explicit Depacketiser(const Profile& p);
    // One received PHY frame.
    void pushFrame(const RxFrame& f);
    // Same for decoded data without a PHY: seq, fec profile and 4 cells (empty = block lost).
    void pushCells(int seq, int fecIdx, const std::vector<std::vector<uint8_t>>& cells);
    // Finalise the open groups (end of stream or a long gap).
    void flush();
    std::vector<AvPacket> takePackets();
    const Stats& stats() const { return stats_; }
private:
    struct Group { int fec; int id; std::vector<std::vector<uint8_t>> cells; int present = 0; bool done = false; };
    void finish(Group& g);
    Profile p_;
    std::map<int, Group> open_;
    std::deque<int> doneIds_;
    std::vector<AvPacket> out_;
    Stats stats_;
};

// ---- bit budget -------------------------------------------------------------------------------------------------------------------------------------------------------
struct BitBudget {
    double symRate;
    double rawBits;          // symRate x bits per symbol (before coding, pilots, preamble, header)
    double phyBlocks;        // bits/s of the block payloads (after LDPC, CRC-16, preamble, header and pilots)
    double afterCellHeader;  // minus the 1 byte offset per cell
    double afterFec;         // minus the parity cells
    double forPackets;       // = afterFec
    double forPayload;       // payload bits/s for a given mean packet size (the 9 bytes of packet overhead)
    double forVideo;         // forPayload minus audio and the fixed side data
};
BitBudget bitBudget(const Profile& p, int fecIdx, double meanPacketPayloadBytes = 900.0, double audioBps = 96000.0, double audioPacketPayloadBytes = 240.0,
                    double sideBps = 2000.0);

// ---- synthetic payload (tests, link tests) --------------------------------------------------------------------------------------------------------------------
// Video units of about videoBps / fps bytes (key units 5 x larger every keyInterval units) and Opus-sized audio units of 20 ms, as packets. The bytes are a function of
// (stream, unit number): verifyUnit() checks them.
class SyntheticAv : public PayloadSource {
public:
    SyntheticAv(double videoBps, double fps, double audioBps, size_t maxPayload, int keyInterval = 30);
    bool next(AvPacket& out) override;
    static std::vector<uint8_t> unitBytes(uint8_t stream, uint32_t unit, size_t n);
    // Duration produced so far in seconds.
    double timeSec() const { return std::max(vT_, aT_); }
private:
    double videoBps_, fps_, audioBps_;
    size_t maxPayload_;
    int keyInterval_;
    uint32_t vUnit_ = 0, aUnit_ = 0;
    double vT_ = 0, aT_ = 0;
    uint16_t vSeq_ = 0, aSeq_ = 0;
    std::deque<AvPacket> q_;
};

}  // namespace dect2::kavb
