// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// A small FLAC encoder for 16-bit mono or stereo (L/R, interleaved) audio (the recordings of the general receiver): fixed linear predictors of order 0 to 4 chosen per
// block, one Rice partition, a Vorbis comment block for tags. About half the size of WAV for speech, lossless. The MD5 field is left zero
// (not computed; decoders skip the check). Not thread safe: one writer per file.
#pragma once
#include "dect2/common/crc.h"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace dect2 {

class FlacEnc {
public:
    static constexpr int kBlock = 4096;

    // f: an open file (binary, writable). comments: "NAME=value" Vorbis comments (TITLE=..., DATE=...).
    bool begin(FILE* f, uint32_t rate, const std::vector<std::string>& comments = {}, int channels = 1) {
        ch_ = channels == 2 ? 2 : 1;
        f_ = f; rate_ = rate; frames_ = 0; total_ = 0; minFrame_ = 0xFFFFFF; maxFrame_ = 0; buf_.clear();
        if (!f_) return false;
        if (fwrite("fLaC", 1, 4, f_) != 4) return false;
        writeStreamInfo(comments.empty());   // the last metadata block when there are no comments
        if (!comments.empty() && !writeComments(comments)) return false;
        return true;
    }
    // n frames (n samples per channel); stereo input is interleaved
    void write(const int16_t* s, size_t n) {
        if (!f_) return;
        buf_.insert(buf_.end(), s, s + n * (size_t)ch_);
        size_t off = 0;
        const size_t blk = (size_t)kBlock * (size_t)ch_;
        while (buf_.size() - off >= blk) { encodeBlock(buf_.data() + off, kBlock); off += blk; }
        buf_.erase(buf_.begin(), buf_.begin() + (std::ptrdiff_t)off);
    }
    // flushes the last block and writes the final STREAMINFO (the file stays open for the caller to close)
    void finish() {
        if (!f_) return;
        if (!buf_.empty()) { encodeBlock(buf_.data(), buf_.size() / (size_t)ch_); buf_.clear(); }
        fflush(f_);
        fseek(f_, 8, SEEK_SET);   // after "fLaC" and the metadata block header
        writeStreamInfoBody();
        fseek(f_, 0, SEEK_END);
        f_ = nullptr;
    }
    uint64_t samples() const { return total_; }

private:
    FILE* f_ = nullptr;
    uint32_t rate_ = 48000;
    int ch_ = 1;
    uint64_t frames_ = 0, total_ = 0;
    uint32_t minFrame_ = 0, maxFrame_ = 0;
    std::vector<int16_t> buf_;

    struct Bits {
        std::vector<uint8_t> v; uint32_t acc = 0; int n = 0;
        void put(uint32_t value, int bits) { for (int i = bits - 1; i >= 0; i--) { acc = (acc << 1) | ((value >> i) & 1); if (++n == 8) { v.push_back((uint8_t)acc); acc = 0; n = 0; } } }
        void align() { while (n) put(0, 1); }
    };
    static uint8_t crc8(const uint8_t* d, size_t n) { uint8_t c = 0; for (size_t i = 0; i < n; i++) { c ^= d[i]; for (int k = 0; k < 8; k++) c = (uint8_t)((c & 0x80) ? (c << 1) ^ 0x07 : c << 1); } return c; }
    static uint16_t crc16(const uint8_t* d, size_t n) { return dect2::crc::crc16Buypass(d, n); }   // CRC-16/BUYPASS, the FLAC frame footer

