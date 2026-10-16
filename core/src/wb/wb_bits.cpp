#include "dect2/wb_bits.h"
#include "dect2/u8file.h"
#include <algorithm>
#include <cctype>
#include <cstdio>

namespace dect2 {
namespace wb {

Bits applyBits(const Bits& in, const BitOpts& o, size_t* errors) {
    Bits b;
    if (errors) *errors = 0;
    b.reserve(in.size());
    for (size_t i = o.skip; i < in.size(); i++) b.push_back(in[i] & 1);
    if (o.invert) for (auto& v : b) v ^= 1;
    if (o.manchester) {
        Bits m;
        m.reserve(b.size() / 2);
        for (size_t i = (size_t)(o.manchesterPhase & 1); i + 1 < b.size(); i += 2) {
            const uint8_t a = b[i], c = b[i + 1];
            if (a == c) { if (errors) (*errors)++; continue; }
            const uint8_t rising = a == 0 && c == 1;
            m.push_back(o.manchester == 1 ? rising : (uint8_t)!rising);
        }
        b.swap(m);
    }
    if (o.nrzi) {
        uint8_t prev = 0;
        for (size_t i = 0; i < b.size(); i++) { const uint8_t cur = b[i]; b[i] = (cur == prev) ? 1 : 0; prev = cur; }
    }
    if (o.diffDecode) {
        uint8_t prev = 0;
        for (size_t i = 0; i < b.size(); i++) { const uint8_t cur = b[i]; b[i] = cur ^ prev; prev = cur; }
    }
    if (o.bitReverse) {
        for (size_t i = 0; i + 8 <= b.size(); i += 8)
            for (size_t k = 0; k < 4; k++) std::swap(b[i + k], b[i + 7 - k]);
    }
    return b;
}

std::vector<uint8_t> packBits(const Bits& b, bool lsbFirst) {
    std::vector<uint8_t> o(b.size() / 8);
    for (size_t i = 0; i < o.size(); i++) {
        uint8_t v = 0;
        for (int k = 0; k < 8; k++) {
            const uint8_t bit = b[i * 8 + (size_t)k] & 1;
            v = lsbFirst ? (uint8_t)(v | bit << k) : (uint8_t)(v << 1 | bit);
        }
        o[i] = v;
    }
    return o;
}

std::string formatBits(const Bits& b, int view, int bpl, bool lsbFirst, size_t maxBytes, size_t firstByte) {
    const std::vector<uint8_t> by = packBits(b, lsbFirst);
    std::string s;
    char t[32];
    const size_t from = std::min(firstByte, by.size());
    const size_t n = std::min(by.size(), from + maxBytes);
    bpl = std::max(1, bpl);
    for (size_t i = from; i < n; i += (size_t)bpl) {
        snprintf(t, sizeof t, "%08zx  ", i * 8);
        s += t;
        for (size_t k = i; k < std::min(n, i + (size_t)bpl); k++) {
            if (view == ViewHex) { snprintf(t, sizeof t, "%02X ", by[k]); s += t; }
            else if (view == ViewBinary) { for (int j = 7; j >= 0; j--) s += (char)('0' + ((by[k] >> j) & 1)); s += ' '; }
            else s += (by[k] >= 32 && by[k] < 127) ? (char)by[k] : '.';
        }
        s += '\n';
    }
    if (by.size() > n) s += "...\n";
    return s;
}

bool parsePattern(const std::string& text, Bits& out) {
    out.clear();
    std::string t;
    for (char c : text) if (c != ' ' && c != '_' && c != '\t') t += c;
    if (t.empty()) return false;
    bool hex = false;
    if (t.size() > 2 && t[0] == '0' && (t[1] == 'x' || t[1] == 'X')) { hex = true; t = t.substr(2); }
    else if (t.size() > 2 && t[0] == '0' && (t[1] == 'b' || t[1] == 'B')) t = t.substr(2);
    else {
        bool bin = true;
        for (char c : t) if (c != '0' && c != '1') bin = false;
        if (!bin) hex = true;
    }
    if (hex) {
        for (char c : t) {
            if (!std::isxdigit((unsigned char)c)) return false;
            const int v = std::isdigit((unsigned char)c) ? c - '0' : std::tolower(c) - 'a' + 10;
            for (int k = 3; k >= 0; k--) out.push_back((uint8_t)((v >> k) & 1));
        }
    } else for (char c : t) { if (c != '0' && c != '1') return false; out.push_back((uint8_t)(c - '0')); }
    return !out.empty();
}

std::vector<size_t> findPattern(const Bits& b, const Bits& pat, bool inv, int maxMis, size_t limit, std::vector<size_t>* invHits) {
    std::vector<size_t> hits;
    if (pat.empty() || b.size() < pat.size()) return hits;
    for (size_t i = 0; i + pat.size() <= b.size(); i++) {
        int mis = 0, misInv = 0;
        for (size_t k = 0; k < pat.size(); k++) {
            const bool d = (b[i + k] & 1) != pat[k];
            mis += d; misInv += !d;
            if (mis > maxMis && (!inv || misInv > maxMis)) break;
        }
        if (mis <= maxMis) { if (hits.size() < limit) hits.push_back(i); }
        else if (inv && misInv <= maxMis && invHits && invHits->size() < limit) invHits->push_back(i);
    }
    return hits;
}

bool writeBitsText(const std::string& path, const Bits& b, std::string& err) {
    FILE* f = u8fopen(path, "wb");
    if (!f) { err = "cannot write " + path; return false; }
    std::string line;
    for (size_t i = 0; i < b.size(); i++) {
        line += (char)('0' + (b[i] & 1));
        if ((i + 1) % 64 == 0 || i + 1 == b.size()) { line += '\n'; fwrite(line.data(), 1, line.size(), f); line.clear(); }
    }
    fclose(f);
    return true;
}

bool writeBitsBin(const std::string& path, const Bits& b, bool lsbFirst, std::string& err) {
    FILE* f = u8fopen(path, "wb");
    if (!f) { err = "cannot write " + path; return false; }
    Bits padded = b;
    while (padded.size() % 8) padded.push_back(0);
    const std::vector<uint8_t> by = packBits(padded, lsbFirst);
    if (!by.empty()) fwrite(by.data(), 1, by.size(), f);
    fclose(f);
    return true;
}

std::string bitsToString(const Bits& b, size_t from, size_t n) {
    std::string s;
    for (size_t i = from; i < b.size() && i < from + n; i++) s += (char)('0' + (b[i] & 1));
    return s;
}

} // namespace wb
} // namespace dect2
