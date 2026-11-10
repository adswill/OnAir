// Spektrum DSM2 / DSMX (Cypress CYRF6936 DSSS, 2.4 GHz). Receive only: nothing here can command a model.
//
// Sources (all GPL-3, re-implemented, not ported):
//   DIY-Multiprotocol-TX-Module, Multiprotocol/DSM.ino and DSM_cyrf6936.ino (Pascal Langer and contributors): the PN code table below (DSM_pncodes,
//   copied as data, in the order the chips are sent, LSB first), the code selection per hop channel (pn row, SOP column, the two data codes), the 16 byte
//   data packet (two id bytes, seven 16 bit channel words: index above 10 or 11 value bits, bit 15 on the second frame) and the DSMX channel list.
//   gr-dsmx-rc (L. Cardoso, GRCon 2016): how a 64 chip 8DR symbol carries one byte (bit 7 the code, bit 6 the polarity, bits 0..5 a cyclic shift),
//   that the SOP is the SOP code followed by its complement, and the packet body: a length byte, the 16 bytes, two CRC bytes.
//   CYRF6936 datasheet: 1 Mchip/s, 64 chip 8DR, CRC-16 polynomial 0x8005 with a programmable seed.
// The CRC over the air is not published anywhere we could find (gr-dsmx-rc did not match it either), so it is NOT checked: a packet is taken when
// all 19 symbols despread within the error limit and the channel words pass a strict format check. See docs/RCLINK-3.9.md.
#include "dect2/rclink/rc_proto.h"
#include <cstdio>

