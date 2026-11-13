// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// KAPP selective repeat (pure state machines, no radio) and the profile stepping rule.
//
// Message layer built on the PHY blocks: every PHY block of a data burst carries a 4 byte header and up to (payloadBytes - 4) message bytes:
//   byte 0: bit 7 = "this block holds the last byte of the message", bits 6..0 = message offset bits 22..16
//   byte 1, 2: offset bits 15..0;  byte 3: number of message bytes in the block;  then the bytes, then zero padding.
// Each block says where its bytes belong, so a block that fails costs only its own bytes, whatever profile the resend goes out at (this replaces the
// 10-bit-per-entry retransmit table of the spec, which needs block 0 to arrive to place the others). The sender sends the lowest unacknowledged bytes
// first (missing blocks, then new data). Messages up to 8 MiB.
#pragma once
#include "dect2/kapp/kapp_frame.h"
#include "dect2/kapp/kapp_rx.h"
#include <algorithm>
#include <map>

namespace dect2::kapp {

constexpr size_t kArqBlockHeader = 4;
constexpr size_t kArqMaxMessage = (size_t)1 << 23;

bool packArqBlock(uint32_t offset, bool last, const uint8_t* data, size_t len, uint8_t* out, size_t cap);   // out is zero filled to cap
bool unpackArqBlock(const uint8_t* in, size_t n, uint32_t& offset, bool& last, const uint8_t*& data, size_t& len);

// sorted, non-overlapping [start, end) intervals
using IntervalSet = std::map<uint32_t, uint32_t>;
void addInterval(IntervalSet& s, uint32_t a, uint32_t b);
void subtractInterval(IntervalSet& s, uint32_t a, uint32_t b);

struct BlockRun {
    uint32_t offset = 0;
    uint32_t len = 0;
};

class ArqSender {
public:
    explicit ArqSender(std::string callsign) : call_(std::move(callsign)) {}
    bool setMessage(std::vector<uint8_t> msg);          // false when empty or too long
    bool done() const { return haveMsg_ && unacked_.empty(); }
    size_t bytesPending() const;
    // The next burst at this profile with at most maxBlocks blocks; the payload is empty when everything is acknowledged. A burst still waiting for its
    // ACK counts as lost (onTimeout).
    BurstSpec nextBurst(int profileId, int maxBlocks);
    bool hasOutstanding() const { return haveOut_; }
    const std::vector<BlockRun>& outstanding() const { return layout_; }
    int outstandingSeq() const { return outSeq_; }
    // false when the sequence number is not the outstanding burst's
    bool onAck(int seq, const std::vector<uint8_t>& blockOk);
    void onTimeout();

    uint64_t burstsSent() const { return bursts_; }
    uint64_t blocksSent() const { return blocks_; }
    uint64_t capacityBytesSent() const { return capBytes_; }
    double efficiency() const { return capBytes_ ? (double)msg_.size() / (double)capBytes_ : 0.0; }   // message bytes / payload bytes put on air

private:
    std::string call_;
    std::vector<uint8_t> msg_;
    bool haveMsg_ = false;
    IntervalSet unacked_;
    uint32_t highSent_ = 0;
    bool haveOut_ = false;
    int outSeq_ = 0;
    int seq_ = 0;
    std::vector<BlockRun> layout_;
    uint64_t bursts_ = 0, blocks_ = 0, capBytes_ = 0;
};

class ArqReceiver {
public:
    explicit ArqReceiver(std::string callsign) : call_(std::move(callsign)) {}
    // process a data burst (not an ACK); returns the ACK content for it
    AckInfo onBurst(const RxBurst& b);
    bool complete() const { return haveTotal_ && have_.size() == 1 && have_.begin()->first == 0 && have_.begin()->second == total_; }
    const std::vector<uint8_t>& message() const { return buf_; }
    size_t bytesReceived() const;

private:
    std::string call_;
    std::vector<uint8_t> buf_;
    IntervalSet have_;
    bool haveTotal_ = false;
    uint32_t total_ = 0;
};

// Profile stepping (spec section 7). margin = reported Es/N0 - the profile's required Es/N0.
//  up one when margin >= 3 dB and the next profile's requirement + 1.5 dB <= reported Es/N0, on two ACKs in a row;
//  down one when more than 20 percent of the blocks failed or margin < 1 dB; down two on a second such ACK in a row or on a lost ACK;
//  after any step down no step up for 4 bursts.
class ProfileController {
public:
    explicit ProfileController(int startProfile = 2) : cur_(std::max(0, std::min(kMaxProfileId, startProfile))) {}
    int profile() const { return cur_; }
    int onAck(float snrDb, int nBlocks, int nFailed);
    int onTimeout();
    int hold() const { return hold_; }

private:
    void down(int n);
    int cur_;
    int upCount_ = 0;
    int hold_ = 0;
    int failStreak_ = 0;
};

// Loopback of a whole message through a lossy link, no radio: each block is lost independently with probability blockLoss, each ACK with ackLoss.
struct SimResult {
    bool delivered = false;     // the receiver holds the whole message
    bool matches = false;       // ... and it is identical
    int bursts = 0;
    int blocks = 0;
    int timeouts = 0;
    double efficiency = 0.0;    // message bytes / payload bytes sent
};
SimResult simulateTransfer(const std::vector<uint8_t>& msg, int profileId, double blockLoss, uint64_t seed, int maxBlocksPerBurst = 64, double ackLoss = 0.0);

}  // namespace dect2::kapp
