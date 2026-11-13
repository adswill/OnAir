// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/tv/bbunpack.h"
#include <cstring>
#include <algorithm>
#include <cstdint>
#include <vector>

namespace dect2 {

// CRC-8 (polynomial 0xD5, MSB first, initial 0) of the user packets, a byte at a time from a table: for an 8-bit CRC, shifting a byte in bit by
// bit is the same as XORing it into the register and shifting eight times, so the entry for crc ^ byte is the bit-serial result (the bit loop
// with its branch cost several microseconds per packet: a noticeable part of a T2 or S2 receiver's output).
namespace {
struct Crc8Table {
    uint8_t t[256];
    Crc8Table() {
        for (unsigned v = 0; v < 256; v++) {
            unsigned c = v;
            for (int b = 0; b < 8; b++) c = (c & 0x80) ? (((c << 1) & 0xff) ^ 0xD5) : ((c << 1) & 0xff);
            t[v] = (uint8_t)c;
        }
    }
};
const Crc8Table kCrc8;
}
static uint8_t crc8Byte(const uint8_t* p, int n) {
    unsigned crc = 0;
    for (int i = 0; i < n; i++) crc = kCrc8.t[crc ^ p[i]];
    return (uint8_t)crc;
}

void BbUnpacker::lost() {
    st_.framesLost++;
    gap_ = true;
    havePartial_ = false;
    partial_.clear();
    haveCrc_ = false;
}

void BbUnpacker::emit(const uint8_t* payload) {
    uint8_t pkt[188];
    pkt[0] = 0x47;
    memcpy(pkt + 1, payload, 187);
    st_.packets++;
    if (sink_) sink_(pkt);
}

void BbUnpacker::push(const std::vector<uint8_t>& bits, const BbHeader& h) {
    if (bits.size() < 80 || !h.crcOk) { lost(); return; }
    st_.frames++;
    st_.hem = h.hem; st_.issyi = h.issyi; st_.npd = h.npd; st_.tsGs = h.tsGs;
    st_.sisMis = h.sisMis; st_.isi = h.isi;
    // TS/GS=11 is a transport stream. SIS/MIS only says whether MATYPE-2 is reserved or an ISI:
    // a single-PLP multiplex (Dutch DVB-T2, issue 101) often still sets MIS with ISI 0.
    if (h.tsGs != 3) { st_.unsupported++; lost(); st_.framesLost--; return; }
    if (h.sisMis == 0) {
        if (!haveIsi_) { isi_ = h.isi; haveIsi_ = true; }
        else if (h.isi != isi_) { st_.otherIsi++; return; }
    }
    const int dfl = h.dfl;
    if (dfl <= 0 || 80 + dfl > (int)bits.size() || (dfl & 7)) { st_.unsupported++; return; }
    const int len = dfl / 8;
    std::vector<uint8_t> data(len);
    for (int i = 0; i < len; i++) {
        unsigned v = 0;
        for (int b = 0; b < 8; b++) v = (v << 1) | bits[80 + i * 8 + b];
        data[i] = (uint8_t)v;
    }
    // user-packet layout inside the data field
    // HEM: 187 bytes (sync removed). Normal mode: 188 bytes, the first one is the CRC-8 of the previous packet.
    int stride = h.hem ? 187 : 188;
    if (h.npd) stride += 1;                      // deleted-null-packet counter
    if (h.issyi && !h.hem) stride += 2;          // ISSY after every user packet (normal mode)
    const int sd = h.syncd == 0xFFFF ? -1 : h.syncd / 8;
    auto finish = [&](const uint8_t* pk) {   // (a pointer into the data field: no copy of every packet into a vector of its own)
        int o = h.hem ? 0 : 1;
        if (!h.hem && haveCrc_ && pk[0] != lastCrc_) st_.crcErrors++;
        emit(pk + o);
        lastCrc_ = crc8Byte(pk + o, 187);
        haveCrc_ = true;
        if (h.npd) for (int k = 0; k < pk[stride - 1]; k++) { uint8_t nul[187] = {0x1F, 0xFF, 0x10}; memset(nul + 3, 0xFF, 184); emit(nul); st_.nullInserted++; }
    };
    int pos = 0;
    const int head = sd < 0 ? len : std::min(sd, len);
    if (havePartial_ && !gap_) {
        partial_.insert(partial_.end(), data.begin(), data.begin() + head);
        if ((int)partial_.size() == stride) { finish(partial_.data()); partial_.clear(); havePartial_ = false; }
        else if ((int)partial_.size() < stride && sd < 0) { gap_ = false; return; } // still incomplete: wait for the next frame
        else { st_.resyncs++; partial_.clear(); havePartial_ = false; }
    } else {
        if (havePartial_) st_.resyncs++;
        partial_.clear();
        havePartial_ = false;
    }
    gap_ = false;
    if (sd < 0) return;
    pos = sd;
    while (pos + stride <= len) {
        finish(data.data() + pos);
        pos += stride;
    }
    if (pos < len) { partial_.assign(data.begin() + pos, data.end()); havePartial_ = true; }
}

} // namespace dect2