    void writeStreamInfoBody() {
        uint8_t b[34] = {};
        const uint32_t minB = total_ < (uint64_t)kBlock && total_ > 0 ? (uint32_t)total_ : (uint32_t)kBlock, maxB = (uint32_t)kBlock;
        b[0] = (uint8_t)(minB >> 8); b[1] = (uint8_t)minB; b[2] = (uint8_t)(maxB >> 8); b[3] = (uint8_t)maxB;
        const uint32_t mf = minFrame_ == 0xFFFFFF ? 0 : minFrame_;
        b[4] = (uint8_t)(mf >> 16); b[5] = (uint8_t)(mf >> 8); b[6] = (uint8_t)mf;
        b[7] = (uint8_t)(maxFrame_ >> 16); b[8] = (uint8_t)(maxFrame_ >> 8); b[9] = (uint8_t)maxFrame_;
        // 20 bits rate, 3 bits channels-1, 5 bits bits-1, 36 bits total samples
        const uint64_t v = ((uint64_t)rate_ << 44) | ((uint64_t)(ch_ - 1) << 41) | ((uint64_t)15 << 36) | (total_ & 0xFFFFFFFFFULL);
        for (int i = 0; i < 8; i++) b[10 + i] = (uint8_t)(v >> (56 - 8 * i));
        fwrite(b, 1, 34, f_);
    }
    void writeStreamInfo(bool last) {
        const uint8_t h[4] = {(uint8_t)(last ? 0x80 : 0x00), 0, 0, 34};
        fwrite(h, 1, 4, f_);
        writeStreamInfoBody();
    }
    bool writeComments(const std::vector<std::string>& c) {
        std::vector<uint8_t> d;
        auto le32 = [&](uint32_t x) { for (int i = 0; i < 4; i++) d.push_back((uint8_t)(x >> (8 * i))); };
        const std::string vendor = "OnAir";
        le32((uint32_t)vendor.size()); d.insert(d.end(), vendor.begin(), vendor.end());
        le32((uint32_t)c.size());
        for (const auto& s : c) { le32((uint32_t)s.size()); d.insert(d.end(), s.begin(), s.end()); }
        // the STREAMINFO written just before was flagged "not last"; this one ends the metadata
        const uint8_t h[4] = {0x84, (uint8_t)(d.size() >> 16), (uint8_t)(d.size() >> 8), (uint8_t)d.size()};
        return fwrite(h, 1, 4, f_) == 4 && fwrite(d.data(), 1, d.size(), f_) == d.size();
    }

    static uint32_t zig(int32_t r) { return r >= 0 ? (uint32_t)r << 1 : (((uint32_t)(-(int64_t)r)) << 1) - 1; }

    void encodeSub(Bits& w, const int16_t* s, size_t n) {
        // choose the fixed order with the smallest residual
        int bestOrd = 0; uint64_t bestSum = ~0ULL;
        std::vector<int32_t> best;
        for (int ord = 0; ord <= 4 && (size_t)ord < n; ord++) {
            std::vector<int32_t> res(n - (size_t)ord);
            uint64_t sum = 0;
            for (size_t i = (size_t)ord; i < n; i++) {
                int64_t p = 0;
                const int64_t a = i >= 1 ? s[i - 1] : 0, b = i >= 2 ? s[i - 2] : 0, c = i >= 3 ? s[i - 3] : 0, d = i >= 4 ? s[i - 4] : 0;
                switch (ord) { case 0: p = 0; break; case 1: p = a; break; case 2: p = 2 * a - b; break; case 3: p = 3 * a - 3 * b + c; break; default: p = 4 * a - 6 * b + 4 * c - d; }
                const int32_t r = (int32_t)((int64_t)s[i] - p);
                res[i - (size_t)ord] = r; sum += (uint64_t)std::llabs((long long)r);
            }
            if (sum < bestSumGuard(bestSum)) { bestSum = sum; bestOrd = ord; best.swap(res); }
        }
        // Rice parameter with the fewest bits
        int bestK = 0; uint64_t bestBits = ~0ULL;
        for (int k = 0; k <= 14; k++) {
            uint64_t bits = 0;
            for (int32_t r : best) bits += (zig(r) >> k) + 1 + (uint64_t)k;
            if (bits < bestBits) { bestBits = bits; bestK = k; }
        }
        const bool verbatim = bestBits + 40 > (uint64_t)n * 16;
        // subframe
        w.put(0, 1);
        if (verbatim) {
            w.put(1, 6); w.put(0, 1);
            for (size_t i = 0; i < n; i++) w.put((uint16_t)s[i], 16);
        } else {
            w.put(8 + (uint32_t)bestOrd, 6); w.put(0, 1);
            for (int i = 0; i < bestOrd; i++) w.put((uint16_t)s[i], 16);
            w.put(0, 2); w.put(0, 4); w.put((uint32_t)bestK, 4);
            for (int32_t r : best) {
                const uint32_t u = zig(r), q = u >> bestK;
                for (uint32_t i = 0; i < q; i++) w.put(0, 1);
                w.put(1, 1);
                if (bestK) w.put(u & ((1u << bestK) - 1), bestK);
            }
        }
    }

