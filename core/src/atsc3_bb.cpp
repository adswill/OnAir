#include "dect2/atsc3_bb.h"
#include <algorithm>

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

std::vector<uint8_t> bbScrambleSequence(int n, int variant) {
    std::vector<uint8_t> out(n);
    // x^16 + x^13 + x^12 + x^11 + x^7 + x^6 + x^3 + x + 1, initial state 0xF180
    int r[16];
    for (int i = 0; i < 16; i++) r[i] = (0xF180 >> (15 - i)) & 1;
    for (int i = 0; i < n; i++) {
        out[i] = variant == 0 ? r[15] : r[0];
        int fb = r[15] ^ r[12] ^ r[11] ^ r[10] ^ r[6] ^ r[5] ^ r[2] ^ r[0];
        for (int j = 15; j > 0; j--) r[j] = r[j - 1];
        r[0] = fb;
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
    for (size_t i = 0; i < bytes.size(); i++)
        for (int k = 0; k < 8; k++) b[i * 8 + k] = (bytes[i] >> (7 - k)) & 1;
    return b;
}

} // namespace atsc3
} // namespace dect2
