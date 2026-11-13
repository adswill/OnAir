// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/voice/heard_ids.h"
#include <algorithm>
#include <cmath>

namespace dect2::voice {

namespace {
template <class M> void trim(M& m, size_t cap) {
    if (m.size() <= cap) return;
    // drop the entries heard only once first
    for (auto it = m.begin(); it != m.end() && m.size() > cap;) { if (it->second <= 1) it = m.erase(it); else ++it; }
    while (m.size() > cap) m.erase(m.begin());
}
}

void HeardIds::note(uint32_t id) { ids_[id]++; trim(ids_, kMax); }
void HeardIds::noteText(const std::string& s) { if (!s.empty()) texts_[s]++; trim(texts_, kMax); }

std::vector<HeardIds::Cand> HeardIds::nearIds(uint32_t rx, int maxFlips, size_t maxN) const {
    std::vector<Cand> out;
    for (const auto& kv : ids_) {
        if (kv.first == rx) continue;
        const int f = __builtin_popcount(kv.first ^ rx);
        if (f <= maxFlips) out.push_back({kv.first, f, kv.second});
    }
    std::sort(out.begin(), out.end(), [](const Cand& a, const Cand& b) { return a.flips != b.flips ? a.flips < b.flips : a.count > b.count; });
    if (out.size() > maxN) out.resize(maxN);
    return out;
}

std::vector<HeardIds::TextCand> HeardIds::nearText(const std::string& rx, int maxSubs, size_t maxN) const {
    std::vector<TextCand> out;
    for (const auto& kv : texts_) {
        if (kv.first.size() != rx.size() || kv.first == rx) continue;
        int d = 0;
        for (size_t i = 0; i < rx.size() && d <= maxSubs; i++) d += kv.first[i] != rx[i];
        if (d <= maxSubs) out.push_back({kv.first, d, kv.second});
    }
    std::sort(out.begin(), out.end(), [](const TextCand& a, const TextCand& b) { return a.subs != b.subs ? a.subs < b.subs : a.count > b.count; });
    if (out.size() > maxN) out.resize(maxN);
    return out;
}

double HeardIds::accidentalChance(int bits, int maxFlips) const {
    double ball = 1, c = 1;
    for (int k = 1; k <= maxFlips; k++) { c = c * (bits - k + 1) / k; ball += c; }
    return std::min(1.0, (double)ids_.size() * ball / std::pow(2.0, bits));
}

} // namespace dect2::voice
