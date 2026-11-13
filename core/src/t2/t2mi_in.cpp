// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/t2/t2mi_in.h"

namespace dect2 {

void T2miIngest::reset() { pid_ = -1; probe_.clear(); plp_.clear(); count_.clear(); lastIdx_.clear(); }
std::vector<int> T2miIngest::plps() const { std::vector<int> v; for (auto& k : plp_) v.push_back(k.first); return v; }
uint64_t T2miIngest::packets(int p) const { auto it = plp_.find(p); return it == plp_.end() ? 0 : it->second.stats().packets; }
uint64_t T2miIngest::framesLost(int p) const { auto it = plp_.find(p); return it == plp_.end() ? 0 : it->second.stats().framesLost; }

void T2miIngest::drain(t2mi::Extract& e) {
    for (size_t i = 0; i < e.frames.size(); i++) {
        const int plp = e.plpIds[i];
        auto& up = plp_[plp];
        if (!lastIdx_.count(plp)) up.setSink([this, plp](const uint8_t* p) { if (sink_) sink_(plp, p); });
        const int idx = e.frameIdx[i];
        lastIdx_[plp] = idx;
        std::vector<uint8_t> bits(e.frames[i].size() * 8);
        for (size_t b = 0; b < e.frames[i].size(); b++) for (int k = 0; k < 8; k++) bits[b * 8 + (size_t)k] = (e.frames[i][b] >> (7 - k)) & 1;
        BbHeader h;
        if (!parseBbHeader(bits.data(), h) || !h.crcOk) { up.lost(); continue; }
        up.push(bits, h);
        count_[plp]++;
    }
    e.frames.clear(); e.plpIds.clear(); e.frameIdx.clear(); e.counts.clear();
}

void T2miIngest::feed(const uint8_t* p) {
    if (p[0] != 0x47 || (p[1] & 0x80)) return;
    const int pid = ((p[1] & 0x1F) << 8) | p[2];
    if (pid_ >= 0) {
        if (pid != pid_) return;
        auto& e = probe_[pid_];
        e->feed(p);
        drain(*e);
        return;
    }
    if (pid == 0x1FFF) return;
    if (!(p[1] & 0x40) && !probe_.count(pid)) return;    // start on packets with a unit start
    if (!probe_.count(pid)) { if (probe_.size() >= 48) return; probe_[pid] = std::make_unique<t2mi::Extract>(pid); }
    auto& e = probe_[pid];
    e->feed(p);
    if (e->frames.size() >= 4 && e->crcBad <= e->frames.size() / 4) {
        pid_ = pid;
        drain(*e);
        for (auto it = probe_.begin(); it != probe_.end();) if (it->first != pid_) it = probe_.erase(it); else ++it;
    }
}

}  // namespace dect2
