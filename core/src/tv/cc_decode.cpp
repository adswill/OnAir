// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/tv/cc_decode.h"

namespace dect2 {

namespace {
void utf8(std::string& o, uint32_t c) { if (c < 0x80) o += (char)c; else if (c < 0x800) { o += (char)(0xC0 | c >> 6); o += (char)(0x80 | (c & 63)); } else { o += (char)(0xE0 | c >> 12); o += (char)(0x80 | ((c >> 6) & 63)); o += (char)(0x80 | (c & 63)); } }
uint32_t map608(uint8_t c) {
    switch (c) { case 0x2A: return 0xE1; case 0x5C: return 0xE9; case 0x5E: return 0xED; case 0x5F: return 0xF3; case 0x60: return 0xFA; case 0x7B: return 0xE7; case 0x7C: return 0xF7; case 0x7D: return 0xD1; case 0x7E: return 0xF1; case 0x7F: return 0x25A0; default: return c; }
}
uint32_t special608(uint8_t c) {
    static const uint16_t t[16] = {0xAE, 0xB0, 0xBD, 0xBF, 0x2122, 0xA2, 0xA3, 0x266A, 0xE0, 0x20, 0xE8, 0xE2, 0xEA, 0xEE, 0xF4, 0xFB};
    return t[(c - 0x30) & 15];
}
bool parityOk(uint8_t b) { int n = 0; for (int i = 0; i < 8; i++) n += (b >> i) & 1; return n & 1; }
}

void CaptionDecoder::emit(const std::string& text, double start, double end) {
    if (text.empty()) return;
    std::string t = text;
    while (!t.empty() && (t.back() == ' ' || t.back() == '\n')) t.pop_back();
    size_t h = 0; while (h < t.size() && t[h] == ' ') h++;
    if (h) t.erase(0, h);
    if (t.empty()) return;
    SubCue c; c.start = start; c.end = end; c.text = t; cues_.push_back(std::move(c));
}
void CaptionDecoder::flushRow(double t) {
    if (!cur_.empty() && rowStart_ >= 0) emit(cur_, rowStart_, t);
    cur_.clear(); rowStart_ = -1;
}
void CaptionDecoder::finish() {
    const double t = dispStart_ >= 0 ? dispStart_ + 3 : 0;
    if (dispStart_ >= 0 && !disp_.empty()) { emit(disp_, dispStart_, t); disp_.clear(); dispStart_ = -1; }
    if (!cur_.empty()) flushRow(rowStart_ + 3);
    if (!line708_.empty()) { emit(line708_, line708Start_, line708Start_ + 3); line708_.clear(); }
}

void CaptionDecoder::pair608(uint8_t b1, uint8_t b2, double t) {
    pairs_++;
    if (b1 == 0 && b2 == 0) return;
    if (b1 == last1_ && b2 == last2_ && b1 >= 0x10 && b1 <= 0x1F) { last1_ = last2_ = 0; return; }   // control codes are sent twice
    if (b1 >= 0x10 && b1 <= 0x1F) { last1_ = b1; last2_ = b2; } else { last1_ = last2_ = 0; }
    const int ch = (b1 & 0x08) ? 2 : 1;
    if (b1 >= 0x20) {
        std::string& to = mode_ == Pop ? nd_ : cur_;
        if (mode_ != Pop && cur_.empty()) rowStart_ = t;
        utf8(to, map608(b1)); if (b2 >= 0x20) utf8(to, map608(b2));
        return;
    }
    if (ch != 1) return;
    const uint8_t c1 = b1 & 0xF7;                   // the command in channel 1 / 3 terms
    if (c1 == 0x11 && b2 >= 0x30 && b2 <= 0x3F) { std::string& to = mode_ == Pop ? nd_ : cur_; if (mode_ != Pop && cur_.empty()) rowStart_ = t; utf8(to, special608(b2)); return; }
    if (c1 == 0x14) {
        switch (b2) {
        case 0x20: mode_ = Pop; break;
        case 0x25: case 0x26: case 0x27: flushRow(t); mode_ = Roll; break;
        case 0x29: flushRow(t); mode_ = Paint; break;
        case 0x2C: if (dispStart_ >= 0) { emit(disp_, dispStart_, t); disp_.clear(); dispStart_ = -1; } flushRow(t); break;    // erase displayed
        case 0x2D: if (mode_ != Pop) flushRow(t); break;                                                                   // carriage return
        case 0x2E: nd_.clear(); break;                                                                                     // erase non-displayed
        case 0x2F:                                                                                                         // end of caption
            if (dispStart_ >= 0) emit(disp_, dispStart_, t);
            disp_ = nd_; nd_.clear(); dispStart_ = t;
            if (mode_ == Roll) mode_ = Pop;
            break;
        case 0x21: { std::string& to = mode_ == Pop ? nd_ : cur_; if (!to.empty()) { while (!to.empty() && (((uint8_t)to.back() & 0xC0) == 0x80)) to.pop_back(); if (!to.empty()) to.pop_back(); } break; }
        default: break;
        }
        return;
    }
    // preamble address codes and tab offsets: a new position starts a new line in the buffer
    if (b1 >= 0x10 && b1 <= 0x17 && b2 >= 0x40 && b2 <= 0x7F) {
        std::string& to = mode_ == Pop ? nd_ : cur_;
        if (!to.empty() && to.back() != '\n') to += mode_ == Pop ? '\n' : ' ';
    }
}

void CaptionDecoder::text708(const std::string& s, double t) { if (line708_.empty()) line708Start_ = t; line708_ += s; text708_ = true; }

void CaptionDecoder::packet708(const std::vector<uint8_t>& p, double t) {
    pkts_++;
    size_t i = 1;
    while (i < p.size()) {
        int svc = p[i] >> 5; const int blk = p[i] & 31; i++;
        if (svc == 7 && blk > 0 && i < p.size()) { svc = p[i] & 63; i++; }
        if (svc == 0 && blk == 0) break;
        const size_t end = std::min(p.size(), i + (size_t)blk);
        if (svc != 1) { i = end; continue; }
        size_t q = i;
        while (q < end) {
            const uint8_t c = p[q++];
            if (c == 0x10) { if (q < end) { const uint8_t e = p[q++]; if (e >= 0x20 && e < 0x80 && e != 0x7F) utf8(line708_.empty() ? (line708Start_ = t, line708_) : line708_, 0x20); } }   // extended set: a space marks the unknown glyph
            else if (c == 0x18) q += 2;
            else if (c <= 0x1F) {
                if (c == 0x0D || c == 0x03 || c == 0x0C || c == 0x0E) { if (!line708_.empty()) { emit(line708_, line708Start_, t); line708_.clear(); } }
                else if (c == 0x08) { while (!line708_.empty() && (((uint8_t)line708_.back() & 0xC0) == 0x80)) line708_.pop_back(); if (!line708_.empty()) line708_.pop_back(); }
                else if (c >= 0x11 && c <= 0x17) q += 1;
                else if (c >= 0x19) q += 2;
            } else if (c >= 0x80 && c <= 0x9F) {
                static const int args[32] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 0, 0, 2, 3, 2, 0, 0, 0, 0, 4, 6, 6, 6, 6, 6, 6, 6, 6};
                q += (size_t)args[c - 0x80];
                if (c == 0x89 || c >= 0x98) { if (!line708_.empty()) { emit(line708_, line708Start_, t); line708_.clear(); } }
            } else if (c >= 0x20 && c <= 0x7F) { std::string s; utf8(s, c == 0x7F ? 0x266A : c); text708(s, t); }
            else if (c >= 0xA0) { std::string s; utf8(s, c); text708(s, t); }
        }
        i = end;
    }
}

void CaptionDecoder::feed(const uint8_t* d, int n, double t) {
    for (int i = 0; i < n; i++, d += 3) {
        if (!(d[0] & 4)) continue;
        const int type = d[0] & 3;
        if (type == 0) {
            if (parityOk(d[1]) && parityOk(d[2])) pair608(d[1] & 0x7F, d[2] & 0x7F, t);
        } else if (type >= 2) {
            if (type == 3) {
                const int code = d[1] & 63; pktNeed_ = code == 0 ? 127 : code * 2 - 1;
                pkt_.assign(1, d[1]);
                pkt_.push_back(d[2]);
            } else if (!pkt_.empty()) { pkt_.push_back(d[1]); pkt_.push_back(d[2]); }
            if (!pkt_.empty() && (int)pkt_.size() >= pktNeed_ + 1) { packet708(pkt_, t); pkt_.clear(); }
        }
    }
}

}  // namespace dect2
