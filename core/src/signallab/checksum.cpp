// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Signal Lab stage 5b: checksum catalogue (Rocksoft-model CRCs, Fletcher, plain sums) and the search that locates a checksum in the
// frames: every coverage start (bit by bit, and with the sync word in front), every checksum position, both bit orders inside the
// bytes, both byte orders of the stored value and both polarities. A hit needs most frames to agree and the covered data to vary.
#include "dect2/signallab/signallab.h"
#include <algorithm>
#include <cmath>
#include <set>

namespace dect2::signallab {

const std::vector<CheckAlgo>& checkAlgos() {
    static const std::vector<CheckAlgo> a = {
        {"CRC-8", CheckKind::CRC, 8, 0x07, 0x00, false, false, 0x00, 0xF4},
        {"CRC-8/MAXIM-DOW", CheckKind::CRC, 8, 0x31, 0x00, true, true, 0x00, 0xA1},
        {"CRC-8/SAE-J1850", CheckKind::CRC, 8, 0x1D, 0xFF, false, false, 0xFF, 0x4B},
        {"CRC-8/DVB-S2", CheckKind::CRC, 8, 0xD5, 0x00, false, false, 0x00, 0xBC},
        {"CRC-8/AUTOSAR", CheckKind::CRC, 8, 0x2F, 0xFF, false, false, 0xFF, 0xDF},
        {"CRC-8/WCDMA", CheckKind::CRC, 8, 0x9B, 0x00, true, true, 0x00, 0x25},
        {"CRC-16/IBM-3740 (CCITT-FALSE)", CheckKind::CRC, 16, 0x1021, 0xFFFF, false, false, 0x0000, 0x29B1},
        {"CRC-16/XMODEM", CheckKind::CRC, 16, 0x1021, 0x0000, false, false, 0x0000, 0x31C3},
        {"CRC-16/KERMIT", CheckKind::CRC, 16, 0x1021, 0x0000, true, true, 0x0000, 0x2189},
        {"CRC-16/IBM-SDLC (X-25)", CheckKind::CRC, 16, 0x1021, 0xFFFF, true, true, 0xFFFF, 0x906E},
        {"CRC-16/GENIBUS", CheckKind::CRC, 16, 0x1021, 0xFFFF, false, false, 0xFFFF, 0xD64E},
        {"CRC-16/MCRF4XX", CheckKind::CRC, 16, 0x1021, 0xFFFF, true, true, 0x0000, 0x6F91},
        {"CRC-16/ARC", CheckKind::CRC, 16, 0x8005, 0x0000, true, true, 0x0000, 0xBB3D},
        {"CRC-16/MODBUS", CheckKind::CRC, 16, 0x8005, 0xFFFF, true, true, 0x0000, 0x4B37},
        {"CRC-16/USB", CheckKind::CRC, 16, 0x8005, 0xFFFF, true, true, 0xFFFF, 0xB4C8},
        {"CRC-16/UMTS (BUYPASS)", CheckKind::CRC, 16, 0x8005, 0x0000, false, false, 0x0000, 0xFEE8},
        {"CRC-16/DNP", CheckKind::CRC, 16, 0x3D65, 0x0000, true, true, 0xFFFF, 0xEA82},
        {"CRC-16/EN-13757", CheckKind::CRC, 16, 0x3D65, 0x0000, false, false, 0xFFFF, 0xC2B7},
        {"CRC-32/ISO-HDLC", CheckKind::CRC, 32, 0x04C11DB7, 0xFFFFFFFF, true, true, 0xFFFFFFFF, 0xCBF43926},
        {"CRC-32/BZIP2", CheckKind::CRC, 32, 0x04C11DB7, 0xFFFFFFFF, false, false, 0xFFFFFFFF, 0xFC891918},
        {"CRC-32/MPEG-2", CheckKind::CRC, 32, 0x04C11DB7, 0xFFFFFFFF, false, false, 0x00000000, 0x0376E6E7},
        {"CRC-32/CKSUM", CheckKind::CRC, 32, 0x04C11DB7, 0x00000000, false, false, 0xFFFFFFFF, 0x765E7680},
        {"CRC-32C (ISCSI)", CheckKind::CRC, 32, 0x1EDC6F41, 0xFFFFFFFF, true, true, 0xFFFFFFFF, 0xE3069283},
        {"Fletcher-16", CheckKind::Fletcher16, 16, 0, 0, false, false, 0, 0x1EDE},
        {"Sum-8", CheckKind::Sum8, 8, 0, 0, false, false, 0, 0xDD},
        {"XOR-8", CheckKind::Xor8, 8, 0, 0, false, false, 0, 0x31},
        {"Sum-16", CheckKind::Sum16, 16, 0, 0, false, false, 0, 0x01DD},
    };
    return a;
}

namespace {
uint32_t reflect(uint32_t v, int w) { uint32_t r = 0; for (int i = 0; i < w; i++) if (v & (1u << i)) r |= 1u << (w - 1 - i); return r; }
uint32_t maskOf(int w) { return w >= 32 ? 0xFFFFFFFFu : ((1u << w) - 1); }

// a running checksum: start(), feed bytes, value() at any point
struct Run {
    const CheckAlgo* a = nullptr;
    const uint32_t* tab = nullptr;
    uint32_t reg = 0, s1 = 0, s2 = 0;
    void start() {
        if (a->kind == CheckKind::CRC) reg = a->refin ? reflect(a->init, a->width) : a->init;
        else { reg = 0; s1 = 0; s2 = 0; }
    }
    void feed(uint8_t b) {
        switch (a->kind) {
            case CheckKind::CRC:
                if (a->refin) reg = (reg >> 8) ^ tab[(reg ^ b) & 0xFF];
                else if (a->width >= 8) reg = ((reg << 8) ^ tab[((reg >> (a->width - 8)) ^ b) & 0xFF]) & maskOf(a->width);
                break;
            case CheckKind::Fletcher16: s1 = (s1 + b) % 255; s2 = (s2 + s1) % 255; break;
            case CheckKind::Sum8: reg = (reg + b) & 0xFF; break;
            case CheckKind::Xor8: reg ^= b; break;
            case CheckKind::Sum16: reg = (reg + b) & 0xFFFF; break;
        }
    }
    uint32_t value() const {
        if (a->kind == CheckKind::CRC) return (reg ^ a->xorout) & maskOf(a->width);   // refin == refout for every catalogue entry
        if (a->kind == CheckKind::Fletcher16) return (s2 << 8) | s1;
        return reg;
    }
};

const std::vector<std::vector<uint32_t>>& tables() {
    static const std::vector<std::vector<uint32_t>> t = [] {
        std::vector<std::vector<uint32_t>> all;
        for (const CheckAlgo& a : checkAlgos()) {
            std::vector<uint32_t> tab(256, 0);
            if (a.kind == CheckKind::CRC) {
                const uint32_t m = maskOf(a.width);
                for (uint32_t b = 0; b < 256; b++) {
                    uint32_t r;
                    if (a.refin) {
                        const uint32_t rp = reflect(a.poly, a.width);
                        r = b;
                        for (int k = 0; k < 8; k++) r = (r & 1) ? (r >> 1) ^ rp : r >> 1;
                    } else {
                        r = b << (a.width - 8);
                        const uint32_t top = 1u << (a.width - 1);
                        for (int k = 0; k < 8; k++) r = (r & top) ? ((r << 1) ^ a.poly) & m : (r << 1) & m;
                    }
                    tab[b] = r & m;
                }
            }
            all.push_back(std::move(tab));
        }
        return all;
    }();
    return t;
}

uint32_t readValue(const Bits& f, size_t at, int w, bool lsb, bool little) {
    const int nb = w / 8;
    uint32_t v = 0;
    for (int k = 0; k < nb; k++) {
        uint32_t byte = 0;
        for (int i = 0; i < 8; i++) { const uint8_t bit = f[at + (size_t)k * 8 + (size_t)i]; byte |= lsb ? (uint32_t)bit << i : (uint32_t)bit << (7 - i); }
        if (little) v |= byte << (8 * k); else v = (v << 8) | byte;
    }
    return v;
}
}

uint32_t checksum(const CheckAlgo& a, const uint8_t* d, size_t n) {
    const size_t idx = (size_t)(&a - &checkAlgos()[0]);
    Run r;
    r.a = &a;
    r.tab = idx < tables().size() ? tables()[idx].data() : nullptr;
    if (!r.tab) return 0;   // only catalogue entries have tables
    r.start();
    for (size_t i = 0; i < n; i++) r.feed(d[i]);
    return r.value();
}

std::vector<uint8_t> packBytes(const Bits& b, size_t from, size_t nBits, bool lsbFirst) {
    std::vector<uint8_t> o;
    for (size_t i = 0; i + 8 <= nBits && from + i + 8 <= b.size(); i += 8) {
        uint8_t v = 0;
        for (int k = 0; k < 8; k++) { const uint8_t bit = b[from + i + (size_t)k]; v |= lsbFirst ? (uint8_t)(bit << k) : (uint8_t)(bit << (7 - k)); }
        o.push_back(v);
    }
    return o;
}

std::vector<ChecksumHit> findChecksums(const std::vector<Bits>& frames0, const Bits& sync, int maxHits) {
    std::vector<ChecksumHit> hits;
    if (frames0.size() < 2) return hits;
    std::vector<Bits> frames(frames0.begin(), frames0.begin() + (long)std::min<size_t>(frames0.size(), 48));
    size_t L = SIZE_MAX, Lmax = 0;
    for (const Bits& f : frames) { L = std::min(L, f.size()); Lmax = std::max(Lmax, f.size()); }
    const bool variable = Lmax > L + 16;
    const int nf = (int)frames.size();
    const int need = std::max(2, (int)std::ceil(0.8 * nf));
    const auto& algos = checkAlgos();
    const auto& tabs = tables();
    const bool syncBytes = sync.size() >= 8 && sync.size() % 8 == 0;
    for (int inv = 0; inv < 2; inv++) {
        std::vector<Bits> fr = frames;
        if (inv) for (auto& f : fr) f = invert(f);
        for (int withSync = 0; withSync < (syncBytes ? 2 : 1); withSync++) {
            std::vector<Bits> data = fr;
            if (withSync) for (auto& f : data) { Bits s = inv ? invert(sync) : sync; s.insert(s.end(), f.begin(), f.end()); f.swap(s); }
            const size_t off = withSync ? sync.size() : 0;
            const size_t maxStart = withSync ? 0 : std::min<size_t>(40, L > 24 ? L - 24 : 0);
            for (size_t s = 0; s <= maxStart; s++) {
                // columns that vary from frame to frame, counted from s: a check over a few changing bits (a counter after a
                // wrong line code) is an affine function of them and can match by chance
                std::vector<int> varCols(L + off + 1, 0);
                for (size_t k = s; k < L + off; k++) {
                    bool z = false, o = false;
                    for (const Bits& f : data) { if (f[k]) o = true; else z = true; }
                    varCols[k - s + 1] = varCols[k - s] + (z && o ? 1 : 0);
                }
                for (int lsb = 0; lsb < 2; lsb++) {
                    std::vector<std::vector<uint8_t>> bytes;
                    for (const Bits& f : data) bytes.push_back(packBytes(f, s, f.size() - s, lsb));
                    for (size_t ai = 0; ai < algos.size(); ai++) {
                        const CheckAlgo& a = algos[ai];
                        const int w = a.width;
                        // fixed length: counts per (end byte, byte order); variable: per tail trim
                        const size_t nb = (L + off - s) / 8;
                        std::vector<int> cnt(2 * (nb + 1), 0), cntV(2 * 9, 0);
                        std::vector<std::set<uint32_t>> varied(2 * (nb + 1));
                        std::vector<std::set<uint8_t>> content(nb + 1);   // distinct bytes covered (periodic or empty data fits anything)
                        for (int fi = 0; fi < nf; fi++) {
                            Run r; r.a = &a; r.tab = tabs[ai].data(); r.start();
                            const Bits& f = data[(size_t)fi];
                            const auto& by = bytes[(size_t)fi];
                            std::vector<uint32_t> vals(by.size() + 1);
                            vals[0] = r.value();
                            for (size_t j = 0; j < by.size(); j++) { r.feed(by[j]); vals[j + 1] = r.value(); }
                            for (size_t j = 2; j <= nb && j < vals.size(); j++) {
                                const size_t at = s + 8 * j;
                                if (at + (size_t)w > L + off) break;
                                for (int little = 0; little < (w > 8 ? 2 : 1); little++) {
                                    if (readValue(f, at, w, lsb, little) == vals[j]) {
                                        cnt[2 * j + (size_t)little]++;
                                        auto& vs = varied[2 * j + (size_t)little];
                                        if (vs.size() < 3) vs.insert(vals[j]);   // different values: the covered data differ
                                        auto& cs = content[j];
                                        for (size_t q = 0; q < j && cs.size() < 8; q++) cs.insert(by[q]);
                                    }
                                }
                            }
                            if (variable) {
                                for (int t = 0; t <= 8; t++) {
                                    if (f.size() < s + (size_t)w + (size_t)t + 16) continue;
                                    const size_t at = f.size() - (size_t)w - (size_t)t;
                                    if ((at - s) % 8) continue;
                                    const size_t j = (at - s) / 8;
                                    if (j >= vals.size()) continue;
                                    for (int little = 0; little < (w > 8 ? 2 : 1); little++)
                                        if (readValue(f, at, w, lsb, little) == vals[j]) cntV[2 * (size_t)t + (size_t)little]++;
                                }
                            }
                        }
                        // plain sums and 8-bit checks are easy to hit by chance (sparse data, many positions tried): ask for more frames
                        int needW = w == 8 ? std::max(need, 4) : need;
                        if (a.kind != CheckKind::CRC) needW = std::max({needW, 6, (int)std::ceil(0.9 * nf)});
                        std::vector<ChecksumHit> here;
                        for (size_t j = 2; j <= nb; j++) for (int little = 0; little < 2; little++) {
                            const int c = cnt[2 * j + (size_t)little];
                            if (c < needW || varied[2 * j + (size_t)little].size() < 2 || content[j].size() < std::min<size_t>(6, j + 2)) continue;
                            if ((size_t)varCols[std::min(8 * j, varCols.size() - 1)] < std::min<size_t>((size_t)w + 4, 4 * j)) continue;
                            ChecksumHit h;
                            h.algo = (int)ai; h.coversSync = withSync; h.coverStart = s; h.coverEnd = s + 8 * j - off;
                            h.lsbFirst = lsb; h.inverted = inv; h.matched = c; h.tested = nf;
                            h.coverStart = withSync ? 0 : s; h.littleEndian = little;
                            here.push_back(h);
                        }
                        // a real check holds at one end position; one that holds almost anywhere comes from the decoding (a line
                        // code / descrambler polynomial that is the CRC polynomial makes every window a code word): drop them all
                        if (here.size() <= 2) hits.insert(hits.end(), here.begin(), here.end());
                        if (variable) for (int t = 0; t <= 8; t++) for (int little = 0; little < 2; little++) {
                            const int c = cntV[2 * (size_t)t + (size_t)little];
                            if (c < needW) continue;
                            ChecksumHit h;
                            h.algo = (int)ai; h.coversSync = withSync; h.coverStart = withSync ? 0 : s; h.tailBits = (size_t)t;
                            h.lsbFirst = lsb; h.inverted = inv; h.matched = c; h.tested = nf; h.lengthFromEnd = true; h.littleEndian = little;
                            hits.push_back(h);
                        }
                    }
                }
            }
        }
    }
    std::sort(hits.begin(), hits.end(), [&](const ChecksumHit& a, const ChecksumHit& b) {
        if (a.matched != b.matched) return a.matched > b.matched;
        const int wa = algos[(size_t)a.algo].width, wb = algos[(size_t)b.algo].width;
        if (wa != wb) return wa > wb;
        const bool ca = algos[(size_t)a.algo].kind == CheckKind::CRC, cb = algos[(size_t)b.algo].kind == CheckKind::CRC;
        if (ca != cb) return ca;
        if (a.coversSync != b.coversSync) return a.coversSync;
        // a reflected CRC on LSB-first bytes is the same bit-level check as its plain twin on MSB-first bytes: prefer the natural pairing
        const bool na = algos[(size_t)a.algo].refin == a.lsbFirst, nb = algos[(size_t)b.algo].refin == b.lsbFirst;
        if (na != nb) return na;
        if (a.inverted != b.inverted) return !a.inverted;
        return a.coverStart < b.coverStart;
    });
    std::vector<ChecksumHit> out;
    for (const ChecksumHit& h : hits) {
        bool dup = false;
        for (const ChecksumHit& o : out) if (o.algo == h.algo && o.coverEnd == h.coverEnd && o.lengthFromEnd == h.lengthFromEnd && o.tailBits == h.tailBits) dup = true;
        if (!dup) out.push_back(h);
        if ((int)out.size() >= maxHits) break;
    }
    return out;
}

} // namespace dect2::signallab
