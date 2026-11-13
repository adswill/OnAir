// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// KAWS ARQ. See kaws_arq.h.
#include "dect2/kaws/kaws_arq.h"
#include <algorithm>

namespace dect2::kaws {

bool ArqSender::start(uint8_t msgId, const Bits& message) {
    if (profile_ < 0 || profile_ >= kNumProfiles || kProfiles[profile_].K <= kBlockOverhead || msgId > 15) return false;
    std::vector<Bits> b = splitMessage(kProfiles[profile_].K, msgId, message);
    if (b.empty()) return false;
    msgId_ = msgId;
    message_ = message;
    blocks_ = b;
    acked_.assign(b.size(), false);
    attempts_.assign(b.size(), 0);
    active_ = true;
    return true;
}

std::vector<int> ArqSender::missing() const {
    std::vector<int> m;
    for (size_t i = 0; i < acked_.size(); i++)
        if (!acked_[i]) m.push_back((int)i);
    return m;
}

bool ArqSender::failed() const {
    if (!active_) return false;
    for (int i : missing())
        if (attempts_[(size_t)i] >= kMaxAttempts) return true;
    return false;
}

std::vector<Bits> ArqSender::nextBlocks() {
    std::vector<Bits> out;
    if (!active_ || failed()) return out;
    int cap = std::max(1, blocksPerSlot(profile_));
    for (int i : missing()) {
        if ((int)out.size() >= cap) break;
        out.push_back(blocks_[(size_t)i]);
        attempts_[(size_t)i]++;
    }
    return out;
}

bool ArqSender::onAck(const Ack& ack, bool follow) {
    if (!active_ || ack.msgId != msgId_) return false;
    for (size_t i = 0; i < acked_.size() && i < 16; i++)
        if ((ack.bitmap >> i) & 1u) acked_[i] = true;
    if (follow && ack.suggestedProfile != profile_ && ack.suggestedProfile >= 0 && ack.suggestedProfile <= 5 && !done()) {
        // the profile changes; blocks of the new size are cut from the same message and the acknowledgements restart (a bitmap refers to the old block size)
        profile_ = ack.suggestedProfile;
        rebuild();
    }
    return true;
}

void ArqSender::rebuild() {
    std::vector<Bits> b = splitMessage(kProfiles[profile_].K, msgId_, message_);
    if (b.empty()) return;   // would not fit in 16 blocks: keep going with the old profile's blocks is impossible, so stay inactive
    blocks_ = b;
    acked_.assign(b.size(), false);
    attempts_.assign(b.size(), 0);
}

bool ArqReceiver::onBlock(int K, const Bits& block) {
    uint8_t id, idx;
    Bits pl;
    if (!parseBlock(K, block, id, idx, pl)) return false;
    Msg& m = rx_[id];
    if (m.K != K) { m.K = K; m.payloads.clear(); }   // a profile change restarts the message at the new block size
    m.payloads[idx] = pl;
    return true;
}

// The message length is read from the frame header. At P0 block 0 holds only 104 bits, which is shorter than the header of a chat frame, so the length is
// read from the blocks 0, 1, ... joined (consecutive blocks present from 0), and the message is complete when the joined bits reach the length.
static bool joinBlocks(const std::map<int, Bits>& payloads, Bits& joined, size_t& bits) {
    joined.clear();
    bits = 0;
    for (int i = 0;; i++) {
        auto it = payloads.find(i);
        if (it == payloads.end()) return false;
        joined.insert(joined.end(), it->second.begin(), it->second.end());
        bits = expectedMessageBits(joined);
        if (bits && joined.size() >= bits) return true;
    }
}

bool ArqReceiver::complete(uint8_t id) const {
    auto it = rx_.find(id);
    if (it == rx_.end()) return false;
    Bits joined;
    size_t bits;
    return joinBlocks(it->second.payloads, joined, bits);
}

Bits ArqReceiver::message(uint8_t id) const {
    auto it = rx_.find(id);
    if (it == rx_.end()) return {};
    Bits joined;
    size_t bits;
    if (!joinBlocks(it->second.payloads, joined, bits)) return {};
    joined.resize(bits);
    return joined;
}

Ack ArqReceiver::makeAck(uint8_t id, double snrDb) const {
    Ack a;
    a.from = call_;
    a.msgId = id;
    auto it = rx_.find(id);
    if (it != rx_.end())
        for (auto& kv : it->second.payloads)
            if (kv.first < 32) a.bitmap |= 1u << kv.first;
    a.snrDb = (int)std::lround(std::max(-35.0, std::min(28.0, snrDb)));
    a.suggestedProfile = suggestProfile(snrDb);
    return a;
}

}  // namespace dect2::kaws
