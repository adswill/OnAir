// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Protocol lab: the whole analysis. Several demodulators, both polarities and every line coding are tried; the result that explains the
// packets best (a CRC that holds, a sync word shared by the packets, a clean baud rate) comes first.
#include "dect2/protolab/protolab.h"
#include <algorithm>
#include <cmath>

namespace dect2::protolab {

namespace {
struct Stage {
    Candidate c;
    std::vector<std::vector<uint8_t>> bits;   // decoded bits per packet
    std::vector<int> ss;                      // sync start per packet (bit index), -1 none
    int delta0 = 0;                           // best guess of how many bits before the sync the first byte starts
    bool hdlc = false;
    double structural = 0;
};

double pairFractionOne(const std::vector<uint8_t>& p, int phase) {
    size_t good = 0, all = 0;
    const size_t lim = std::min<size_t>(p.size(), 96);
    for (size_t i = (size_t)phase; i + 1 < lim; i += 2) { all++; good += p[i] != p[i + 1]; }
    return all ? (double)good / (double)all : 0;
}

bool hasFlag(const std::vector<uint8_t>& b) {
    static const uint8_t f[8] = {0, 1, 1, 1, 1, 1, 1, 0};
    for (size_t i = 0; i + 8 <= b.size(); i++) if (std::equal(f, f + 8, b.begin() + (long)i)) return true;
    return false;
}

// how much a CRC result counts: a catalog model beats one fitted to the packets; a longer covered range beats a short one
double crcScore(const CrcHit& h) {
    const bool named = h.name.find("unnamed") == std::string::npos;
    double base = h.isChecksum ? 55.0 : named ? 100.0 : 75.0;
    return base + 10.0 * h.matches / std::max(1, h.total) + std::min(h.coveredBytes, 10) * 0.5 + ((h.init == 0 && h.xorout == 0) ? 1.0 : 0.0);
}

int bestConfident(const std::vector<CrcHit>& h, int needTotal = 3) {
    for (size_t i = 0; i < h.size(); i++) if (h[i].total >= needTotal && h[i].matches >= 3 && h[i].matches * 10 >= h[i].total * 6) return (int)i;
    return -1;
}

std::vector<std::vector<uint8_t>> toBytes(const Stage& s, int delta, bool lsb) {
    std::vector<std::vector<uint8_t>> out;
    for (size_t i = 0; i < s.bits.size(); i++) {
        if (s.hdlc) { out.push_back(bitsToBytes(s.bits[i], 0, true)); continue; }
        if (s.ss[i] < 0 || s.ss[i] - delta < 0) continue;
        out.push_back(bitsToBytes(s.bits[i], (size_t)(s.ss[i] - delta), lsb));
    }
    return out;
}

// the CRC models of the catalog on every byte grid and bit order: cheap
void catalogPass(Stage& s, const AnalyseOptions& o) {
    Candidate& c = s.c;
    c.score = s.structural;
    if (!o.tryCrc) return;
    int bestDelta = s.delta0;
    bool bestLsb = s.hdlc;
    std::vector<CrcHit> bestHits;
    int bestScore = 0;
    const int syncBits = (int)c.sync.sync.size();
    CrcOptions co;
    co.generic = false;
    for (int lsb = 0; lsb < (s.hdlc ? 1 : 2); lsb++)
        for (int d = 0; d < (s.hdlc ? 1 : 8); d++) {
            const bool l = s.hdlc ? true : lsb == 1;
            auto bytes = toBytes(s, d, l);
            if (bytes.size() < 3) continue;
            co.maxStart = s.hdlc ? 2 : std::min(12, (d + syncBits) / 8 + 4);
            auto hits = searchCrc(bytes, s.hdlc ? 0 : (d + syncBits + 7) / 8, co);
            const int bi = bestConfident(hits);
            int sc = bi >= 0 ? hits[(size_t)bi].matches * 10 + (hits[(size_t)bi].isChecksum ? 0 : 5) : 0;
            if (sc > 0 && d == s.delta0) sc += 1;
            if (sc > bestScore) { bestScore = sc; bestDelta = d; bestLsb = l; bestHits = hits; }
        }
    s.delta0 = bestDelta;
    c.lsbFirst = bestLsb;
    c.packets = toBytes(s, bestDelta, bestLsb);
    c.crc = bestHits;
    const int bi = bestConfident(bestHits);
    if (bi >= 0) {
        const CrcHit& h = bestHits[(size_t)bi];
        c.score += crcScore(h);
    }
}

// the expensive part, for the candidates that have no CRC yet: solve an unknown polynomial from the packets, and try the whitening sequences
void deepPass(Stage& s, const AnalyseOptions& o) {
    Candidate& c = s.c;
    if (!o.tryCrc || bestConfident(c.crc) >= 0 || c.packets.size() < 3) return;
    const int syncBits = (int)c.sync.sync.size();
    // 1. whitening: undo each candidate sequence after the sync word and look for a catalog CRC (equal-length packets hide a whitening
    //    from the polynomial search, so this comes before it). The byte grid is a guess within a bit or so, the whitening start a guess
    //    within the bytes the packets share.
    if (!s.hdlc) {
        for (int dd : {0, -1, 1}) {
            const int delta = (s.delta0 + dd + 8) % 8;
            const std::vector<std::vector<uint8_t>> base = toBytes(s, delta, false);
            if (base.size() < 3) continue;
            const int from = (delta + syncBits) / 8;
            for (const Whitening& w : whiteningCandidates()) {
                for (int start = std::max(0, from - 3); start <= from + 1; start++) {
                    auto trial = base;
                    for (auto& p : trial) if ((int)p.size() > start) { std::vector<uint8_t> tail(p.begin() + start, p.end()); whiten(tail, w); std::copy(tail.begin(), tail.end(), p.begin() + start); }
                    CrcOptions wo;
                    wo.generic = false;
                    wo.minWidth = 16;
                    wo.maxSlack = 1;
                    wo.maxStart = start + 3;
                    auto hits = searchCrc(trial, start, wo);
                    const int wi = bestConfident(hits);
                    if (wi >= 0 && hits[(size_t)wi].matches == hits[(size_t)wi].total) {   // a short CRC would fit by accident
                        c.lsbFirst = false;
                        c.packets = trial;
                        c.crc = hits;
                        c.whitening = w.name;
                        c.score += crcScore(hits[(size_t)wi]) - 8.0;
                        return;
                    }
                }
            }
        }
    }
    // 2. an unknown polynomial: solve it from the packets
    if (o.genericCrc) {
        CrcOptions co;
        co.generic = true;
        for (int lsb = 0; lsb < (s.hdlc ? 1 : 2); lsb++) {
            const bool l = s.hdlc ? true : lsb == 1;
            auto bytes = toBytes(s, s.delta0, l);
            if (bytes.size() < 3) continue;
            co.maxStart = s.hdlc ? 2 : std::min(12, (s.delta0 + syncBits) / 8 + 4);
            auto hits = searchCrc(bytes, s.hdlc ? 0 : (s.delta0 + syncBits + 7) / 8, co);
            const int bi = bestConfident(hits);
            if (bi >= 0) {
                c.lsbFirst = l;
                c.packets = bytes;
                c.crc = hits;
                c.score += crcScore(hits[(size_t)bi]);
                return;
            }
        }
    }
}

void dedupe(std::vector<Candidate>& v) {
    std::vector<Candidate> out;
    for (const Candidate& c : v) {
        bool dup = false;
        for (const Candidate& k : out) if (k.coding == c.coding && k.packets == c.packets && k.whitening == c.whitening) { dup = true; break; }
        if (!dup) out.push_back(c);
    }
    v = std::move(out);
}

// from bit packets (raw, after the slicer) to staged candidates, one per line coding
std::vector<Stage> stages(const std::vector<std::vector<uint8_t>>& raw, const Candidate& base) {
    std::vector<Stage> out;
    if (raw.size() < 2) return out;
    auto make = [&](Coding coding, const std::vector<std::vector<uint8_t>>& dec, bool hdlc) {
        Stage s;
        s.c = base;
        s.c.coding = coding;
        s.bits = dec;
        s.hdlc = hdlc;
        if (hdlc) {
            s.ss.assign(dec.size(), 0);
            s.c.sync.found = s.c.sync.total = (int)dec.size();
            s.structural = 30.0 * std::min<size_t>(dec.size(), 8) / 8.0 + 10;
        } else {
            s.c.sync = findSync(dec);
            s.ss = s.c.sync.start;
            if (s.c.sync.total > 0) {
                s.structural = 20.0 * s.c.sync.found / s.c.sync.total + std::min<double>((double)s.c.sync.sync.size(), 32) / 4.0 + (s.c.sync.preambleBits >= 16 ? 10 : 0);
                if (s.c.sync.found < 2) s.structural = 0;
            }
            s.delta0 = s.c.sync.preambleBits >= 16 ? s.c.sync.gridDelta : 0;   // the preamble is whole bytes
        }
        if (s.c.sync.total >= 2 || hdlc) out.push_back(std::move(s));
    };
    make(Coding::NRZ, raw, false);
    {
        std::vector<std::vector<uint8_t>> d;
        for (const auto& p : raw) d.push_back(decodeLine(p, Coding::NRZI));
        make(Coding::NRZI, d, false);
        std::vector<std::vector<uint8_t>> frames;
        int withFlag = 0;
        for (const auto& p : d) {
            if (hasFlag(p)) withFlag++;
            for (auto& f : hdlcFrames(p)) frames.push_back(std::move(f));
        }
        if (withFlag * 2 >= (int)d.size() && frames.size() >= 2) make(Coding::Hdlc, frames, true);
    }
    {   // Manchester family: the pair boundary can fall on either chip, packet by packet (the first chip may be missing)
        std::vector<int> phase(raw.size(), 0);
        double sum = 0;
        for (size_t i = 0; i < raw.size(); i++) {
            const double f0 = pairFractionOne(raw[i], 0), f1 = pairFractionOne(raw[i], 1);
            phase[i] = f1 > f0 ? 1 : 0;
            sum += std::max(f0, f1);
        }
        if (sum / (double)raw.size() >= 0.93) {
            for (Coding c : {Coding::ManchesterIEEE, Coding::ManchesterThomas, Coding::DiffManchester}) {
                std::vector<std::vector<uint8_t>> d;
                for (size_t i = 0; i < raw.size(); i++) d.push_back(decodeLine(raw[i], c, phase[i]));
                make(c, d, false);
            }
        }
    }
    return out;
}
}   // namespace

Report analysePackets(const std::vector<std::vector<uint8_t>>& raw, double baud, const AnalyseOptions& o) {
    Report r;
    r.packetsFound = raw.size();
    Candidate base;
    base.demod = o.demod == Demod::Auto ? Demod::FSK2 : o.demod;
    base.baud = baud;
    base.baudQuality = 1;
    std::vector<Stage> st = stages(raw, base);
    std::sort(st.begin(), st.end(), [](const Stage& a, const Stage& b) { return a.structural > b.structural; });
    if (st.size() > 4) st.resize(4);
    for (Stage& s : st) catalogPass(s, o);
    std::stable_sort(st.begin(), st.end(), [](const Stage& a, const Stage& b) { return a.c.score > b.c.score; });
    for (size_t i = 0; i < st.size(); i++) { if (i < 2) deepPass(st[i], o); r.candidates.push_back(st[i].c); }
    std::sort(r.candidates.begin(), r.candidates.end(), [](const Candidate& a, const Candidate& b) { return a.score > b.score; });
    if (!r.candidates.empty()) {
        const Candidate& b = r.candidates[0];
        const int bi = bestConfident(b.crc);
        r.fields = analyseFields(b.packets, bi >= 0 && !b.crc[(size_t)bi].isChecksum && b.crc[(size_t)bi].endSlack == 0 ? b.crc[(size_t)bi].width / 8 : 0);
    } else r.note = "no structure found in the packets";
    return r;
}

Report analyse(const std::vector<cf>& iq, double fs, const AnalyseOptions& o) {
    Report r;
    r.bursts = findBursts(iq, fs, o.bursts);
    r.packetsFound = r.bursts.size();
    if (r.bursts.size() < 2) { r.note = r.bursts.empty() ? "no bursts found: raise the level or choose a stronger signal" : "only one burst: at least two packets are needed to compare"; return r; }
    const size_t maxBursts = 64;
    std::vector<Demod> demods;
    if (o.demod == Demod::Auto) demods = {Demod::OOK, Demod::FSK2, Demod::FSK4, Demod::PSK2, Demod::PSK4};
    else demods = {o.demod};
    std::vector<Stage> all;
    for (Demod d : demods) {
        if (o.demod == Demod::Auto && (d == Demod::PSK2 || d == Demod::PSK4)) {   // phase modulation only when nothing simpler explained the packets
            bool good = false;
            for (Stage& s : all) good |= s.structural >= 40;
            if (good) break;
        }
        std::vector<Soft> soft;
        for (size_t i = 0; i < r.bursts.size() && i < maxBursts; i++)
            soft.push_back(demodulate(iq.data() + r.bursts[i].start, r.bursts[i].end - r.bursts[i].start, d, fs));
        std::vector<const Soft*> ptr;
        for (const Soft& s : soft) ptr.push_back(&s);
        BaudEstimate be = estimateBaud(ptr, fs, d == Demod::FSK4 ? 4 : 2);
        const double baud = o.baud > 0 ? o.baud : be.baud;
        if (baud <= 0) continue;
        if (o.baud <= 0 && be.quality < 0.3) continue;   // the intervals are not whole multiples of anything
        const int levels = d == Demod::FSK4 ? 4 : 2;
        for (int inv = 0; inv < 2; inv++) {
            std::vector<std::vector<uint8_t>> raw;
            for (const Soft& s : soft) {
                auto bits = sliceBits(s, fs, baud, levels, inv == 1);
                if (bits.size() >= 16) raw.push_back(std::move(bits));
            }
            Candidate base;
            base.demod = d;
            base.baud = baud;
            base.baudQuality = be.quality;
            base.levels = d == Demod::PSK4 ? 4 : levels;
            base.invert = inv == 1;
            std::vector<Stage> st = stages(raw, base);
            std::sort(st.begin(), st.end(), [](const Stage& a, const Stage& b) { return a.structural > b.structural; });
            if (st.size() > 3) st.resize(3);
            for (Stage& s : st) {
                if (s.structural < 8) continue;
                if (inv == 1 && s.c.coding != Coding::NRZ) continue;   // differential and Manchester readings do not depend on the polarity
                s.structural += 10.0 * be.quality - ((d == Demod::PSK2 || d == Demod::PSK4) ? 4.0 : 0.0);   // a PSK reading must beat the simpler ones clearly
                if (inv == 1) s.structural -= 0.5;
                all.push_back(std::move(s));
            }
        }
    }
    std::stable_sort(all.begin(), all.end(), [](const Stage& a, const Stage& b) { return a.structural > b.structural; });
    if (all.size() > 12) all.resize(12);   // the CRC search is for the twelve best-looking readings only
    for (Stage& s : all) catalogPass(s, o);
    std::stable_sort(all.begin(), all.end(), [](const Stage& a, const Stage& b) { return a.c.score > b.c.score; });
    for (size_t i = 0; i < all.size() && i < 4; i++) { deepPass(all[i], o); }   // only the best few get the expensive search
    for (Stage& s : all) r.candidates.push_back(s.c);
    std::stable_sort(r.candidates.begin(), r.candidates.end(), [](const Candidate& a, const Candidate& b) { return a.score > b.score; });
    dedupe(r.candidates);
    if (r.candidates.size() > 8) r.candidates.resize(8);
    if (r.candidates.empty()) { r.note = "no demodulator gave bits with a clear structure; try another demodulator, a baud rate by hand, or a cleaner capture"; return r; }
    const Candidate& b = r.candidates[0];
    const int bi = bestConfident(b.crc);
    r.fields = analyseFields(b.packets, bi >= 0 && !b.crc[(size_t)bi].isChecksum && b.crc[(size_t)bi].endSlack == 0 ? b.crc[(size_t)bi].width / 8 : 0);
    return r;
}

}   // namespace dect2::protolab
