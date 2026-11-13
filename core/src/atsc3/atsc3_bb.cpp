// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/atsc3/atsc3_bb.h"
#include <algorithm>
#include "dect2/common/bits.h"

namespace dect2 {
namespace atsc3 {

std::vector<uint8_t> makeBbHeader(const BbHeader& h) {
    std::vector<uint8_t> out;
    const int ptr = h.pointer;
    bool two = h.twoByteBase || h.ofi != 0 || ptr >= 128;
    if (!two) {
        out.push_back((uint8_t)(ptr & 0x7F));   // MODE = 0
        return out;
    }
    out.push_back((uint8_t)(0x80 | (ptr & 0x7F)));                                   // MODE = 1, pointer LSB
    out.push_back((uint8_t)(((ptr >> 7) & 0x3F) << 2 | (h.ofi & 3)));                // pointer MSB (6 bits), OFI (2 bits)
    if (h.ofi == 1) {
        out.push_back((uint8_t)((h.extType & 7) << 5 | (h.extLen & 31)));
    } else if (h.ofi == 2) {
        out.push_back((uint8_t)((h.extType & 7) << 5 | (h.extLen & 31)));           // EXT_TYPE, EXT_LEN LSB
        out.push_back((uint8_t)((h.extLen >> 5) & 0xFF));                            // EXT_LEN MSB
    } else if (h.ofi == 3) {
        return {};   // mixed extension mode is not generated
    }
    if (h.ofi == 1 || h.ofi == 2) {
        for (int i = 0; i < h.extLen; i++) {
            uint8_t b = 0;
            if (h.extType == 0 && h.counter >= 0) { if (i == 0) b = (uint8_t)(h.counter >> 8); else if (i == 1) b = (uint8_t)h.counter; }
            out.push_back(b);
        }
    }
    return out;
}

bool parseBbHeader(const uint8_t* d, int size, BbHeader& h) {
    h = BbHeader();
    if (size < 1) return false;
    if (!(d[0] & 0x80)) { h.pointer = d[0] & 0x7F; h.headerBytes = 1; return true; }
    if (size < 2) return false;
    h.twoByteBase = true;
    h.pointer = (d[0] & 0x7F) | ((d[1] >> 2) << 7);
    h.ofi = d[1] & 3;
    int pos = 2;
    if (h.ofi == 0) { h.headerBytes = 2; return true; }
    if (h.ofi == 1) {
        if (size < 3) return false;
        h.extType = d[2] >> 5; h.extLen = d[2] & 31; pos = 3;
    } else if (h.ofi == 2) {
        if (size < 4) return false;
        h.extType = d[2] >> 5; h.extLen = (d[2] & 31) | (d[3] << 5); pos = 4;
    } else {
        // mixed: NUM_EXT (3 bits), EXT_LEN (13 bits) cover all extensions
        if (size < 4) return false;
        h.extType = 8; h.extLen = (d[2] & 31) | (d[3] << 5); pos = 4;
    }
    if (pos + h.extLen > size) return false;
    if (h.ofi != 3 && h.extType == 0 && h.extLen >= 2) h.counter = (d[pos] << 8) | d[pos + 1];
    h.headerBytes = pos + h.extLen;
    return true;
}

std::vector<uint8_t> makeBbPacket(int bytes, const std::vector<uint8_t>& payload, int pointer, int counter, int* used) {
    BbHeader h;
    h.pointer = pointer < 0 ? 8191 : pointer;
    if (pointer < 0) h.twoByteBase = true;
    auto hdr = makeBbHeader(h);
    const int natural = (int)hdr.size();
    // The packet is always full: when the payload is short, the header grows by an extension field that carries the padding (A/322 5.2.2.2)
    int need = bytes - (int)payload.size();   // header length that would fill the packet exactly
    if (counter >= 0) {
        // a counter extension: base (2) + optional field (1) + extension (at least the 2 counter bytes)
        h.twoByteBase = true; h.ofi = 1; h.extType = 0; h.extLen = 2; h.counter = counter;
        if (need > 5) {
            if (need - 3 <= 31) h.extLen = need - 3;
            else { h.ofi = 2; h.extLen = need - 4; }
        }
        hdr = makeBbHeader(h);
    } else if (need >= 3 && need > natural) {
        h.twoByteBase = true;
        if (need - 3 <= 31) { h.ofi = 1; h.extLen = need - 3; }
        else { h.ofi = 2; h.extLen = need - 4; }
        h.extType = 7;
        hdr = makeBbHeader(h);
    } else if (need == 2 && natural == 1) {
        h.twoByteBase = true;   // a two byte base field fills the packet
        hdr = makeBbHeader(h);
    }
    int room = bytes - (int)hdr.size();
    int take = std::min<int>(room, (int)payload.size());
    std::vector<uint8_t> out(hdr);
    out.insert(out.end(), payload.begin(), payload.begin() + take);
    out.resize(bytes, 0);   // only if the header could not grow enough (header of one or two bytes in a nearly full packet)
    if (used) *used = take;
    return out;
}

std::vector<uint8_t> bbScrambleSequence(int n, int) {
    // A/322 Figure 5.6: a 16 stage register of the Galois kind, G(x) = 1 + X + X^3 + X^6 + X^7 + X^11 + X^12 + X^13 + X^16, loaded with 0xF180
    // (X1 .. X16 = 0000 0001 1000 1111). One shift per byte; the byte is D7 .. D0 = X14, X13, X12, X11, X7, X4, X3, X1, sent MSB first. This
    // reading gives the first bytes 0xC0 0x6D 0x3F of the standard's text and unscrambled a real L1-Basic from a DekTec modulator.
    std::vector<uint8_t> out((size_t)n);
    int x[17];   // x[1] .. x[16]
    for (int i = 1; i <= 16; i++) x[i] = (0x018F >> (16 - i)) & 1;
    static const int kD[8] = {14, 13, 12, 11, 7, 4, 3, 1};   // D7 .. D0
    for (int i = 0; i < n; i++) {
        out[(size_t)i] = (uint8_t)x[kD[i & 7]];
        if ((i & 7) != 7) continue;
        const int fb = x[16];
        for (int k = 16; k > 1; k--) x[k] = x[k - 1] ^ ((k == 2 || k == 4 || k == 7 || k == 8 || k == 12 || k == 13 || k == 14) ? fb : 0);
        x[1] = fb;
    }
    return out;
}

void bbScramble(std::vector<uint8_t>& bits, int variant) {
    auto s = bbScrambleSequence((int)bits.size(), variant);
    for (size_t i = 0; i < bits.size(); i++) bits[i] ^= s[i];
}

std::vector<uint8_t> bitsToBytes(const std::vector<uint8_t>& bits) {
    std::vector<uint8_t> b(bits.size() / 8);
    for (size_t i = 0; i < b.size(); i++) {
        int v = 0;
        for (int k = 0; k < 8; k++) v = (v << 1) | bits[i * 8 + k];
        b[i] = (uint8_t)v;
    }
    return b;
}

std::vector<uint8_t> bytesToBits(const std::vector<uint8_t>& bytes) {
    std::vector<uint8_t> b(bytes.size() * 8);
    dect2::bits::bytesToBitsMsb(bytes.data(), bytes.size(), b.data());
    return b;
}

} // namespace atsc3
} // namespace dect2
