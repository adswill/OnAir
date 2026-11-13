// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// KAWS ARQ (spec 4.5): half-duplex, one slot turnaround. The sender transmits blocks in its slot, the receiver answers in the next slot with an ack frame
// (message id, a bitmap of the blocks received with a good CRC, the measured SNR, the suggested profile). Missing blocks go again in the next caller slot;
// a block is abandoned after kMaxAttempts transmissions. Up to 16 blocks per message (4-bit block index).
#pragma once
#include "dect2/kaws/kaws_frame.h"
#include "dect2/kaws/kaws_sched.h"
#include <map>

namespace dect2::kaws {

constexpr int kMaxAttempts = 8;

class ArqSender {
public:
    ArqSender(std::string myCall, int profile) : call_(std::move(myCall)), profile_(profile) {}
    // Start a message (bit string). False if it needs more than 16 blocks or the profile cannot carry data.
    bool start(uint8_t msgId, const Bits& message);
    // The blocks to transmit in the next caller slot: the lowest-numbered blocks not yet acknowledged, at most blocksPerSlot(profile); each counts one attempt.
    // Empty when the message is complete or failed.
    std::vector<Bits> nextBlocks();
    // The ack of the receiver. Returns false if it is for another message or from a corrupt frame. Adopts the suggested profile when `follow` is set.
    bool onAck(const Ack& ack, bool follow = true);
    bool done() const { return active_ && missing().empty(); }
    bool failed() const;
    int profile() const { return profile_; }
    int totalBlocks() const { return (int)blocks_.size(); }
    std::vector<int> missing() const;
    int attempts(int block) const { return block >= 0 && block < (int)attempts_.size() ? attempts_[(size_t)block] : 0; }

private:
    std::string call_;
    int profile_;
    uint8_t msgId_ = 0;
    bool active_ = false;
    std::vector<Bits> blocks_;       // as many as the message needs, built for the profile at start
    Bits message_;
    std::vector<bool> acked_;
    std::vector<int> attempts_;
    void rebuild();                  // re-split the message after a profile change (blocks not yet acknowledged carry on with the new size)
};

class ArqReceiver {
public:
    explicit ArqReceiver(std::string myCall) : call_(std::move(myCall)) {}
    // A decoded block of K bits (K of the profile it came in). Returns true when it was a good structured block.
    bool onBlock(int K, const Bits& block);
    // The message of msgId is complete (block 0 present and every block up to its length received).
    bool complete(uint8_t msgId) const;
    Bits message(uint8_t msgId) const;   // the concatenated payload bits, trimmed to the length announced in block 0
    // The ack frame for msgId: bitmap of the good blocks, measured SNR, suggested profile (from the SNR).
    Ack makeAck(uint8_t msgId, double snrDb) const;
    void forget(uint8_t msgId) { rx_.erase(msgId); }

private:
    struct Msg { int K = 0; std::map<int, Bits> payloads; };
    std::string call_;
    std::map<uint8_t, Msg> rx_;
};

}  // namespace dect2::kaws
