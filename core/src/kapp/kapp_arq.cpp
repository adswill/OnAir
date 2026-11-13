// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// KAPP selective repeat and profile stepping (see kapp_arq.h).
#include "dect2/kapp/kapp_arq.h"
#include <cstring>
#include <iterator>

namespace dect2::kapp {

bool packArqBlock(uint32_t offset, bool last, const uint8_t* data, size_t len, uint8_t* out, size_t cap) {
    if (cap < kArqBlockHeader || len > cap - kArqBlockHeader || len > 255 || offset >= (1u << 23)) return false;
    std::memset(out, 0, cap);
    out[0] = (uint8_t)((last ? 0x80u : 0u) | ((offset >> 16) & 0x7Fu));
    out[1] = (uint8_t)((offset >> 8) & 0xFFu);
    out[2] = (uint8_t)(offset & 0xFFu);
    out[3] = (uint8_t)len;
    if (len) std::memcpy(out + kArqBlockHeader, data, len);
    return true;
}

bool unpackArqBlock(const uint8_t* in, size_t n, uint32_t& offset, bool& last, const uint8_t*& data, size_t& len) {
    if (n < kArqBlockHeader) return false;
    last = (in[0] & 0x80u) != 0;
    offset = ((uint32_t)(in[0] & 0x7Fu) << 16) | ((uint32_t)in[1] << 8) | in[2];
    len = in[3];
    if (len > n - kArqBlockHeader) return false;
    data = in + kArqBlockHeader;
    return true;
}

void addInterval(IntervalSet& s, uint32_t a, uint32_t b) {
    if (a >= b) return;
    auto it = s.lower_bound(a);
    if (it != s.begin()) {
        auto p = std::prev(it);
        if (p->second >= a) {
            a = p->first;
            b = std::max(b, p->second);
            it = s.erase(p);
        }
    }
    while (it != s.end() && it->first <= b) {
        b = std::max(b, it->second);
        it = s.erase(it);
    }
    s[a] = b;
}

void subtractInterval(IntervalSet& s, uint32_t a, uint32_t b) {
    if (a >= b) return;
    auto it = s.lower_bound(a);
    if (it != s.begin()) {
        auto p = std::prev(it);
        if (p->second > a) it = p;
    }
    while (it != s.end() && it->first < b) {
        const uint32_t s0 = it->first, e0 = it->second;
        it = s.erase(it);
        if (s0 < a) s[s0] = a;
        if (e0 > b) s[b] = e0;
    }
}

// ---- sender ----
bool ArqSender::setMessage(std::vector<uint8_t> msg) {
    if (msg.empty() || msg.size() > kArqMaxMessage) return false;
    msg_ = std::move(msg);
    haveMsg_ = true;
    unacked_.clear();
    unacked_[0] = (uint32_t)msg_.size();
    highSent_ = 0;
    haveOut_ = false;
    layout_.clear();
    bursts_ = blocks_ = capBytes_ = 0;
    return true;
}

size_t ArqSender::bytesPending() const {
    size_t n = 0;
    for (const auto& kv : unacked_) n += kv.second - kv.first;
    return n;
}

BurstSpec ArqSender::nextBurst(int profileId, int maxBlocks) {
    BurstSpec spec;
    spec.callsign = call_;
    spec.profileId = profileId;
    const Profile* P = profileById(profileId);
    if (!P || !haveMsg_ || unacked_.empty()) return spec;
    if (haveOut_) onTimeout();

    const size_t cap = (size_t)P->payloadBytes;
    const size_t dataCap = std::min<size_t>(cap - kArqBlockHeader, 255);
    maxBlocks = std::max(1, std::min(maxBlocks, kMaxBlocks));

    layout_.clear();
    bool retrans = false, hasLast = false;
    auto it = unacked_.begin();
    uint32_t cur = it->first;
    const uint32_t total = (uint32_t)msg_.size();
    while ((int)layout_.size() < maxBlocks && it != unacked_.end()) {
        const uint32_t len = (uint32_t)std::min<size_t>(dataCap, it->second - cur);
        const bool last = cur + len == total;
        const size_t at = spec.payload.size();
        spec.payload.resize(at + cap);
        packArqBlock(cur, last, msg_.data() + cur, len, spec.payload.data() + at, cap);
        layout_.push_back(BlockRun{cur, len});
        if (cur < highSent_) retrans = true;
        if (last) hasLast = true;
        highSent_ = std::max(highSent_, cur + len);
        cur += len;
        if (cur >= it->second) {
            ++it;
            if (it != unacked_.end()) cur = it->first;
        }
    }
    spec.flags = kFlagAckRequest | (retrans ? kFlagRetransmit : 0) | (hasLast ? kFlagFinal : 0);
    seq_ = (seq_ + 1) & 63;
    spec.seq = seq_;
    outSeq_ = seq_;
    haveOut_ = true;
    bursts_++;
    blocks_ += layout_.size();
    capBytes_ += (uint64_t)layout_.size() * cap;
    return spec;
}

bool ArqSender::onAck(int seq, const std::vector<uint8_t>& blockOk) {
    if (!haveOut_ || seq != outSeq_) return false;
    for (size_t i = 0; i < layout_.size() && i < (size_t)kAckMaxBlocks; ++i) {
        if (i < blockOk.size() && blockOk[i]) subtractInterval(unacked_, layout_[i].offset, layout_[i].offset + layout_[i].len);
    }
    haveOut_ = false;
    return true;
}

void ArqSender::onTimeout() { haveOut_ = false; }

// ---- receiver ----
AckInfo ArqReceiver::onBurst(const RxBurst& b) {
    AckInfo ack;
    ack.callsign = call_;
    ack.seq = b.seq;
    ack.snrDb = b.snrDb;
    ack.blockOk = b.blockOk;
    const Profile* P = profileById(b.profile);
    if (!P) return ack;
    const size_t bp = (size_t)P->payloadBytes;
    for (size_t i = 0; i < b.blockOk.size(); ++i) {
        if (!b.blockOk[i] || (i + 1) * bp > b.payload.size()) continue;
        uint32_t off = 0;
        bool last = false;
        const uint8_t* d = nullptr;
        size_t len = 0;
        if (!unpackArqBlock(b.payload.data() + i * bp, bp, off, last, d, len)) {
            ack.blockOk[i] = 0;   // CRC passed but the block is no ARQ block: do not claim it
            continue;
        }
        if (buf_.size() < (size_t)off + len) buf_.resize((size_t)off + len, 0);
        if (len) std::memcpy(buf_.data() + off, d, len);
        addInterval(have_, off, off + (uint32_t)len);
        if (last) {
            haveTotal_ = true;
            total_ = off + (uint32_t)len;
        }
    }
    return ack;
}

size_t ArqReceiver::bytesReceived() const {
    size_t n = 0;
    for (const auto& kv : have_) n += kv.second - kv.first;
    return n;
}

// ---- profile stepping ----
void ProfileController::down(int n) {
    cur_ = std::max(0, cur_ - n);
    hold_ = 4;
    upCount_ = 0;
}

int ProfileController::onAck(float snrDb, int nBlocks, int nFailed) {
    const bool held = hold_ > 0;
    if (hold_ > 0) --hold_;
    const double failRate = nBlocks > 0 ? (double)nFailed / (double)nBlocks : 0.0;
    const double margin = (double)snrDb - (double)kProfiles[(size_t)cur_].needEsN0Db;
    if (failRate > 0.20) {
        ++failStreak_;
        if (failStreak_ >= 2) {
            down(2);
            failStreak_ = 0;
        } else {
            down(1);
        }
        return cur_;
    }
    failStreak_ = 0;
    if (margin < 1.0 && cur_ > 0) {
        down(1);
        return cur_;
    }
    const bool canUp = cur_ < kMaxProfileId && margin >= 3.0 && (double)kProfiles[(size_t)cur_ + 1].needEsN0Db + 1.5 <= (double)snrDb;
    if (canUp && !held) {
        if (++upCount_ >= 2) {
            ++cur_;
            upCount_ = 0;
        }
    } else {
        upCount_ = 0;
    }
    return cur_;
}

int ProfileController::onTimeout() {
    down(2);
    failStreak_ = 0;
    return cur_;
}

// ---- loopback simulation ----
namespace {
double uniform01(kris::XorShift64& g) { return (double)(g.next() >> 11) * (1.0 / 9007199254740992.0); }
}

SimResult simulateTransfer(const std::vector<uint8_t>& msg, int profileId, double blockLoss, uint64_t seed, int maxBlocksPerBurst, double ackLoss) {
    SimResult res;
    const Profile* P = profileById(profileId);
    ArqSender tx("TXCALL");
    ArqReceiver rx("RXCALL");
    if (!P || !tx.setMessage(msg)) return res;
    kris::XorShift64 g{seed * 0x9E3779B97F4A7C15ull + 0x1234567ull};
    const size_t bp = (size_t)P->payloadBytes;
    for (int guard = 0; guard < 200000 && !tx.done(); ++guard) {
        const BurstSpec s = tx.nextBurst(profileId, maxBlocksPerBurst);
        if (s.payload.empty()) break;
        RxBurst b;
        b.callsign = s.callsign;
        b.profile = s.profileId;
        b.seq = s.seq;
        b.flags = s.flags;
        b.nBlocks = (int)(s.payload.size() / bp);
        b.snrDb = 20.f;
        b.blockOk.assign((size_t)b.nBlocks, 0);
        b.payload.assign(s.payload.size(), 0);
        for (int i = 0; i < b.nBlocks; ++i) {
            const double u = uniform01(g);
            if (u >= blockLoss) {
                b.blockOk[(size_t)i] = 1;
                std::copy(s.payload.begin() + (long)((size_t)i * bp), s.payload.begin() + (long)((size_t)(i + 1) * bp), b.payload.begin() + (long)((size_t)i * bp));
            }
        }
        const AckInfo ack = rx.onBurst(b);
        const double ua = uniform01(g);
        if (ua < ackLoss) {
            tx.onTimeout();
            res.timeouts++;
        } else {
            tx.onAck(ack.seq, ack.blockOk);
        }
    }
    res.delivered = rx.complete();
    res.matches = res.delivered && rx.message() == msg;
    res.bursts = (int)tx.burstsSent();
    res.blocks = (int)tx.blocksSent();
    res.efficiency = tx.efficiency();
    return res;
}

}  // namespace dect2::kapp
