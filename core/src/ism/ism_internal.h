// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Private to the ISM sensor decoders: the bit buffer, the pulse-slicer types and the prototypes of the slicers and device decoders
// shared by ism_proto.cpp, ism_slice.cpp and ism_dec_*.cpp. Not part of the API. Written from rtl_433 (GPL-2.0), see ism_proto.cpp.
#pragma once
#include "dect2/ism/ism_proto.h"
#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>

namespace dect2 {

namespace ism_detail {

inline int addBytes(const uint8_t* m, unsigned n) { int r = 0; for (unsigned i = 0; i < n; i++) r += m[i]; return r; }
inline uint8_t xorBytes(const uint8_t* m, unsigned n) { uint8_t r = 0; for (unsigned i = 0; i < n; i++) r ^= m[i]; return r; }
inline int parity8(uint8_t b) { b ^= b >> 4; b ^= b >> 2; b ^= b >> 1; return b & 1; }
inline int parityBytes(const uint8_t* m, unsigned n) { int r = 0; for (unsigned i = 0; i < n; i++) r ^= parity8(m[i]); return r; }
inline uint8_t reflect4(uint8_t x) {
    x = (uint8_t)(((x & 0xCC) >> 2) | ((x & 0x33) << 2));
    x = (uint8_t)(((x & 0xAA) >> 1) | ((x & 0x55) << 1));
    return x;
}
inline std::string hexs(unsigned v, int digits) { char b[16]; snprintf(b, sizeof b, "%0*x", digits, v); return b; }
inline std::string hexU(unsigned v, int digits) { char b[16]; snprintf(b, sizeof b, "%0*X", digits, v); return b; }
inline std::string decs(long v) { char b[24]; snprintf(b, sizeof b, "%ld", v); return b; }

// ---------------------------------------------------------------- bitbuffer.c

constexpr int kMaxRows = 50;
constexpr unsigned kMaxBits = 4096;

struct Bits {
    std::vector<std::vector<uint8_t>> bb;
    std::vector<unsigned> n;
    Bits() { clear(); }
    int rows() const { return (int)bb.size(); }
    void clear() { bb.assign(1, {}); n.assign(1, 0); }
    bool hasData() const { return n[0] > 0 || rows() > 1; }
    void addBit(int b) {
        unsigned& k = n.back();
        if (k >= kMaxBits) return;
        auto& r = bb.back();
        if ((k >> 3) >= r.size()) r.push_back(0);
        if (b) r[k >> 3] |= (uint8_t)(0x80 >> (k & 7));
        k++;
    }
    void addRow() {                                   // as bitbuffer_add_row: empty rows count
        if (rows() < kMaxRows) { bb.emplace_back(); n.push_back(0); }
        else { bb.back().clear(); n.back() = 0; }
    }
    void addSync() { if (n.back()) addRow(); }
    int bit(int row, unsigned pos) const {
        if (row < 0 || row >= rows() || pos >= n[row]) return 0;
        return (bb[row][pos >> 3] >> (7 - (pos & 7))) & 1;
    }
    uint8_t byteAt(int row, unsigned i) const { return row < rows() && i < bb[row].size() ? bb[row][i] : 0; }
    const uint8_t* row(int r) { bb[r].resize(std::max<size_t>(bb[r].size(), 64), 0); return bb[r].data(); }   // padded with zeros, as rtl_433's fixed rows
    void invert() {
        for (int r = 0; r < rows(); r++) {
            if (!n[r]) continue;
            const unsigned lastCol = (n[r] - 1) / 8, lastBits = (n[r] - 1) % 8 + 1;
            for (unsigned c = 0; c <= lastCol && c < bb[r].size(); c++) bb[r][c] = (uint8_t)~bb[r][c];
            if (lastCol < bb[r].size()) bb[r][lastCol] ^= (uint8_t)(0xFF >> lastBits);
        }
    }
    // bitbuffer_search: position of the pattern in the row, or the row length when absent
    unsigned search(int r, unsigned start, const uint8_t* pat, unsigned plen) const {
        const unsigned len = n[r];
        unsigned ipos = start, ppos = 0;
        while (ipos < len && ppos < plen) {
            if (bit(r, ipos) == ((pat[ppos >> 3] >> (7 - (ppos & 7))) & 1)) {
                ppos++; ipos++;
                if (ppos == plen) return ipos - plen;
            } else { ipos -= ppos; ipos++; ppos = 0; }
        }
        return len;
    }
    void extract(int r, unsigned pos, uint8_t* out, unsigned nbits) const {
        for (unsigned i = 0; i < (nbits + 7) / 8; i++) out[i] = 0;
        for (unsigned i = 0; i < nbits; i++) if (bit(r, pos + i)) out[i >> 3] |= (uint8_t)(0x80 >> (i & 7));
    }
    int repeats(int r) const {
        int c = 0;
        for (int j = 0; j < rows(); j++) {
            if (n[j] != n[r]) continue;
            bool same = true;
            for (unsigned i = 0; i < (n[r] + 7) / 8 && same; i++) same = byteAt(j, i) == byteAt(r, i);
            c += same;
        }
        return c;
    }
    int findRepeatedRow(int minRepeats, unsigned minBits) const {
        for (int i = 0; i < rows(); i++) if (n[i] >= minBits && repeats(i) >= minRepeats) return i;
        return -1;
    }
};

// bitbuffer_manchester_decode: pairs 01 -> 1, 10 -> 0 (the second bit), stops at the first pair without a transition
inline unsigned manchester(const Bits& in, int row, unsigned start, Bits& out, unsigned max) {
    unsigned len = in.n[row], ipos = start;
    if (max && len > start + max * 2) len = start + max * 2;
    while (ipos < len) {
        const int b1 = in.bit(row, ipos++), b2 = in.bit(row, ipos++);
        if (b1 == b2) break;
        out.addBit(b2);
    }
    return ipos;
}

// bitbuffer_differential_manchester_decode
inline unsigned diffManchester(const Bits& in, int row, unsigned start, Bits& out, unsigned max) {
    unsigned len = in.n[row], ipos = start;
    int b1, b2 = 0;
    if (max && len > start + max * 2) len = start + max * 2;
    while (ipos < len) {
        b1 = in.bit(row, ipos++); b2 = in.bit(row, ipos++);
        const int b3 = in.bit(row, ipos);
        if (b1 != b2) {
            if (b2 != b3) out.addBit(0);
            else { b2 = b1; ipos -= 1; break; }
        } else { b2 = 1 - b1; ipos -= 2; break; }
    }
    while (ipos < len) {
        b1 = in.bit(row, ipos++);
        if (b1 == b2) break;
        b2 = in.bit(row, ipos++);
        out.addBit(b1 == b2 ? 1 : 0);
    }
    return ipos;
}

// ---------------------------------------------------------------- pulse_slicer.c


extern thread_local float tlPcmBitUs;   // the bit width slicePcm measured for the rows it hands out
enum Mod { OokPwm, OokPpm, OokPcm, OokMcZero, FskPcm, FskPwm };
struct Timing { float s = 0, l = 0, reset = 0, gap = 0, sync = 0, tol = 0; };
using Emit = std::function<void(Bits&)>;


// the slicers (ism_slice.cpp)
void slicePcm(const IsmTrain& t, const Timing& d, const Emit& emit);
void slicePpm(const IsmTrain& t, const Timing& d, const Emit& emit);
void slicePwm(const IsmTrain& t, const Timing& d, const Emit& emit);
void sliceMcZero(const IsmTrain& t, const Timing& d, const Emit& emit);

using Out = std::vector<IsmReading>;

inline IsmReading mk(const char* model, const char* kind, const std::string& id, const char* mic) {
    IsmReading r; r.model = model; r.kind = kind; r.id = id; r.mic = mic; return r;
}

// the device decoders (ism_dec_*.cpp)
void decNexus(Bits& B, Out& out);
void decRubicson(Bits& B, Out& out);
void decGenericTemp(Bits& B, Out& out);
void decTx141(Bits& B, Out& out);
bool acuriteCheck(const uint8_t* bb, int browlen, int explen);
void decAcurite(Bits& B, Out& out);
void decWh1080(Bits& B, Out& out);
void decF007th(Bits& B, Out& out);
double osTemp(const uint8_t* m);
int osHum(const uint8_t* m);
bool osChecksum(const uint8_t* m, int idx);
void decOregon(Bits& B, Out& out);
void decSchrader(Bits& B, Out& out);
void decSchraderEg53(Bits& B, Out& out);
void decGenericRemote(Bits& B, Out& out, std::vector<uint32_t>& seen);
int bcd(int x);
void decBresser5in1(Bits& B, Out& out);
void decBresser6in1(Bits& B, Out& out);
void decBresser7in1(Bits& B, Out& out);
void decTx29(Bits& B, Out& out, bool tx35);
void decWh24(Bits& B, Out& out);
void decToyota(Bits& B, Out& out);
void decPmv107j(Bits& B, Out& out);
void decCitroen(Bits& B, Out& out);
void decRenault(Bits& B, Out& out);
void decFord(Bits& B, Out& out);
void decHyundai(Bits& B, Out& out);
void decAbarth(Bits& B, Out& out);
void decEfergy(Bits& B, Out& out);
void decCurrentCost(Bits& B, Out& out);
uint8_t dec3of6(uint8_t v);
bool mbusCrc(const uint8_t* b, unsigned off);
const char* mbusType(uint8_t t);
void decMbus(Bits& B, Out& out);
void decKnxRf(Bits& B, Out& out);
void decHomeMatic(Bits& B, Out& out);
void decEnOcean(Bits& B, Out& out);
void decDash7(Bits& B, Out& out, bool lo);
const char* zwClassName(uint8_t c);
std::string zwCommand(uint8_t c, uint8_t k);
double zwValue(const uint8_t* p, int size, bool& ok);
void zwField(IsmReading& R, const char* name, int value, const std::string& text);
void zwPayload(IsmReading& R, const uint8_t* p, unsigned n);
void decZwave(Bits& B, Out& out, int rate);
void decAcurite606(Bits& B, Out& out);
void decAcurite609(Bits& B, Out& out);
void decPrologue(Bits& B, Out& out);
void decKedsum(Bits& B, Out& out);
void decGtWt02(Bits& B, Out& out);
void decBresser3ch(Bits& B, Out& out);
void decWh2(Bits& B, Out& out);
void decTp11(Bits& B, Out& out);
void decTp12(Bits& B, Out& out);
void decFt004b(Bits& B, Out& out);
void decErtScm(Bits& B, Out& out);
void decErtIdm(Bits& B, Out& out);
void decSolightTe44(Bits& B, Out& out);
void decHoneywell(Bits& B, Out& out);
void decKerui(Bits& B, Out& out);
void decSpringfield(Bits& B, Out& out);
// 3.9 (ism_dec_39.cpp)
void decX10Rf(Bits& B, Out& out);
void decInsteon(Bits& B, Out& out);
void decInfactory(Bits& B, Out& out);
void decWh31e(Bits& B, Out& out);
void decNissan(Bits& B, Out& out);
void decGmTpms(Bits& B, Out& out);
void decEfergyOptical(Bits& B, Out& out);
void decEmt7110(Bits& B, Out& out);
void decScmPlus(Bits& B, Out& out);

} // namespace ism_detail
using namespace ism_detail;

} // namespace dect2