    void encodeBlock(const int16_t* il, size_t n) {   // n frames
        std::vector<int16_t> lr[2];
        if (ch_ == 2) { lr[0].resize(n); lr[1].resize(n); for (size_t i = 0; i < n; i++) { lr[0][i] = il[2 * i]; lr[1][i] = il[2 * i + 1]; } }
        Bits w;
        // frame header
        w.put(0x3FFE, 14); w.put(0, 1); w.put(0, 1);                  // sync, reserved, fixed block size
        const bool full = n == (size_t)kBlock;
        w.put(full ? 12 : 7, 4);                                       // block size 4096, or 16-bit value after the header
        w.put(0, 4);                                                   // sample rate from STREAMINFO
        w.put(ch_ == 2 ? 1 : 0, 4);                                    // mono, or left / right
        w.put(4, 3); w.put(0, 1);                                      // 16 bits per sample, reserved
        { uint64_t fn = frames_;                                       // UTF-8 style frame number
          if (fn < 0x80) w.put((uint32_t)fn, 8);
          else if (fn < 0x800) { w.put(0xC0 | (uint32_t)(fn >> 6), 8); w.put(0x80 | (uint32_t)(fn & 0x3F), 8); }
          else if (fn < 0x10000) { w.put(0xE0 | (uint32_t)(fn >> 12), 8); w.put(0x80 | (uint32_t)((fn >> 6) & 0x3F), 8); w.put(0x80 | (uint32_t)(fn & 0x3F), 8); }
          else if (fn < 0x200000) { w.put(0xF0 | (uint32_t)(fn >> 18), 8); w.put(0x80 | (uint32_t)((fn >> 12) & 0x3F), 8); w.put(0x80 | (uint32_t)((fn >> 6) & 0x3F), 8); w.put(0x80 | (uint32_t)(fn & 0x3F), 8); }
          else { w.put(0xF8 | (uint32_t)(fn >> 24), 8); w.put(0x80 | (uint32_t)((fn >> 18) & 0x3F), 8); w.put(0x80 | (uint32_t)((fn >> 12) & 0x3F), 8); w.put(0x80 | (uint32_t)((fn >> 6) & 0x3F), 8); w.put(0x80 | (uint32_t)(fn & 0x3F), 8); } }
        if (!full) w.put((uint32_t)(n - 1), 16);
        w.put(crc8(w.v.data(), w.v.size()), 8);
        if (ch_ == 2) { encodeSub(w, lr[0].data(), n); encodeSub(w, lr[1].data(), n); }
        else encodeSub(w, il, n);
        w.align();
        const uint16_t c = crc16(w.v.data(), w.v.size());
        w.put(c, 16);
        fwrite(w.v.data(), 1, w.v.size(), f_);
        const uint32_t fs = (uint32_t)w.v.size();
        if (fs < minFrame_) minFrame_ = fs;
        if (fs > maxFrame_) maxFrame_ = fs;
        frames_++; total_ += n;
    }
    static uint64_t bestSumGuard(uint64_t s) { return s; }
};

}  // namespace dect2
