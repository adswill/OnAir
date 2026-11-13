// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Protocol lab: line codings, HDLC framing, preamble and sync search, whitening.
#include "dect2/protolab/protolab.h"
#include <algorithm>
#include <cmath>

namespace dect2::protolab {

const char* codingName(Coding c) {
    switch (c) {
        case Coding::NRZ: return "NRZ";
        case Coding::NRZI: return "NRZI (differential)";
        case Coding::ManchesterIEEE: return "Manchester (01 = 1)";
        case Coding::ManchesterThomas: return "Manchester (10 = 1)";
        case Coding::DiffManchester: return "Differential Manchester";
        case Coding::Hdlc: return "HDLC (NRZI + bit stuffing)";
    }
    return "?";
}

std::vector<uint8_t> decodeLine(const std::vector<uint8_t>& b, Coding c, int phase) {
    std::vector<uint8_t> o;
    switch (c) {
        case Coding::NRZ: return b;
        case Coding::NRZI:
        case Coding::Hdlc:   // the stuffing is removed per frame by hdlcFrames
            for (size_t i = 1; i < b.size(); i++) o.push_back(b[i] == b[i - 1] ? 1 : 0);
            return o;
        case Coding::ManchesterIEEE:
        case Coding::ManchesterThomas:
            for (size_t i = (size_t)phase; i + 1 < b.size(); i += 2) {
                if (b[i] == b[i + 1]) break;   // not a Manchester pair: the packet is over (or damaged)
                const bool oneIs01 = c == Coding::ManchesterIEEE;
                o.push_back((uint8_t)((b[i] == 0 && b[i + 1] == 1) == oneIs01 ? 1 : 0));
            }
            return o;
        case Coding::DiffManchester:
            for (size_t i = (size_t)phase; i + 1 < b.size(); i += 2) {
                if (b[i] == b[i + 1]) break;
                const uint8_t prev = i >= 1 ? b[i - 1] : (uint8_t)(1 - b[i]);
                o.push_back(b[i] == prev ? 1 : 0);   // no transition at the start of the cell: 1
            }
            return o;
    }
    return o;
}

std::vector<std::vector<uint8_t>> hdlcFrames(const std::vector<uint8_t>& b) {
    std::vector<std::vector<uint8_t>> frames;
    static const uint8_t flag[8] = {0, 1, 1, 1, 1, 1, 1, 0};
    std::vector<size_t> flags;
    for (size_t i = 0; i + 8 <= b.size(); i++) {
        if (std::equal(flag, flag + 8, b.begin() + (long)i)) { flags.push_back(i); i += 7; }
    }
    for (size_t k = 0; k + 1 < flags.size(); k++) {
        const size_t from = flags[k] + 8, to = flags[k + 1];
        if (to <= from + 16) continue;   // two flags in a row: idle
        std::vector<uint8_t> f;
        int ones = 0;
        bool bad = false;
        for (size_t i = from; i < to; i++) {
            if (ones == 5) {
                ones = 0;
                if (b[i] == 0) continue;   // stuffed zero
                bad = true;
                break;
            }
            f.push_back(b[i]);
            ones = b[i] ? ones + 1 : 0;
        }
        if (bad) continue;
        f.resize(f.size() / 8 * 8);
        if (f.size() >= 16) frames.push_back(std::move(f));
    }
    return frames;
}

std::vector<uint8_t> bitsToBytes(const std::vector<uint8_t>& bits, size_t from, bool lsb) {
    std::vector<uint8_t> o;
    for (size_t i = from; i + 8 <= bits.size(); i += 8) {
        uint8_t v = 0;
        for (int j = 0; j < 8; j++) {
            const uint8_t bit = bits[i + (size_t)j] & 1;
            if (lsb) v |= (uint8_t)(bit << j);
            else v = (uint8_t)((v << 1) | bit);
        }
        o.push_back(v);
    }
    return o;
}

std::vector<uint8_t> bytesToBits(const std::vector<uint8_t>& bytes, bool lsb) {
    std::vector<uint8_t> o;
    for (uint8_t v : bytes)
        for (int j = 0; j < 8; j++) o.push_back(lsb ? (uint8_t)((v >> j) & 1) : (uint8_t)((v >> (7 - j)) & 1));
    return o;
}

namespace {
struct Pre { int period = 0; int start = 0; int end = 0; };   // leading repeating pattern: bits [start, end)

Pre preambleOf(const std::vector<uint8_t>& b) {
    Pre best;
    const int n = (int)b.size();
    for (int p = 1; p <= 16; p++) {
        // the longest run of b[i] == b[i-p] within the first 400 bits, the pattern having at least two distinct bits unless p == 1
        int bestRun = 0, bestStart = 0;
        int run = 0, st = 0;
        for (int i = p; i < std::min(n, 400); i++) {
            if (b[(size_t)i] == b[(size_t)(i - p)]) { if (run == 0) st = i - p; run++; }
            else { if (run > bestRun && st < 16) { bestRun = run; bestStart = st; } run = 0; }
        }
        if (run > bestRun && st < 16) { bestRun = run; bestStart = st; }
        const int total = bestRun + p;
        if (total > best.end - best.start + 1 && total >= 12 && bestRun >= 10 && bestStart < 16) {
            // reject a period that is only a multiple of a smaller one
            best.period = p;
            best.start = bestStart;
            best.end = bestStart + total;
        }
    }
    return best;
}

bool periodicWindow(const std::vector<uint8_t>& b, size_t at, size_t len) {
    for (size_t p = 1; p <= 8; p++) {
        bool ok = true;
        for (size_t i = p; i < len && ok; i++) ok = b[at + i] == b[at + i - p];
        if (ok) return true;
    }
    return false;
}
}   // namespace

SyncInfo findSync(const std::vector<std::vector<uint8_t>>& pk) {
    SyncInfo si;
    std::vector<size_t> use;
    for (size_t i = 0; i < pk.size(); i++) if (pk[i].size() >= 24) use.push_back(i);
    si.total = (int)use.size();
    si.start.assign(pk.size(), -1);
    if (use.size() < 2) return si;

    std::vector<Pre> pre;
    int withPre = 0;
    std::vector<int> lens;
    for (size_t i : use) {
        pre.push_back(preambleOf(pk[i]));
        if (pre.back().period) { withPre++; lens.push_back(pre.back().end - pre.back().start); }
    }
    const size_t L = 16;
    if (withPre * 10 >= (int)use.size() * 6) {
        std::sort(lens.begin(), lens.end());
        si.preambleBits = lens[lens.size() / 2];
        {
            std::vector<int> g;
            for (int l : lens) g.push_back(l % 8);
            std::sort(g.begin(), g.end());
            si.gridDelta = g[g.size() / 2];
        }
        size_t ref = 0;
        for (size_t k = 0; k < use.size(); k++) if (pre[k].period) { ref = k; break; }
        for (int j = 0; j < pre[ref].period; j++) si.preamblePattern.push_back(pk[use[ref]][(size_t)(pre[ref].start + j)]);
        for (size_t k = 0; k < use.size(); k++) if (pre[k].period) si.start[use[k]] = pre[k].end;
    } else {
        // no preamble: the 16-bit window that most packets share (not a repeating pattern, not constant)
        int bestSupport = 0, bestPos = -1;
        std::vector<uint8_t> bestWin;
        const std::vector<uint8_t>& p0 = pk[use[0]];
        for (size_t pos = 0; pos + L <= std::min<size_t>(p0.size(), 160); pos++) {
            if (periodicWindow(p0, pos, L)) continue;
            int support = 0;
            for (size_t i : use) {
                const auto& q = pk[i];
                const size_t lim = std::min<size_t>(q.size(), 320);
                bool has = false;
                for (size_t t = 0; t + L <= lim && !has; t++) has = std::equal(p0.begin() + (long)pos, p0.begin() + (long)(pos + L), q.begin() + (long)t);
                support += has;
            }
            if (support > bestSupport) { bestSupport = support; bestPos = (int)pos; bestWin.assign(p0.begin() + (long)pos, p0.begin() + (long)(pos + L)); }
        }
        if (bestPos < 0 || bestSupport * 10 < (int)use.size() * 6) return si;
        for (size_t i : use) {
            const auto& q = pk[i];
            for (size_t t = 0; t + L <= std::min<size_t>(q.size(), 320); t++)
                if (std::equal(bestWin.begin(), bestWin.end(), q.begin() + (long)t)) { si.start[i] = (int)t; break; }
        }
    }
    // the bits the packets share from there on (at most 32)
    const std::vector<uint8_t>* ref = nullptr;
    int refStart = 0;
    for (size_t i : use) if (si.start[i] >= 0) { ref = &pk[i]; refStart = si.start[i]; break; }
    int have = 0;
    for (size_t i : use) have += si.start[i] >= 0;
    si.found = have;
    if (!ref) return si;
    for (int k = 0; k < 32; k++) {
        if ((size_t)(refStart + k) >= ref->size()) break;
        int agree = 0, present = 0;
        for (size_t i : use) {
            if (si.start[i] < 0) continue;
            const size_t at = (size_t)(si.start[i] + k);
            if (at >= pk[i].size()) continue;
            present++;
            agree += pk[i][at] == (*ref)[(size_t)(refStart + k)];
        }
        if (present * 10 < have * 8 || agree * 10 < present * 9) break;
        si.sync.push_back((*ref)[(size_t)(refStart + k)]);
    }
    return si;
}

const std::vector<Whitening>& whiteningCandidates() {
    static const std::vector<Whitening> w = {
        {"PN9 (x^9+x^5+1, seed 0x1FF)", 9, 0x021, 0x1FF, true},
        {"PN7 (x^7+x^4+1, seed 0x7F)", 7, 0x09, 0x7F, false},
        {"PN9 (x^9+x^4+1, seed 0x1FF)", 9, 0x011, 0x1FF, true},
    };
    return w;
}

void whiten(std::vector<uint8_t>& bytes, const Whitening& w) {
    uint32_t reg = w.seed;
    for (uint8_t& v : bytes) {
        uint8_t x = 0;
        for (int b = 0; b < 8; b++) {
            x |= (uint8_t)((reg & 1) << b);
            uint32_t t = reg & w.poly;
            t ^= t >> 16; t ^= t >> 8; t ^= t >> 4; t ^= t >> 2; t ^= t >> 1;
            const uint32_t fb = t & 1;
            reg = (reg >> 1) | (fb << (w.width - 1));
        }
        v ^= x;
    }
}

}   // namespace dect2::protolab