namespace dect2 {
namespace rclink {

namespace {
// DSM_pncodes: blocks of 8 codes for pn rows 1, 2, 3, 4, 0, then one more code (row 0 column 8)
const uint8_t kPn[41][8] = {
    {0x83, 0xF7, 0xA8, 0x2D, 0x7A, 0x44, 0x64, 0xD3},
    {0x3F, 0x2C, 0x4E, 0xAA, 0x71, 0x48, 0x7A, 0xC9},
    {0x17, 0xFF, 0x9E, 0x21, 0x36, 0x90, 0xC7, 0x82},
    {0xBC, 0x5D, 0x9A, 0x5B, 0xEE, 0x7F, 0x42, 0xEB},
    {0x24, 0xF5, 0xDD, 0xF8, 0x7A, 0x77, 0x74, 0xE7},
    {0x3D, 0x70, 0x7C, 0x94, 0xDC, 0x84, 0xAD, 0x95},
    {0x1E, 0x6A, 0xF0, 0x37, 0x52, 0x7B, 0x11, 0xD4},
    {0x62, 0xF5, 0x2B, 0xAA, 0xFC, 0x33, 0xBF, 0xAF},
    {0x40, 0x56, 0x32, 0xD9, 0x0F, 0xD9, 0x5D, 0x97},
    {0x8E, 0x4A, 0xD0, 0xA9, 0xA7, 0xFF, 0x20, 0xCA},
    {0x4C, 0x97, 0x9D, 0xBF, 0xB8, 0x3D, 0xB5, 0xBE},
    {0x0C, 0x5D, 0x24, 0x30, 0x9F, 0xCA, 0x6D, 0xBD},
    {0x50, 0x14, 0x33, 0xDE, 0xF1, 0x78, 0x95, 0xAD},
    {0x0C, 0x3C, 0xFA, 0xF9, 0xF0, 0xF2, 0x10, 0xC9},
    {0xF4, 0xDA, 0x06, 0xDB, 0xBF, 0x4E, 0x6F, 0xB3},
    {0x9E, 0x08, 0xD1, 0xAE, 0x59, 0x5E, 0xE8, 0xF0},
    {0xC0, 0x90, 0x8F, 0xBB, 0x7C, 0x8E, 0x2B, 0x8E},
    {0x80, 0x69, 0x26, 0x80, 0x08, 0xF8, 0x49, 0xE7},
    {0x7D, 0x2D, 0x49, 0x54, 0xD0, 0x80, 0x40, 0xC1},
    {0xB6, 0xF2, 0xE6, 0x1B, 0x80, 0x5A, 0x36, 0xB4},
    {0x42, 0xAE, 0x9C, 0x1C, 0xDA, 0x67, 0x05, 0xF6},
    {0x9B, 0x75, 0xF7, 0xE0, 0x14, 0x8D, 0xB5, 0x80},
    {0xBF, 0x54, 0x98, 0xB9, 0xB7, 0x30, 0x5A, 0x88},
    {0x35, 0xD1, 0xFC, 0x97, 0x23, 0xD4, 0xC9, 0x88},
    {0xE1, 0xD6, 0x31, 0x26, 0x5F, 0xBD, 0x40, 0x93},
    {0xDC, 0x68, 0x08, 0x99, 0x97, 0xAE, 0xAF, 0x8C},
    {0xC3, 0x0E, 0x01, 0x16, 0x0E, 0x32, 0x06, 0xBA},
    {0xE0, 0x83, 0x01, 0xFA, 0xAB, 0x3E, 0x8F, 0xAC},
    {0x5C, 0xD5, 0x9C, 0xB8, 0x46, 0x9C, 0x7D, 0x84},
    {0xF1, 0xC6, 0xFE, 0x5C, 0x9D, 0xA5, 0x4F, 0xB7},
    {0x58, 0xB5, 0xB3, 0xDD, 0x0E, 0x28, 0xF1, 0xB0},
    {0x5F, 0x30, 0x3B, 0x56, 0x96, 0x45, 0xF4, 0xA1},
    {0x03, 0xBC, 0x6E, 0x8A, 0xEF, 0xBD, 0xFE, 0xF8},
    {0x88, 0x17, 0x13, 0x3B, 0x2D, 0xBF, 0x06, 0xD6},
    {0xF1, 0x94, 0x30, 0x21, 0xA1, 0x1C, 0x88, 0xA9},
    {0xD0, 0xD2, 0x8E, 0xBC, 0x82, 0x2F, 0xE3, 0xB4},
    {0x8C, 0xFA, 0x47, 0x9B, 0x83, 0xA5, 0x66, 0xD0},
    {0x07, 0xBD, 0x9F, 0x26, 0xC8, 0x31, 0x0F, 0xB8},
    {0xEF, 0x03, 0x95, 0x89, 0xB4, 0x71, 0x61, 0x9D},
    {0x40, 0xBA, 0x97, 0xD5, 0x86, 0x4F, 0xCC, 0xD1},
    {0xD7, 0xA1, 0x54, 0xB1, 0x5E, 0x89, 0xAE, 0x86},
};
uint64_t code64(int idx) {   // bit j = chip j
    uint64_t c = 0;
    for (int b = 0; b < 8; b++) c |= (uint64_t)kPn[idx][b] << (8 * b);
    return c;
}
int blockOf(int row) { return row == 0 ? 4 : row - 1; }   // DSM_read_code: row--, row > 4 -> 4
uint64_t rotChips(uint64_t c, int s) { return s ? (c >> s) | (c << (64 - s)) : c; }   // chip j of the result = chip (j + s) of c
} // namespace

DsmCodes dsmCodes(int row, int sopCol) {
    DsmCodes k;
    const int b = blockOf(((row % 5) + 5) % 5) * 8;
    sopCol &= 7;
    k.sop = code64(b + sopCol);
    k.data0 = code64(b + 7 - sopCol);
    k.data1 = code64(b + 8 - sopCol);
    return k;
}

uint64_t dsmSymbol(const DsmCodes& k, uint8_t v) {
    uint64_t c = rotChips((v & 0x80) ? k.data1 : k.data0, v & 63);
    return (v & 0x40) ? c : ~c;
}

int dsmDespread(const DsmCodes& k, uint64_t chips, int* errors) {
    int best = 99, bv = -1;
    for (int code = 0; code < 2; code++) {
        const uint64_t base = code ? k.data1 : k.data0;
        for (int s = 0; s < 64; s++) {
            const int e = __builtin_popcountll(rotChips(base, s) ^ chips);
            const int ei = 64 - e;   // the complement
            if (e < best) { best = e; bv = code << 7 | 0x40 | s; }
            if (ei < best) { best = ei; bv = code << 7 | s; }
        }
    }
    if (errors) *errors = best;
    return bv;
}

uint16_t dsmCrc(const uint8_t* p, size_t n, uint16_t seed) {
    uint16_t c = seed;
    for (size_t i = 0; i < n; i++) {
        c ^= (uint16_t)p[i] << 8;
        for (int k = 0; k < 8; k++) c = (c & 0x8000) ? (uint16_t)((c << 1) ^ 0x8005) : (uint16_t)(c << 1);
    }
    return c;
}

int dsmSopColumn(const uint8_t id[4]) { return (id[0] + id[1] + id[2] + 2) & 7; }
int dsmPnRow(int rfChannel, bool dsmx) { return dsmx ? (rfChannel - 2) % 5 : rfChannel % 5; }

std::vector<int> dsmxChannels(const uint8_t id[4]) {
    std::vector<int> ch;
    uint32_t t = ~((uint32_t)id[0] << 24 | (uint32_t)id[1] << 16 | (uint32_t)id[2] << 8 | id[3]);
    while (ch.size() < 23) {
        t = t * 0x0019660Du + 0x3C6EF35Fu;
        const int next = (int)((t >> 8) % 0x49) + 3;
        if ((next ^ id[3]) & 1) continue;
        int lo = 0, mid = 0, hi = 0;
        bool dup = false;
        for (int c : ch) { if (c == next) { dup = true; break; } if (c <= 27) lo++; else if (c <= 51) mid++; else hi++; }
        if (dup) continue;
        if ((next < 28 && lo < 8) || (next >= 28 && next < 52 && mid < 7) || (next >= 52 && hi < 8)) ch.push_back(next);
    }
    return ch;
}

std::vector<uint8_t> dsmDataPacket(const uint8_t id[4], bool dsmx, bool upper, int bits, const int idx[7], const uint16_t val[7]) {
    std::vector<uint8_t> p(16);
    p[0] = dsmx ? id[2] : (uint8_t)~id[2];
    p[1] = dsmx ? id[3] : (uint8_t)~id[3];
    for (int i = 0; i < 7; i++) {
        uint16_t v = 0xFFFF;
        if (idx[i] >= 0) v = (uint16_t)((upper && i == 0 ? 0x8000 : 0) | idx[i] << bits | (val[i] & ((1 << bits) - 1)));
        p[2 + 2 * i] = (uint8_t)(v >> 8);
        p[3 + 2 * i] = (uint8_t)v;
    }
    return p;
}

std::vector<uint8_t> dsmChips(const DsmCodes& k, const std::vector<uint8_t>& packet, uint16_t crcSeed) {
    std::vector<uint8_t> c;
    auto put = [&](uint64_t s) { for (int j = 0; j < 64; j++) c.push_back((uint8_t)((s >> j) & 1)); };
    for (uint8_t b : {0x33, 0x33, 0x04}) for (int j = 0; j < 8; j++) c.push_back((uint8_t)((b >> j) & 1));   // CYRF_WritePreamble(0x333304)
    put(k.sop);
    put(~k.sop);
    std::vector<uint8_t> body = {(uint8_t)packet.size()};
    body.insert(body.end(), packet.begin(), packet.end());
    const uint16_t crc = dsmCrc(body.data(), body.size(), crcSeed);
    body.push_back((uint8_t)(crc >> 8));
    body.push_back((uint8_t)crc);
    for (uint8_t v : body) put(dsmSymbol(k, v));
    return c;
}

Decoded dsmParse(const uint8_t* p, size_t n, int rfChannel, int row) {
    Decoded d;
    d.proto = Dsm;
    if (n < 16) return d;
    int bits = 0, used = 0;
    uint16_t w[7];
    for (int i = 0; i < 7; i++) w[i] = (uint16_t)(p[2 + 2 * i] << 8 | p[3 + 2 * i]);
    // 11 bit (DSMX, DSM2 2048) or 10 bit (DSM2 1024) words: the channel indexes of one packet are all different, the unused words are FFFF,
    // only the first word may carry bit 15
    for (int b : {11, 10}) {
        bool ok = true;
        unsigned seen = 0;
        int u = 0;
        for (int i = 0; i < 7 && ok; i++) {
            if (w[i] == 0xFFFF) continue;
            const uint16_t x = i == 0 ? (uint16_t)(w[i] & 0x7FFF) : w[i];
            if (x & 0x8000) { ok = false; break; }
            const int ix = x >> b;
            if (ix >= (b == 11 ? 16 : 32) || (seen >> ix & 1)) ok = false;
            seen |= 1u << ix;
            u++;
        }
        if (ok && u >= 2) { bits = b; used = u; break; }
    }
    if (!bits) return d;
    d.ok = true;
    char t[220];
    std::snprintf(t, sizeof t, "%02X%02X", p[0], p[1]);
    d.id = t;
    const char* kind = "DSM";
    if (rfChannel >= 0 && row >= 0) kind = dsmPnRow(rfChannel, true) == row ? "DSMX" : dsmPnRow(rfChannel, false) == row ? "DSM2" : "DSM";
    d.type = std::string(kind) + " channels";
    std::string s;
    for (int i = 0; i < 7; i++) {
        if (w[i] == 0xFFFF) continue;
        const uint16_t x = (uint16_t)(w[i] & 0x7FFF);
        char c[32];
        std::snprintf(c, sizeof c, "%sch%d %u", s.empty() ? "" : " ", (x >> bits) + 1, x & ((1u << bits) - 1));
        s += c;
    }
    std::snprintf(t, sizeof t, "%s (%d bit, %d words%s); CRC not checked", s.c_str(), bits, used, (w[0] & 0x8000) ? ", second frame" : "");
    d.detail = t;
    return d;
}

} // namespace rclink
} // namespace dect2
