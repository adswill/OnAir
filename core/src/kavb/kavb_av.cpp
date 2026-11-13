// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/kavb/kavb_av.h"
#include "dect2/rs/kris.h"
#include <algorithm>
#include <cstring>

namespace dect2::kavb {

// ---- packets -----------------------------------------------------------------------------------------------------------------------------------------------------------

std::vector<uint8_t> serializePacket(const AvPacket& p) {
    std::vector<uint8_t> d;
    d.reserve(p.payload.size() + kPacketOverhead);
    d.push_back(static_cast<uint8_t>((static_cast<int>(p.type) << 4) | (p.stream & 15)));
    d.push_back(static_cast<uint8_t>(p.seq >> 8));
    d.push_back(static_cast<uint8_t>(p.seq));
    const size_t n = std::min<size_t>(p.payload.size(), 65535);
    d.push_back(static_cast<uint8_t>(n >> 8));
    d.push_back(static_cast<uint8_t>(n));
    d.insert(d.end(), p.payload.begin(), p.payload.begin() + n);
    const uint32_t c = kris::crc32(d.data(), d.size());
    for (int i = 3; i >= 0; i--) d.push_back(static_cast<uint8_t>(c >> (8 * i)));
    return d;
}

int parsePacket(const uint8_t* d, size_t n, AvPacket& out) {
    if (n < 5) return 0;
    const int type = d[0] >> 4;
    if (type > 4) return -1;
    const size_t len = (static_cast<size_t>(d[3]) << 8) | d[4];
    const size_t total = 5 + len + 4;
    if (n < total) return 0;
    const uint32_t c = kris::crc32(d, 5 + len);
    const uint32_t got = (static_cast<uint32_t>(d[5 + len]) << 24) | (static_cast<uint32_t>(d[6 + len]) << 16) | (static_cast<uint32_t>(d[7 + len]) << 8) | d[8 + len];
    if (c != got) return -1;
    out.type = static_cast<PktType>(type);
    out.stream = d[0] & 15;
    out.seq = static_cast<uint16_t>((d[1] << 8) | d[2]);
    out.payload.assign(d + 5, d + 5 + len);
    return static_cast<int>(total);
}

// ---- FEC groups (the erasure code itself is in Rust: rust/onair_kris/src/erasure.rs) -------------------------------------------------------------------

FecProfile fecProfile(int idx) {
    static const FecProfile t[kNumFecProfiles] = {{4, 0}, {14, 2}, {12, 4}, {28, 4}, {24, 8}, {6, 2}, {56, 8}, {48, 16}};
    return t[idx & 7];
}

std::vector<std::vector<uint8_t>> fecEncode(int fecIdx, const std::vector<std::vector<uint8_t>>& data) {
    const FecProfile f = fecProfile(fecIdx);
    const size_t n = data.empty() ? 0 : data[0].size();
    std::vector<std::vector<uint8_t>> par(f.parity, std::vector<uint8_t>(n, 0));
    if (f.parity == 0 || n == 0 || static_cast<int>(data.size()) != f.data) return par;
    std::vector<uint8_t> flat(static_cast<size_t>(f.data) * n), pflat(static_cast<size_t>(f.parity) * n);
    for (int i = 0; i < f.data; i++) std::copy(data[i].begin(), data[i].end(), flat.begin() + static_cast<size_t>(i) * n);
    if (onair_kris_erasure_encode(f.data, f.parity, static_cast<int32_t>(n), flat.data(), pflat.data()) != 0) return par;
    for (int j = 0; j < f.parity; j++) par[j].assign(pflat.begin() + static_cast<size_t>(j) * n, pflat.begin() + static_cast<size_t>(j + 1) * n);
    return par;
}

bool fecRecover(int fecIdx, std::vector<std::vector<uint8_t>>& cells) {
    const FecProfile f = fecProfile(fecIdx);
    const int G = f.data, N = f.data + f.parity;
    if (static_cast<int>(cells.size()) != N) return false;
    bool anyMissing = false;
    size_t n = 0;
    for (int i = 0; i < N; i++) {
        if (cells[i].empty()) { if (i < G) anyMissing = true; }
        else n = cells[i].size();
    }
    if (!anyMissing) return true;
    if (n == 0) return false;
    std::vector<uint8_t> flat(static_cast<size_t>(N) * n, 0), present(N, 0);
    for (int i = 0; i < N; i++)
        if (cells[i].size() == n) {
            std::copy(cells[i].begin(), cells[i].end(), flat.begin() + static_cast<size_t>(i) * n);
            present[i] = 1;
        }
    if (onair_kris_erasure_recover(G, f.parity, static_cast<int32_t>(n), flat.data(), present.data()) != 0) return false;
    for (int i = 0; i < G; i++)
        if (cells[i].empty()) cells[i].assign(flat.begin() + static_cast<size_t>(i) * n, flat.begin() + static_cast<size_t>(i + 1) * n);
    return true;
}

// ---- fragmentation ---------------------------------------------------------------------------------------------------------------------------------------------------

std::vector<AvPacket> fragmentUnit(PktType type, uint8_t stream, uint16_t& seq, uint32_t ptsMs, bool key, const std::vector<uint8_t>& unit, size_t maxPayload) {
    std::vector<AvPacket> out;
    size_t pos = 0;
    bool first = true;
    do {
        AvPacket p;
        p.type = type;
        p.stream = stream;
        p.seq = seq++;
        size_t room = maxPayload > 6 ? maxPayload - 1 : 1;
        std::vector<uint8_t> pl(1, 0);
        if (first) {
            for (int i = 3; i >= 0; i--) pl.push_back(static_cast<uint8_t>(ptsMs >> (8 * i)));
            room = room > 4 ? room - 4 : 1;
        }
        const size_t take = std::min(room, unit.size() - pos);
        pl.insert(pl.end(), unit.begin() + pos, unit.begin() + pos + take);
        pos += take;
        pl[0] = static_cast<uint8_t>((first ? 1 : 0) | (pos >= unit.size() ? 2 : 0) | (key ? 4 : 0));
        p.payload = std::move(pl);
        out.push_back(std::move(p));
        first = false;
    } while (pos < unit.size());
    return out;
}

bool Reassembler::push(const AvPacket& p, Unit& out) {
    if (p.payload.empty()) return false;
    const uint8_t fl = p.payload[0];
    size_t off = 1;
    if (fl & 1) {
        if (open_) dropped_++;
        if (p.payload.size() < 5) { open_ = false; return false; }
        cur_.ptsMs = (static_cast<uint32_t>(p.payload[1]) << 24) | (static_cast<uint32_t>(p.payload[2]) << 16) | (static_cast<uint32_t>(p.payload[3]) << 8) | p.payload[4];
        cur_.key = (fl & 4) != 0;
        cur_.bytes.clear();
        open_ = true;
        off = 5;
    } else if (!open_ || p.seq != nextSeq_) {
        if (open_) dropped_++;
        open_ = false;
        return false;
    }
    cur_.bytes.insert(cur_.bytes.end(), p.payload.begin() + off, p.payload.end());
    nextSeq_ = static_cast<uint16_t>(p.seq + 1);
    if (fl & 2) {
        out = cur_;
        open_ = false;
        return true;
    }
    return false;
}

// ---- packetiser ---------------------------------------------------------------------------------------------------------------------------------------------------------

Packetiser::Packetiser(const Profile& p, int fecIdx) : p_(p), fec_(fecIdx & 7) {
    const FecProfile f = fecProfile(fec_);
    cellStart_.assign(f.data, -1);
    gbuf_.assign(static_cast<size_t>(f.data) * (p_.blockPayloadBytes - 1), 0xFF);
}

size_t Packetiser::maxPayload() const { return gbuf_.size() - kPacketOverhead; }

bool Packetiser::push(const AvPacket& p) {
    if (p.payload.size() > maxPayload()) return false;
    const auto d = serializePacket(p);
    if (fill_ + d.size() > gbuf_.size()) closeGroup();
    const size_t cd = static_cast<size_t>(p_.blockPayloadBytes - 1);
    const size_t cell = fill_ / cd;
    if (cellStart_[cell] < 0) cellStart_[cell] = static_cast<int>(fill_ % cd);
    std::memcpy(gbuf_.data() + fill_, d.data(), d.size());
    fill_ += d.size();
    if (fill_ == gbuf_.size()) closeGroup();
    return true;
}

void Packetiser::closeGroup() {
    if (fill_ == 0) return;
    const FecProfile f = fecProfile(fec_);
    const size_t cd = static_cast<size_t>(p_.blockPayloadBytes - 1);
    std::vector<std::vector<uint8_t>> cells(f.data);
    for (int i = 0; i < f.data; i++) {
        cells[i].resize(cd + 1);
        cells[i][0] = cellStart_[i] < 0 ? 0xFF : static_cast<uint8_t>(cellStart_[i]);
        std::memcpy(cells[i].data() + 1, gbuf_.data() + i * cd, cd);
    }
    const auto par = fecEncode(fec_, cells);
    for (auto& c : cells) pendingCells_.push_back(std::move(c));
    for (const auto& c : par) pendingCells_.push_back(c);
    while (pendingCells_.size() >= static_cast<size_t>(kBlocksPerFrame)) {
        std::vector<uint8_t> fr;
        for (int i = 0; i < kBlocksPerFrame; i++) fr.insert(fr.end(), pendingCells_[i].begin(), pendingCells_[i].end());
        frames_.push_back(std::move(fr));
        pendingCells_.erase(pendingCells_.begin(), pendingCells_.begin() + kBlocksPerFrame);
    }
    std::fill(gbuf_.begin(), gbuf_.end(), 0xFF);
    std::fill(cellStart_.begin(), cellStart_.end(), -1);
    fill_ = 0;
}

std::vector<std::vector<uint8_t>> Packetiser::takeFrames(bool flush) {
    if (flush) closeGroup();
    std::vector<std::vector<uint8_t>> out(frames_.begin(), frames_.end());
    frames_.clear();
    framesOut_ += static_cast<long>(out.size());
    return out;
}

// ---- depacketiser -------------------------------------------------------------------------------------------------------------------------------------------------------

Depacketiser::Depacketiser(const Profile& p) : p_(p) {}

void Depacketiser::pushFrame(const RxFrame& f) {
    std::vector<std::vector<uint8_t>> cells(kBlocksPerFrame);
    if (f.blocks.size() == static_cast<size_t>(kBlocksPerFrame) && f.prof.blockPayloadBytes == p_.blockPayloadBytes)
        for (int b = 0; b < kBlocksPerFrame; b++)
            if (f.blocks[b].ok) cells[b] = f.blocks[b].bytes;
    pushCells(f.hdr.seq, f.hdr.fec, cells);
}

void Depacketiser::pushCells(int seq, int fecIdx, const std::vector<std::vector<uint8_t>>& cells) {
    const FecProfile fp = fecProfile(fecIdx);
    const int N = fp.data + fp.parity, gf = N / kBlocksPerFrame;
    const int gid = seq / gf, pos = seq % gf;
    if (std::find(doneIds_.begin(), doneIds_.end(), gid) != doneIds_.end()) return;
    // a frame of a new group closes the older open groups
    for (auto it = open_.begin(); it != open_.end();)
        if (it->first != gid) {
            finish(it->second);
            it = open_.erase(it);
        } else ++it;
    auto it = open_.find(gid);
    if (it == open_.end()) {
        Group g;
        g.fec = fecIdx;
        g.id = gid;
        g.cells.assign(N, std::vector<uint8_t>());
        it = open_.emplace(gid, std::move(g)).first;
    }
    Group& g = it->second;
    for (int b = 0; b < kBlocksPerFrame && b < static_cast<int>(cells.size()); b++) {
        auto& slot = g.cells[pos * kBlocksPerFrame + b];
        if (slot.empty() && !cells[b].empty()) {
            slot = cells[b];
            g.present++;
        }
    }
    int dataPresent = 0;
    for (int i = 0; i < fp.data; i++) dataPresent += g.cells[i].empty() ? 0 : 1;
    if (dataPresent == fp.data) {
        finish(g);
        open_.erase(it);
    }
}

void Depacketiser::flush() {
    for (auto& e : open_) finish(e.second);
    open_.clear();
}

std::vector<AvPacket> Depacketiser::takePackets() {
    std::vector<AvPacket> o;
    o.swap(out_);
    return o;
}

void Depacketiser::finish(Group& g) {
    doneIds_.push_back(g.id);
    if (doneIds_.size() > 16) doneIds_.pop_front();
    const FecProfile fp = fecProfile(g.fec);
    const int G = fp.data;
    int missing = 0;
    for (int i = 0; i < G; i++) missing += g.cells[i].empty() ? 1 : 0;
    const bool recovered = missing > 0 && fecRecover(g.fec, g.cells);
    if (missing == 0) stats_.groupsOk++;
    else if (recovered) stats_.groupsRecovered++;
    else stats_.groupsLost++;
    int stillMissing = 0;
    for (int i = 0; i < G; i++) stillMissing += g.cells[i].empty() ? 1 : 0;
    stats_.cellsMissing += stillMissing;
    const size_t cd = static_cast<size_t>(p_.blockPayloadBytes - 1);
    // runs of present data cells
    int i = 0;
    while (i < G) {
        if (g.cells[i].empty()) { i++; continue; }
        int j = i;
        while (j + 1 < G && !g.cells[j + 1].empty()) j++;
        // start of the first packet in the run
        int c0 = i;
        size_t off0 = 0;
        if (i != 0) {
            c0 = -1;
            for (int c = i; c <= j; c++)
                if (g.cells[c][0] != 0xFF) { c0 = c; off0 = g.cells[c][0]; break; }
        }
        if (c0 >= 0) {
            std::vector<uint8_t> bytes;
            std::vector<int> cellOf;   // stream position -> cell index (for resync)
            for (int c = c0; c <= j; c++) bytes.insert(bytes.end(), g.cells[c].begin() + 1, g.cells[c].end());
            size_t pos = off0;
            while (pos < bytes.size()) {
                if (bytes[pos] == 0xFF) break;   // fill
                AvPacket pk;
                const int r = parsePacket(bytes.data() + pos, bytes.size() - pos, pk);
                if (r > 0) {
                    out_.push_back(std::move(pk));
                    stats_.packets++;
                    pos += static_cast<size_t>(r);
                    continue;
                }
                stats_.packetsDropped++;
                if (r == 0) break;   // truncated by a missing cell
                // invalid: resync at the next cell that has a packet start
                int curCell = c0 + static_cast<int>(pos / cd);
                size_t np = bytes.size();
                for (int c = curCell + 1; c <= j; c++)
                    if (g.cells[c][0] != 0xFF) { np = static_cast<size_t>(c - c0) * cd + g.cells[c][0]; break; }
                pos = np;
            }
        }
        i = j + 1;
    }
}

// ---- bit budget -------------------------------------------------------------------------------------------------------------------------------------------------------

BitBudget bitBudget(const Profile& p, int fecIdx, double meanPacketPayloadBytes, double audioBps, double audioPacketPayloadBytes, double sideBps) {
    const FecProfile f = fecProfile(fecIdx);
    BitBudget b{};
    b.symRate = p.symRate;
    b.rawBits = p.symRate * p.bitsPerSym;
    b.phyBlocks = framePayloadBytes(p) * 8.0 / frameSeconds(p);
    const double cd = p.blockPayloadBytes - 1.0;
    b.afterCellHeader = b.phyBlocks * cd / p.blockPayloadBytes;
    b.afterFec = b.afterCellHeader * f.data / (f.data + f.parity);
    b.forPackets = b.afterFec;
    b.forPayload = b.forPackets * meanPacketPayloadBytes / (meanPacketPayloadBytes + kPacketOverhead + 1.0);
    const double audioCost = audioBps * (audioPacketPayloadBytes + kPacketOverhead + 5.0) / audioPacketPayloadBytes;
    const double left = b.forPackets - audioCost - sideBps;
    b.forVideo = std::max(0.0, left * meanPacketPayloadBytes / (meanPacketPayloadBytes + kPacketOverhead + 1.0));
    return b;
}

// ---- synthetic payload ------------------------------------------------------------------------------------------------------------------------------------------

SyntheticAv::SyntheticAv(double videoBps, double fps, double audioBps, size_t maxPayload, int keyInterval)
    : videoBps_(videoBps), fps_(fps), audioBps_(audioBps), maxPayload_(maxPayload), keyInterval_(std::max(1, keyInterval)) {}

std::vector<uint8_t> SyntheticAv::unitBytes(uint8_t stream, uint32_t unit, size_t n) {
    kris::XorShift64 g{(static_cast<uint64_t>(stream) << 40) ^ (static_cast<uint64_t>(unit) * 0x9E3779B97F4A7C15ull) ^ 0x1234567ull};
    std::vector<uint8_t> v(n);
    for (auto& b : v) b = static_cast<uint8_t>(g.next() >> 24);
    return v;
}

bool SyntheticAv::next(AvPacket& out) {
    if (q_.empty()) {
        if (vT_ <= aT_) {
            const bool key = (vUnit_ % static_cast<uint32_t>(keyInterval_)) == 0;
            const double avg = videoBps_ / 8.0 / fps_;
            const double base = avg * keyInterval_ / (keyInterval_ + 4.0);
            const size_t n = static_cast<size_t>(key ? 5.0 * base : base);
            auto pk = fragmentUnit(PktType::Video, 0, vSeq_, static_cast<uint32_t>(vT_ * 1000.0), key, unitBytes(0, vUnit_, n), maxPayload_);
            for (auto& p : pk) q_.push_back(std::move(p));
            vUnit_++;
            vT_ += 1.0 / fps_;
        } else {
            const size_t n = static_cast<size_t>(audioBps_ / 8.0 * 0.02);
            auto pk = fragmentUnit(PktType::Audio, 0, aSeq_, static_cast<uint32_t>(aT_ * 1000.0), true, unitBytes(1, aUnit_, n), maxPayload_);
            for (auto& p : pk) q_.push_back(std::move(p));
            aUnit_++;
            aT_ += 0.02;
        }
    }
    out = std::move(q_.front());
    q_.pop_front();
    return true;
}

}  // namespace dect2::kavb
