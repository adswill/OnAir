// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/tv/teletext.h"
#include <algorithm>
#include <cstring>
#include <cstdint>
#include <mutex>
#include <vector>

namespace dect2 {


// ------------------------------------------------------------------ decoder
void TeletextDecoder::setPid(int pid) {
    std::lock_guard<std::mutex> lk(mu_);
    if (pid == pid_) return;
    pid_ = pid;
    pes_.clear(); inPes_ = false; cc_ = -1;
    pages_.clear();
    curInit_ = false;
}
int TeletextDecoder::pid() const { std::lock_guard<std::mutex> lk(mu_); return pid_; }
void TeletextDecoder::reset() {
    std::lock_guard<std::mutex> lk(mu_);
    pes_.clear(); inPes_ = false; cc_ = -1; pages_.clear(); curInit_ = false; packets_ = 0;
}
uint64_t TeletextDecoder::packets() const { std::lock_guard<std::mutex> lk(mu_); return packets_; }

void TeletextDecoder::feedTs(const uint8_t* p) {
    if (p[0] != 0x47) return;
    const int pid = ((p[1] & 0x1F) << 8) | p[2];
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (pid != pid_) return;
    }
    const bool pusi = (p[1] & 0x40) != 0;
    const int afc = (p[3] >> 4) & 3;
    int off = 4;
    if (afc & 2) off += 1 + p[4];
    if (!(afc & 1) || off >= 188) return;
    std::vector<uint8_t> done;
    {
        std::lock_guard<std::mutex> lk(mu_);
        const int cc = p[3] & 15;
        if (cc_ >= 0 && ((cc_ + 1) & 15) != cc && !pusi) { pes_.clear(); inPes_ = false; } // lost packets: drop the partial PES
        cc_ = cc;
        if (pusi) {
            if (inPes_ && !pes_.empty()) done.swap(pes_);
            pes_.clear();
            inPes_ = true;
            pesLen_ = 0;
        }
        if (inPes_) {
            pes_.insert(pes_.end(), p + off, p + 188);
            if (pes_.size() >= 6 && pesLen_ == 0 && pes_[0] == 0 && pes_[1] == 0 && pes_[2] == 1) {
                const size_t l = ((size_t)pes_[4] << 8) | pes_[5];
                if (l) pesLen_ = l + 6;
            }
            if (pesLen_ && pes_.size() >= pesLen_ && done.empty()) { done.swap(pes_); inPes_ = false; pesLen_ = 0; }
        }
    }
    if (!done.empty()) feedPes(done.data(), done.size());
}

void TeletextDecoder::feedPes(const uint8_t* pes, size_t n) {
    std::vector<uint32_t> off(n / 46 + 2);
    const size_t count = std::min(ttxPesUnits(pes, n, off.data(), off.size()), off.size());
    for (size_t i = 0; i < count; i++) dataUnit(pes + off[i]);
}

void TeletextDecoder::closePage(int64_t key) {
    if (key < 0) return;
    auto it = pages_.find((uint32_t)key);
    if (it == pages_.end()) return;
    it->second.live.complete = true;
    it->second.done = it->second.live;
    it->second.haveDone = true;
}

void TeletextDecoder::dataUnit(const uint8_t* u) {
    const TtxUnit d = ttxDecodeUnit(u);
    if (d.kind == TtxUnit::Ignore) return;
    const int mag = d.mag, row = d.row;
    std::lock_guard<std::mutex> lk(mu_);
    if (!curInit_) { for (int i = 0; i < 9; i++) cur_[i] = -1; curInit_ = true; }
    packets_++;
    if (d.kind == TtxUnit::Header) {
        // this header ends the page being sent in its magazine (every magazine's, with serial transmission)
        if (d.serial) { for (int m = 1; m <= 8; m++) { closePage(cur_[m]); cur_[m] = -1; } }
        else { closePage(cur_[mag]); cur_[mag] = -1; }
        if (!d.pageOk) return;
        const int number = d.number, subcode = d.subcode;
        const uint32_t key = (uint32_t)number << 16 | (uint32_t)subcode;
        if (pages_.size() > 4096 && !pages_.count(key)) {   // a stream with ever-changing subcodes: drop the oldest copies
            std::vector<std::pair<uint64_t, uint32_t>> age;
            for (auto& kv : pages_) age.push_back({kv.second.live.version, kv.first});
            std::sort(age.begin(), age.end());
            for (size_t i = 0; i < 512; i++) pages_.erase(age[i].second);
        }
        TtxPage& p = pages_[key].live;
        if (d.erase || p.number == 0) { p.rows = {}; p.rowMask = 0; for (int i = 0; i < 6; i++) p.fastext[i] = p.fastextSub[i] = 0; }
        p.number = number;
        p.subcode = subcode;
        p.complete = false;
        p.serial = d.serial;
        p.newsflash = d.newsflash;
        p.subtitle = d.subtitle;
        p.inhibit = d.inhibit;
        p.national = d.national;
        for (int i = 0; i < 8; i++) p.rows[0][i] = ' ';
        for (int i = 0; i < 32; i++) p.rows[0][8 + i] = d.head[i];
        p.rowMask |= 1;
        p.version = ++version_;
        cur_[mag] = key;
    } else if (d.kind == TtxUnit::Row && cur_[mag] >= 0) {
        auto it = pages_.find((uint32_t)cur_[mag]);
        if (it == pages_.end()) return;
        TtxPage& p = it->second.live;
        for (int i = 0; i < 40; i++) p.rows[row][i] = d.rowData[i];
        p.rowMask |= 1u << row;
        p.version = ++version_;
    } else if (d.kind == TtxUnit::Fastext && cur_[mag] >= 0) {
        auto it = pages_.find((uint32_t)cur_[mag]);
        if (it == pages_.end()) return;
        TtxPage& p = it->second.live;
        for (int l = 0; l < 6; l++) {
            if (d.linkAct[l] == 1) p.fastext[l] = 0;
            else if (d.linkAct[l] == 2) { p.fastext[l] = d.linkPage[l]; p.fastextSub[l] = d.linkSub[l]; }
        }
        p.version = ++version_;
    }
}

bool TeletextDecoder::page(int number, TtxPage& out, int subcode) const {
    std::lock_guard<std::mutex> lk(mu_);
    const Entry* best = nullptr;
    uint64_t bestV = 0;
    for (auto it = pages_.lower_bound((uint32_t)number << 16); it != pages_.end() && (it->first >> 16) == (uint32_t)number; ++it) {
        if (subcode >= 0 && (int)(it->first & 0xFFFF) != subcode) continue;
        const TtxPage& v = it->second.haveDone ? it->second.done : it->second.live;
        if (!best || v.version >= bestV) { best = &it->second; bestV = v.version; }
    }
    if (!best) return false;
    out = best->haveDone ? best->done : best->live;
    // the Fastext links arrive after the rows: take them from the live copy
    for (int i = 0; i < 6; i++) { out.fastext[i] = best->live.fastext[i]; out.fastextSub[i] = best->live.fastextSub[i]; }
    return true;
}

std::vector<int> TeletextDecoder::pages() const {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<int> v;
    for (auto& kv : pages_) { const int n = (int)(kv.first >> 16); if (v.empty() || v.back() != n) v.push_back(n); }
    return v;
}

std::vector<int> TeletextDecoder::subpages(int number) const {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<int> v;
    for (auto it = pages_.lower_bound((uint32_t)number << 16); it != pages_.end() && (it->first >> 16) == (uint32_t)number; ++it) v.push_back((int)(it->first & 0xFFFF));
    return v;
}


} // namespace dect2
