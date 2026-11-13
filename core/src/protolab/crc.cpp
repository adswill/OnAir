// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Protocol lab: CRC models, and the search for the CRC of an unknown protocol (catalog, then the difference method of CRC RevEng:
// for equal-length messages the init and xorout cancel in m1^m2, so the polynomial can be solved from the packets alone).
#include "dect2/protolab/protolab.h"
#include <algorithm>
#include <cstdio>
#include <map>

namespace dect2::protolab {

namespace {
uint32_t reflect(uint32_t v, int w) {
    uint32_t r = 0;
    for (int i = 0; i < w; i++) if (v & (1u << i)) r |= 1u << (w - 1 - i);
    return r;
}
uint32_t maskOf(int w) { return w >= 32 ? 0xFFFFFFFFu : ((1u << w) - 1); }
}   // namespace

uint32_t crcCompute(const CrcModel& m, const uint8_t* d, size_t n) {
    const int w = m.width;
    const uint32_t mask = maskOf(w);
    uint32_t reg;
    if (m.refin) {
        reg = reflect(m.init, w);
        const uint32_t rp = reflect(m.poly, w);
        for (size_t i = 0; i < n; i++) {
            reg ^= d[i];
            for (int b = 0; b < 8; b++) reg = (reg & 1) ? (reg >> 1) ^ rp : reg >> 1;
        }
    } else {
        reg = m.init & mask;
        const uint32_t top = 1u << (w - 1);
        for (size_t i = 0; i < n; i++) {
            reg ^= (uint32_t)d[i] << (w - 8);
            for (int b = 0; b < 8; b++) reg = (reg & top) ? ((reg << 1) ^ m.poly) & mask : (reg << 1) & mask;
        }
    }
    if (m.refin != m.refout) reg = reflect(reg, w);
    return (reg ^ m.xorout) & mask;
}

const std::vector<CrcModel>& crcCatalog() {
    static const std::vector<CrcModel> c = {
        // name, width, poly, init, refin, refout, xorout
        {"CRC-8", 8, 0x07, 0x00, false, false, 0x00},
        {"CRC-8/CDMA2000", 8, 0x9B, 0xFF, false, false, 0x00},
        {"CRC-8/DARC", 8, 0x39, 0x00, true, true, 0x00},
        {"CRC-8/DVB-S2", 8, 0xD5, 0x00, false, false, 0x00},
        {"CRC-8/EBU", 8, 0x1D, 0xFF, true, true, 0x00},
        {"CRC-8/I-CODE", 8, 0x1D, 0xFD, false, false, 0x00},
        {"CRC-8/ITU", 8, 0x07, 0x00, false, false, 0x55},
        {"CRC-8/MAXIM (Dallas 1-Wire)", 8, 0x31, 0x00, true, true, 0x00},
        {"CRC-8/ROHC", 8, 0x07, 0xFF, true, true, 0x00},
        {"CRC-8/WCDMA", 8, 0x9B, 0x00, true, true, 0x00},
        {"CRC-8/SAE-J1850", 8, 0x1D, 0xFF, false, false, 0xFF},
        {"CRC-8/AUTOSAR", 8, 0x2F, 0xFF, false, false, 0xFF},
        {"CRC-8/BLUETOOTH", 8, 0xA7, 0x00, true, true, 0x00},
        {"CRC-8/NRSC-5 (poly 0x31, init 0xFF)", 8, 0x31, 0xFF, false, false, 0x00},
        {"CRC-8 (poly 0x31, init 0)", 8, 0x31, 0x00, false, false, 0x00},
        {"CRC-16/ARC", 16, 0x8005, 0x0000, true, true, 0x0000},
        {"CRC-16/CCITT-FALSE", 16, 0x1021, 0xFFFF, false, false, 0x0000},
        {"CRC-16/XMODEM", 16, 0x1021, 0x0000, false, false, 0x0000},
        {"CRC-16/KERMIT (CCITT-TRUE)", 16, 0x1021, 0x0000, true, true, 0x0000},
        {"CRC-16/X-25 (HDLC, PPP FCS)", 16, 0x1021, 0xFFFF, true, true, 0xFFFF},
        {"CRC-16/GENIBUS", 16, 0x1021, 0xFFFF, false, false, 0xFFFF},
        {"CRC-16/AUG-CCITT", 16, 0x1021, 0x1D0F, false, false, 0x0000},
        {"CRC-16/MODBUS", 16, 0x8005, 0xFFFF, true, true, 0x0000},
        {"CRC-16/USB", 16, 0x8005, 0xFFFF, true, true, 0xFFFF},
        {"CRC-16/MAXIM", 16, 0x8005, 0x0000, true, true, 0xFFFF},
        {"CRC-16/DNP", 16, 0x3D65, 0x0000, true, true, 0xFFFF},
        {"CRC-16/EN-13757 (wM-Bus)", 16, 0x3D65, 0x0000, false, false, 0xFFFF},
        {"CRC-16/T10-DIF", 16, 0x8BB7, 0x0000, false, false, 0x0000},
        {"CRC-16/TELEDISK", 16, 0xA097, 0x0000, false, false, 0x0000},
        {"CRC-16/CDMA2000", 16, 0xC867, 0xFFFF, false, false, 0x0000},
        {"CRC-16/DECT-X", 16, 0x0589, 0x0000, false, false, 0x0000},
        {"CRC-16/DECT-R", 16, 0x0589, 0x0000, false, false, 0x0001},
        {"CRC-16/MCRF4XX", 16, 0x1021, 0xFFFF, true, true, 0x0000},
        {"CRC-16/RIELLO", 16, 0x1021, 0xB2AA, true, true, 0x0000},
        {"CRC-16/TMS37157", 16, 0x1021, 0x89EC, true, true, 0x0000},
        {"CRC-16/ISO-IEC-14443-3-A", 16, 0x1021, 0xC6C6, true, true, 0x0000},
        {"CRC-16/DDS-110", 16, 0x8005, 0x800D, false, false, 0x0000},
        {"CRC-16/BUYPASS", 16, 0x8005, 0x0000, false, false, 0x0000},
        {"CRC-16/CMS", 16, 0x8005, 0xFFFF, false, false, 0x0000},
        {"CRC-32/ISO-HDLC (Ethernet, zip)", 32, 0x04C11DB7, 0xFFFFFFFF, true, true, 0xFFFFFFFF},
        {"CRC-32/BZIP2", 32, 0x04C11DB7, 0xFFFFFFFF, false, false, 0xFFFFFFFF},
        {"CRC-32/MPEG-2", 32, 0x04C11DB7, 0xFFFFFFFF, false, false, 0x00000000},
        {"CRC-32C (Castagnoli)", 32, 0x1EDC6F41, 0xFFFFFFFF, true, true, 0xFFFFFFFF},
        {"CRC-32/JAMCRC", 32, 0x04C11DB7, 0xFFFFFFFF, true, true, 0x00000000},
        {"CRC-32/POSIX (cksum)", 32, 0x04C11DB7, 0x00000000, false, false, 0xFFFFFFFF},
        {"CRC-32/XFER", 32, 0x000000AF, 0x00000000, false, false, 0x00000000},
        {"CRC-32D", 32, 0xA833982B, 0xFFFFFFFF, true, true, 0xFFFFFFFF},
        {"CRC-32Q", 32, 0x814141AB, 0x00000000, false, false, 0x00000000},
    };
    return c;
}

namespace {
std::string nameOf(int w, uint32_t poly, uint32_t init, bool ri, bool ro, uint32_t xo) {
    for (const CrcModel& m : crcCatalog())
        if (m.width == w && m.poly == poly && m.init == init && m.refin == ri && m.refout == ro && m.xorout == xo) return m.name;
    char b[64];
    snprintf(b, sizeof b, "CRC-%d/unnamed", w);
    return b;
}

struct Geo { int w, d, s; bool be; };

bool readCrc(const std::vector<uint8_t>& p, const Geo& g, uint32_t& val, size_t& dataLen) {
    const size_t cb = (size_t)g.w / 8;
    if (p.size() < (size_t)g.d + cb + (size_t)g.s + 1) return false;
    const size_t at = p.size() - (size_t)g.s - cb;
    dataLen = at - (size_t)g.d;
    val = 0;
    for (size_t i = 0; i < cb; i++) {
        const uint32_t b = p[at + i];
        if (g.be) val = (val << 8) | b;
        else val |= b << (8 * i);
    }
    return true;
}

std::vector<int> startsFor(int maxStart, int hint) {
    std::vector<int> s;
    for (int d : {hint, hint + 1, hint + 2, hint + 3, hint + 4, 0, 1, 2}) if (d >= 0 && d <= maxStart && std::find(s.begin(), s.end(), d) == s.end()) s.push_back(d);
    for (int d = 0; d <= maxStart; d++) if (std::find(s.begin(), s.end(), d) == s.end()) s.push_back(d);
    return s;
}

// table for the raw CRC (init 0, xorout 0) of one polynomial and bit order
struct Tab { uint32_t t[256]; };
void buildTab(Tab& tb, int w, uint32_t poly, bool refin) {
    const uint32_t mask = maskOf(w);
    for (uint32_t b = 0; b < 256; b++) {
        uint32_t r;
        if (refin) {
            const uint32_t rp = reflect(poly, w);
            r = b;
            for (int i = 0; i < 8; i++) r = (r & 1) ? (r >> 1) ^ rp : r >> 1;
        } else {
            r = b << (w - 8);
            for (int i = 0; i < 8; i++) r = (r & (1u << (w - 1))) ? ((r << 1) ^ poly) & mask : (r << 1) & mask;
        }
        tb.t[b] = r;
    }
}
uint32_t rawCrc(const Tab& tb, int w, bool refin, bool refout, const uint8_t* d, size_t n) {
    uint32_t r = 0;
    if (refin) for (size_t i = 0; i < n; i++) r = (r >> 8) ^ tb.t[(r ^ d[i]) & 0xFF];
    else for (size_t i = 0; i < n; i++) r = ((r << 8) ^ tb.t[((r >> (w - 8)) ^ d[i]) & 0xFF]) & maskOf(w);
    if (refin != refout) r = reflect(r, w);
    return r;
}
}   // namespace

std::vector<CrcHit> searchCrc(const std::vector<std::vector<uint8_t>>& pk, int hint, const CrcOptions& o) {
    std::vector<CrcHit> hits;
    std::vector<const std::vector<uint8_t>*> usable;
    for (const auto& p : pk) {
        if (p.size() < 4) continue;
        bool dup = false;   // a repeated packet is one observation, not two
        for (const auto* q : usable) if (*q == p) { dup = true; break; }
        if (!dup) usable.push_back(&p);
    }
    if (usable.size() < 3) return hits;
    const int total = (int)usable.size();
    const int need = std::max(2, (total * 6 + 9) / 10);
    const std::vector<int> starts = startsFor(o.maxStart, hint);

    auto addHit = [&](CrcHit h) {
        for (CrcHit& e : hits)
            if (e.name == h.name && e.width == h.width && e.dataStart == h.dataStart && e.endSlack == h.endSlack && e.crcBigEndian == h.crcBigEndian) {
                if (h.matches > e.matches) e = h;
                return;
            }
        hits.push_back(h);
    };

    // phase 1: the catalog, with the common init / xorout variants of every polynomial (tables built once)
    struct Fast { CrcModel m; Tab t; uint32_t initReg; };
    static const std::vector<Fast> models = [] {
        std::vector<CrcModel> list;
        for (const CrcModel& m : crcCatalog()) {
            list.push_back(m);
            const uint32_t ones = maskOf(m.width);
            for (uint32_t init : {0u, ones})
                for (uint32_t xo : {0u, ones}) {
                    bool known = false;
                    for (const CrcModel& k : crcCatalog())
                        if (k.width == m.width && k.poly == m.poly && k.refin == m.refin && k.refout == m.refout && k.init == init && k.xorout == xo) { known = true; break; }
                    for (const CrcModel& k : list)
                        if (k.width == m.width && k.poly == m.poly && k.refin == m.refin && k.refout == m.refout && k.init == init && k.xorout == xo) { known = true; break; }
                    if (!known) { CrcModel v = m; v.init = init; v.xorout = xo; v.name = ""; list.push_back(v); }
                }
        }
        std::vector<Fast> f(list.size());
        for (size_t i = 0; i < list.size(); i++) {
            f[i].m = list[i];
            buildTab(f[i].t, list[i].width, list[i].poly, list[i].refin);
            f[i].initReg = list[i].refin ? reflect(list[i].init, list[i].width) : list[i].init;
        }
        return f;
    }();
    auto fastCrc = [](const Fast& f, const uint8_t* d, size_t n) {
        const int w = f.m.width;
        uint32_t reg = f.initReg;
        if (f.m.refin) for (size_t i = 0; i < n; i++) reg = (reg >> 8) ^ f.t.t[(reg ^ d[i]) & 0xFF];
        else for (size_t i = 0; i < n; i++) reg = ((reg << 8) ^ f.t.t[((reg >> (w - 8)) ^ d[i]) & 0xFF]) & maskOf(w);
        if (f.m.refin != f.m.refout) reg = reflect(reg, w);
        return (reg ^ f.m.xorout) & maskOf(w);
    };
    for (int w : {8, 16, 32}) {
        if (w < o.minWidth) continue;
        for (int s = 0; s <= o.maxSlack; s++)
            for (int d : starts)
                for (int be = 0; be < (w == 8 ? 1 : 2); be++) {
                    const Geo g{w, d, s, be == 0};
                    std::vector<uint32_t> val(usable.size());
                    std::vector<size_t> len(usable.size());
                    int ok = 0;
                    for (size_t i = 0; i < usable.size(); i++) ok += readCrc(*usable[i], g, val[i], len[i]);
                    if (ok < need) continue;
                    for (const Fast& fm : models) {
                        const CrcModel& m = fm.m;
                        if (m.width != w) continue;
                        int match = 0, seen = 0;
                        for (size_t i = 0; i < usable.size(); i++) {
                            if (!readCrc(*usable[i], g, val[i], len[i])) continue;
                            seen++;
                            const uint8_t* data = usable[i]->data() + d;
                            if (fastCrc(fm, data, len[i]) == val[i]) match++;
                            else if (seen - match > total - need) break;
                        }
                        if (match >= need) {
                            CrcHit h;
                            h.name = m.name[0] ? m.name : nameOf(w, m.poly, m.init, m.refin, m.refout, m.xorout);
                            if (!m.name[0] && h.name.find("unnamed") != std::string::npos) h.name += " (init/xorout variant)";
                            h.width = w; h.poly = m.poly; h.init = m.init; h.xorout = m.xorout; h.refin = m.refin; h.refout = m.refout;
                            h.crcBigEndian = g.be; h.dataStart = d; h.endSlack = s; h.matches = match; h.total = total;
                            h.coveredBytes = (int)len[0];
                            addHit(h);
                        }
                    }
                }

    }

    // phase 2: simple checksums
    for (int s = 0; s <= o.maxSlack; s++)
        for (int d : starts)
            for (int kind = 0; kind < 4; kind++) {
                const int w = kind == 3 ? 16 : 8;
                for (int be = 0; be < (w == 8 ? 1 : 2); be++) {
                    const Geo g{w, d, s, be == 0};
                    int match = 0;
                    for (const auto* p : usable) {
                        uint32_t val; size_t len;
                        if (!readCrc(*p, g, val, len)) continue;
                        uint32_t sum = 0;
                        for (size_t i = 0; i < len; i++) sum += (*p)[(size_t)d + i];
                        uint32_t x = 0;
                        for (size_t i = 0; i < len; i++) x ^= (*p)[(size_t)d + i];
                        const uint32_t want = kind == 0 ? (sum & 0xFF) : kind == 1 ? x : kind == 2 ? ((0u - sum) & 0xFF) : (sum & 0xFFFF);
                        if (want == val) match++;
                    }
                    if (match >= need) {
                        CrcHit h;
                        h.name = kind == 0 ? "sum of bytes (8 bit)" : kind == 1 ? "XOR of bytes" : kind == 2 ? "two's complement sum (8 bit)" : "sum of bytes (16 bit)";
                        h.width = w; h.crcBigEndian = g.be; h.dataStart = d; h.endSlack = s; h.matches = match; h.total = total; h.isChecksum = true;
                        { uint32_t vv; size_t ll = 0; if (readCrc(*usable[0], g, vv, ll)) h.coveredBytes = (int)ll; }
                        addHit(h);
                    }
                }
            }

    // phase 3: solve the polynomial from the differences of equal-length packets (8 and 16 bit)
    bool hasCrc = false;
    for (const CrcHit& h : hits) hasCrc |= !h.isChecksum && h.matches >= need && total >= 3;
    if (o.generic && !hasCrc && total >= o.minPackets) {
        static std::vector<Tab> tabs[2][2];   // [w==16][refin] indexed by poly >> 0 (filled lazily, odd polynomials only)
        for (int w : {8, 16}) {
            if (w < o.minWidth) continue;
            const uint32_t npoly = 1u << w;
            auto& t0 = tabs[w == 16][0];
            auto& t1 = tabs[w == 16][1];
            if (t0.empty()) {
                t0.resize(npoly / 2);
                t1.resize(npoly / 2);
                for (uint32_t i = 0; i < npoly / 2; i++) { buildTab(t0[i], w, 2 * i + 1, false); buildTab(t1[i], w, 2 * i + 1, true); }
            }
            for (int s = 0; s <= o.maxSlack; s++)
                for (int d : starts)
                    for (int be = 0; be < (w == 8 ? 1 : 2); be++) {
                        const Geo g{w, d, s, be == 0};
                        std::map<size_t, std::vector<size_t>> groups;   // covered length -> packets
                        std::vector<uint32_t> val(usable.size());
                        std::vector<size_t> len(usable.size());
                        for (size_t i = 0; i < usable.size(); i++) if (readCrc(*usable[i], g, val[i], len[i])) groups[len[i]].push_back(i);
                        const std::vector<size_t>* best = nullptr;
                        for (auto& kv : groups) if (!best || kv.second.size() > best->size()) best = &kv.second;
                        if (!best || (int)best->size() < o.minPackets) continue;
                        const size_t L = len[(*best)[0]];
                        const uint8_t* r = usable[(*best)[0]]->data() + d;
                        std::vector<std::vector<uint8_t>> diff;
                        std::vector<uint32_t> dval;
                        for (size_t k = 1; k < best->size(); k++) {
                            const uint8_t* q = usable[(*best)[k]]->data() + d;
                            std::vector<uint8_t> x(L);
                            bool nz = false;
                            for (size_t i = 0; i < L; i++) { x[i] = r[i] ^ q[i]; nz |= x[i] != 0; }
                            const uint32_t dv = val[(*best)[0]] ^ val[(*best)[k]];
                            if (!nz || dv == 0) continue;   // an identical message tells nothing about the polynomial
                            diff.push_back(x);
                            dval.push_back(dv);
                        }
                        if ((int)diff.size() + 1 < o.minPackets) continue;
                        for (int ri = 0; ri < 2; ri++)
                            for (int ro = 0; ro < 2; ro++) {
                                const auto& tabv = ri ? t1 : t0;
                                for (uint32_t i = 0; i < npoly / 2; i++) {
                                    bool ok = true;
                                    for (size_t k = 0; k < diff.size() && ok; k++) ok = rawCrc(tabv[i], w, ri, ro, diff[k].data(), L) == dval[k];
                                    if (!ok) continue;
                                    const uint32_t poly = 2 * i + 1;
                                    // init and xorout. With raw = the CRC for init 0 and xorout 0, a packet of L bytes gives K(L) = value ^ raw
                                    //  = crc(init, 0, L zero bytes) ^ xorout. One length: init and xorout cannot be told apart (the first
                                    //  "nice" pair is reported); two lengths or more fix them.
                                    CrcModel m{"", w, poly, 0, ri != 0, ro != 0, 0};
                                    const uint32_t ones = maskOf(w);
                                    std::map<size_t, uint32_t> K;
                                    bool consistent = true;
                                    for (size_t k = 0; k < usable.size() && consistent; k++) {
                                        if (!readCrc(*usable[k], g, val[k], len[k])) continue;
                                        const uint32_t kk = val[k] ^ crcCompute(m, usable[k]->data() + d, len[k]);
                                        auto it = K.find(len[k]);
                                        if (it == K.end()) K[len[k]] = kk;
                                        else if (it->second != kk) consistent = false;   // that length has two different constants: not this polynomial, or a damaged packet
                                    }
                                    if (!consistent || K.empty()) continue;
                                    auto zeroCrc = [&](uint32_t init, size_t L) {
                                        CrcModel z = m;
                                        z.init = init;
                                        std::vector<uint8_t> zeros(L, 0);
                                        return crcCompute(z, zeros.data(), L);
                                    };
                                    uint32_t fi = 0, fx = 0;
                                    bool found = false;
                                    const auto first = *K.begin();
                                    if (K.size() == 1) {
                                        for (uint32_t init : {0u, ones}) {
                                            const uint32_t xo = first.second ^ zeroCrc(init, first.first);
                                            if (xo == 0 || xo == ones) { fi = init; fx = xo; found = true; break; }
                                        }
                                        if (!found) { fi = 0; fx = first.second ^ zeroCrc(0, first.first); found = true; }
                                    } else {
                                        std::vector<uint32_t> order = {0u, ones};
                                        for (uint32_t init = 0; init <= ones; init++) { if (init != 0 && init != ones) order.push_back(init); if (init == ones) break; }
                                        for (size_t oi = 0; oi < order.size() && !found; oi++) {
                                            const uint32_t init = order[oi];
                                            const uint32_t xo = first.second ^ zeroCrc(init, first.first);
                                            bool all = true;
                                            for (auto& kv : K) if ((kv.second ^ zeroCrc(init, kv.first)) != xo) { all = false; break; }
                                            if (all) { found = true; fi = init; fx = xo; }
                                        }
                                    }
                                    if (!found) continue;
                                    m.init = fi; m.xorout = fx;
                                    CrcHit h;
                                    h.width = w; h.poly = poly; h.init = fi; h.xorout = fx; h.refin = ri; h.refout = ro;
                                    h.name = nameOf(w, poly, fi, ri, ro, fx);
                                    h.crcBigEndian = g.be; h.dataStart = d; h.endSlack = s;
                                    int match = 0;
                                    for (size_t k = 0; k < usable.size(); k++) {
                                        uint32_t v; size_t ln;
                                        if (readCrc(*usable[k], g, v, ln) && crcCompute(m, usable[k]->data() + d, ln) == v) match++;
                                    }
                                    h.matches = match; h.total = total;
                                    h.coveredBytes = (int)L;
                                    addHit(h);
                                }
                            }
                    }
        }
    }
    std::stable_sort(hits.begin(), hits.end(), [&](const CrcHit& a, const CrcHit& b) {
        if (a.matches != b.matches) return a.matches > b.matches;
        if (a.isChecksum != b.isChecksum) return !a.isChecksum;
        const bool ah = a.dataStart == hint, bh = b.dataStart == hint;
        if (ah != bh) return ah;
        if (a.endSlack != b.endSlack) return a.endSlack < b.endSlack;
        return a.width > b.width;
    });
    if (hits.size() > 12) hits.resize(12);
    return hits;
}

}   // namespace dect2::protolab
