// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/dvbc/docsis_down.h"
#include <cstdio>
#include <cstring>

namespace dect2 {

namespace {
constexpr int kPid = 0x1FFE;
uint16_t crcX25(const uint8_t* d, size_t n) {
    uint16_t c = 0xFFFF;
    for (size_t i = 0; i < n; i++) { c ^= d[i]; for (int k = 0; k < 8; k++) c = (c & 1) ? (uint16_t)((c >> 1) ^ 0x8408) : (uint16_t)(c >> 1); }
    return (uint16_t)~c;
}
}

const char* docsisMessageName(int t) {
    static const struct { int n; const char* s; } k[] = {{1, "SYNC"}, {2, "UCD"}, {3, "MAP"}, {4, "RNG-REQ"}, {5, "RNG-RSP"}, {6, "REG-REQ"}, {7, "REG-RSP"}, {12, "BPKM-REQ"}, {13, "BPKM-RSP"},
                                                         {14, "REG-ACK"}, {15, "DSA-REQ"}, {16, "DSA-RSP"}, {17, "DSA-ACK"}, {18, "DSC-REQ"}, {19, "DSC-RSP"}, {20, "DSC-ACK"}, {21, "DSD-REQ"}, {22, "DSD-RSP"},
                                                         {28, "UP-DIS"}, {29, "UCD (type 29)"}, {30, "INIT-RNG-REQ"}, {32, "DCD"}, {33, "MDD"}, {35, "UCD (type 35)"}, {49, "OCD"}, {50, "DPD"}, {51, "UCD (type 51)"}};
    for (auto& e : k) if (e.n == t) return e.s;
    return nullptr;
}

void DocsisDownstream::feed(const uint8_t* p, size_t count) {
    for (size_t i = 0; i < count; i++, p += 188) {
        const int pid = ((p[1] & 0x1F) << 8) | p[2];
        if (pid != kPid || (p[1] & 0x80)) continue;
        const int afc = (p[3] >> 4) & 3, cc = p[3] & 15;
        if (!(afc & 1)) continue;
        int off = 4;
        if (afc & 2) off += 1 + p[4];
        if (off >= 188) continue;
        if (cc_ >= 0 && cc != ((cc_ + 1) & 15)) { st_.lost++; buf_.clear(); synced_ = false; }
        cc_ = cc;
        const bool pusi = p[1] & 0x40;
        if (pusi) {
            const int ptr = p[off]; off++;
            if (off + ptr > 188) { buf_.clear(); synced_ = false; continue; }
            if (synced_) buf_.insert(buf_.end(), p + off, p + off + ptr);
            parseFrames();
            buf_.clear(); synced_ = true;
            buf_.insert(buf_.end(), p + off + ptr, p + 188);
        } else if (synced_) buf_.insert(buf_.end(), p + off, p + 188);
        parseFrames();
        st_.present = st_.framesTotal > 0;
    }
}

void DocsisDownstream::parseFrames() {
    size_t pos = 0;
    while (buf_.size() >= pos + 6) {
        const uint8_t fc = buf_[pos];
        if (fc == 0xFF) { pos++; continue; }                     // stuffing
        const bool ehdr = fc & 1;
        const int fcType = fc >> 6, fcParm = (fc >> 1) & 0x1F;
        const size_t len = ((size_t)buf_[pos + 2] << 8) | buf_[pos + 3];
        const size_t ehl = ehdr ? buf_[pos + 1] : 0;
        const size_t hdr = 4 + ehl + 2;
        size_t total = 4 + 2 + len;
        if (fcType == 2) { /* reserved */ }
        if (buf_.size() < pos + hdr) break;
        // HCS over FC, MAC_PARM, LEN and the extended header (CRC-16 of X.25)
        const uint16_t hcs = (uint16_t)buf_[pos + hdr - 2] << 8 | buf_[pos + hdr - 1];
        if (crcX25(buf_.data() + pos, hdr - 2) != hcs) { st_.hcsBad++; pos++; continue; }
        if (buf_.size() < pos + total) break;
        st_.framesTotal++;
        const uint8_t* body = buf_.data() + pos + hdr;
        const size_t blen = total - hdr;
        if (fcType == 3 && !ehdr) {
            if (fcParm == 0) {                                   // timing: management frame with SYNC
                st_.timingSync++;
            }
            if (fcParm <= 1) { management(body, blen); st_.framesMgmt++; }
        } else {
            st_.framesData++;                                    // subscriber or packet data: counted only
        }
        pos += total;
    }
    buf_.erase(buf_.begin(), buf_.begin() + (ptrdiff_t)pos);
}

void DocsisDownstream::management(const uint8_t* m, size_t n) {
    if (n < 20) return;                                          // DA, SA, length, DSAP, SSAP, control, version, type, reserved
    const int ver = m[17], type = m[18];
    st_.macVersionMax = std::max(st_.macVersionMax, ver);
    st_.mgmtByType[type]++;
    { char b[20]; snprintf(b, sizeof b, "%02x:%02x:%02x:%02x:%02x:%02x", m[6], m[7], m[8], m[9], m[10], m[11]); st_.lastMac = b; }
    const uint8_t* b = m + 20; const size_t bn = n - 20;
    if (type == 1 && bn >= 4) st_.lastTimestamp = (uint32_t)b[0] << 24 | b[1] << 16 | b[2] << 8 | b[3];
    if ((type == 2 || type == 29 || type == 35 || type == 51) && bn >= 4) {
        DocsisUpstream u; u.channelId = b[0]; u.changeCount = b[1]; u.miniSlotTicks = b[2]; u.dsChannelId = b[3]; u.ucdType = type;
        auto it = st_.upstream.find(u.channelId);
        if (it != st_.upstream.end()) { u.maps = it->second.maps; u.seen = it->second.seen; }
        size_t p = 4;
        while (p + 2 <= bn) {
            const int t = b[p], l = b[p + 1];
            if (p + 2 + (size_t)l > bn) break;
            const uint8_t* v = b + p + 2;
            if (t == 1 && l == 1) u.symbolRateMsym = v[0] * 0.16;
            if (t == 2 && l == 4) u.frequencyMhz = ((uint32_t)v[0] << 24 | v[1] << 16 | v[2] << 8 | v[3]) / 1e6;
            if (t == 4 || t == 5 || t == 23) u.bursts++;
            p += 2 + (size_t)l;
        }
        u.seen++;
        st_.upstream[u.channelId] = u;
    }
    if (type == 3 && bn >= 1) { auto it = st_.upstream.find(b[0]); if (it != st_.upstream.end()) it->second.maps++; }
    if (type == 33 && bn >= 4) {
        size_t p = 4;
        while (p + 2 <= bn) {
            const int t = b[p], l = b[p + 1];
            if (p + 2 + (size_t)l > bn) break;
            const uint8_t* v = b + p + 2;
            if (t == 1) {   // Downstream Active Channel List: one entry, sub-TLVs
                DocsisDownstreamChannel c; size_t q = 0;
                while (q + 2 <= (size_t)l) {
                    const int st = v[q], sl = v[q + 1];
                    if (q + 2 + (size_t)sl > (size_t)l) break;
                    const uint8_t* w = v + q + 2;
                    if (st == 1 && sl >= 1) c.channelId = w[0];
                    if (st == 2 && sl == 4) c.frequencyMhz = ((uint32_t)w[0] << 24 | w[1] << 16 | w[2] << 8 | w[3]) / 1e6;
                    if (st == 3 && sl >= 1) { c.modulation = w[0] & 15; c.annex = w[0] >> 4; }
                    if (st == 4 && sl >= 1) c.primaryCapable = w[0] != 0;
                    q += 2 + (size_t)sl;
                }
                bool found = false;
                for (auto& e : st_.downstream) if (e.channelId == c.channelId) { e = c; found = true; }
                if (!found) st_.downstream.push_back(c);
            }
            p += 2 + (size_t)l;
        }
    }
}

} // namespace dect2
