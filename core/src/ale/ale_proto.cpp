// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// HF ALE word and call layer (see ale_proto.h).
#include "dect2/ale/ale_proto.h"
#include <algorithm>
#include <cstdio>
#include "dect2/common/bits.h"

namespace dect2 {
namespace ale {

namespace {
constexpr uint32_t kPoly = 0xC75;   // x^11 + x^10 + x^6 + x^5 + x^4 + x^2 + 1

uint32_t parity11(uint32_t d12) {
    uint32_t r = d12 << 11;
    for (int b = 22; b >= 11; b--) if (r & (1u << b)) r ^= kPoly << (b - 11);
    return r & 0x7FF;
}

int popc(uint32_t v) { return dect2::bits::popcount(v); }

uint32_t syndrome(uint32_t c24) {   // the check bits received against those of the data received
    return (c24 & 0xFFF) ^ (golayEncode(c24 >> 12) & 0xFFF);
}

struct Table {
    uint32_t e[4096];
    bool set[4096];
    Table() {
        std::fill(std::begin(set), std::end(set), false);
        auto put = [&](uint32_t pat) { const uint32_t s = syndrome(pat); if (!set[s]) { set[s] = true; e[s] = pat; } };
        put(0);
        for (int i = 0; i < 24; i++) put(1u << i);
        for (int i = 0; i < 24; i++) for (int j = i + 1; j < 24; j++) put((1u << i) | (1u << j));
        for (int i = 0; i < 24; i++) for (int j = i + 1; j < 24; j++) for (int k = j + 1; k < 24; k++) put((1u << i) | (1u << j) | (1u << k));
    }
};
const Table& table() { static const Table t; return t; }

const int kGray[8] = {0, 1, 3, 2, 6, 7, 5, 4};   // tone index -> tribit
} // namespace

const char* typeName(int t) {
    static const char* n[8] = {"DATA", "THRU", "TO", "TWS", "FROM", "TIS", "CMD", "REP"};
    return t >= 0 && t < 8 ? n[t] : "?";
}

int tribit(int tone) { return kGray[tone & 7]; }
int tone(int tb) { for (int i = 0; i < 8; i++) if (kGray[i] == (tb & 7)) return i; return 0; }

uint32_t golayEncode(uint32_t d) {
    d &= 0xFFF;
    const uint32_t c23 = (d << 11) | parity11(d);
    return (c23 << 1) | (uint32_t)(popc(c23) & 1);
}

int golayDecode(uint32_t rx, uint32_t& data) {
    rx &= 0xFFFFFF;
    const Table& t = table();
    const uint32_t s = syndrome(rx);
    if (!t.set[s]) { data = rx >> 12; return -1; }
    const uint32_t c = rx ^ t.e[s];
    data = c >> 12;
    return popc(t.e[s]);
}

bool basicChar(char c) { return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '@' || c == '?'; }
bool asciiChar64(char c) { return (unsigned char)c >= 0x20 && (unsigned char)c <= 0x5F; }

uint32_t makeWord(int type, char c1, char c2, char c3) {
    return ((uint32_t)(type & 7) << 21) | ((uint32_t)(c1 & 0x7F) << 14) | ((uint32_t)(c2 & 0x7F) << 7) | (uint32_t)(c3 & 0x7F);
}

void wordBits(uint32_t w, uint8_t out[147]) {
    const uint32_t a = golayEncode(w >> 12), b = golayEncode(w & 0xFFF) ^ 0xFFFFFF;   // B inverted
    uint8_t one[49];
    for (int k = 0; k < 24; k++) { one[2 * k] = (uint8_t)((a >> (23 - k)) & 1); one[2 * k + 1] = (uint8_t)((b >> (23 - k)) & 1); }
    one[48] = 0;
    for (int r = 0; r < 3; r++) for (int i = 0; i < 49; i++) out[49 * r + i] = one[i];
}

void bitsToTones(const uint8_t* bits, int n, std::vector<int>& tones) {
    for (int i = 0; i + 2 < n; i += 3) tones.push_back(tone((bits[i] << 2) | (bits[i + 1] << 1) | bits[i + 2]));
}

Word decodeBits(const uint8_t in[147]) {
    Word w;
    uint8_t one[49];
    for (int i = 0; i < 49; i++) {
        const int s = in[i] + in[49 + i] + in[98 + i];
        one[i] = s >= 2;
        if (s == 1 || s == 2) w.voteErrs++;
    }
    uint32_t a = 0, b = 0;
    for (int k = 0; k < 24; k++) { a = (a << 1) | one[2 * k]; b = (b << 1) | one[2 * k + 1]; }
    b ^= 0xFFFFFF;
    w.stuffOk = one[48] == 0;
    uint32_t da, db;
    const int ea = golayDecode(a, da), eb = golayDecode(b, db);
    w.ok = ea >= 0 && eb >= 0;
    w.golayErrs = (ea < 0 ? 4 : ea) + (eb < 0 ? 4 : eb);
    w.w = (da << 12) | db;
    w.type = (int)(w.w >> 21) & 7;
    w.c[0] = (char)((w.w >> 14) & 0x7F); w.c[1] = (char)((w.w >> 7) & 0x7F); w.c[2] = (char)(w.w & 0x7F);
    return w;
}

std::vector<uint32_t> addressWords(int first, const std::string& addr) {
    std::string a = addr;
    while (a.size() % 3) a += '@';
    if (a.empty()) a = "@@@";
    std::vector<uint32_t> v;
    for (size_t i = 0; i < a.size(); i += 3) {
        const int t = i == 0 ? first : ((i / 3) % 2 == 1 ? Data : Rep);
        v.push_back(makeWord(t, a[i], a[i + 1], a[i + 2]));
    }
    return v;
}

std::vector<uint32_t> amdWords(const std::string& text) {
    std::string a = text;
    while (a.size() % 3 || a.empty()) a += ' ';
    std::vector<uint32_t> v;
    for (size_t i = 0; i < a.size(); i += 3) {
        const int t = i == 0 ? Cmd : ((i / 3) % 2 == 1 ? Data : Rep);
        v.push_back(makeWord(t, a[i], a[i + 1], a[i + 2]));
    }
    return v;
}

uint32_t lqaWord(int ber, int sinad, int mp) {
    const uint32_t v = ((uint32_t)std::min(31, std::max(0, ber)) << 9) | ((uint32_t)std::min(31, std::max(0, sinad)) << 4) | (uint32_t)(mp & 15);
    return makeWord(Cmd, 'a', (char)((v >> 7) & 0x7F), (char)(v & 0x7F));
}

std::string trimAddress(const std::string& a) {
    if (a == "@?@" || a == "@@?") return a;
    std::string s = a;
    while (!s.empty() && s.back() == '@') s.pop_back();
    return s;
}

const char* kindName(int k) {
    static const char* n[] = {"Sounding", "Individual", "Group", "AllCall", "AnyCall", "Unknown"};
    return k >= 0 && k <= Call::Unknown ? n[k] : "Unknown";
}

void Assembler::close() {
    if (open_) fields_.push_back(cur_);
    open_ = false;
}

void Assembler::add(const Word& w) {
    words_++;
    if (w.type == Data || w.type == Rep) {
        if (open_) cur_.text += w.text();
        return;
    }
    close();
    cur_ = Field{w.type, w.text()};
    open_ = true;
}

Call Assembler::finish() {
    close();
    Call c;
    c.words = words_;
    std::string lastAmd;
    auto addU = [](std::vector<std::string>& v, const std::string& s) { if (!s.empty() && std::find(v.begin(), v.end(), s) == v.end()) v.push_back(s); };
    for (const Field& f : fields_) {
        switch (f.type) {
        case To: addU(c.to, trimAddress(f.text)); break;
        case Thru: addU(c.thru, trimAddress(f.text)); break;
        case Tis: case Tws: c.from = trimAddress(f.text); c.conclusion = f.type; break;
        case From: if (c.from.empty()) c.from = trimAddress(f.text); break;
        case Cmd: {
            if (f.text.empty()) break;
            const unsigned char k = (unsigned char)f.text[0];
            if (k >= 0x60) {
                char b[80];
                if (k == 'a' && f.text.size() >= 3) {
                    const uint32_t v = ((uint32_t)(f.text[1] & 0x7F) << 7) | (uint32_t)(f.text[2] & 0x7F);
                    c.lqa = true; c.lqaBer = (int)(v >> 9); c.lqaSinad = (int)((v >> 4) & 31); c.lqaMp = (int)(v & 15);
                    snprintf(b, sizeof b, "LQA BER %d SINAD %d MP %d", c.lqaBer, c.lqaSinad, c.lqaMp);
                } else {
                    uint32_t v = 0;
                    for (size_t i = 1; i < f.text.size() && i < 3; i++) v = (v << 7) | (uint32_t)(f.text[i] & 0x7F);
                    snprintf(b, sizeof b, "CMD %c 0x%04X", k, v);
                }
                addU(c.cmds, b);
            } else if (f.text != lastAmd) {   // an AMD text (once, however often it was repeated)
                lastAmd = f.text;
                std::string t = f.text;
                while (!t.empty() && t.back() == ' ') t.pop_back();
                if (!c.amd.empty()) c.amd += ' ';
                c.amd += t;
            }
            break;
        }
        default: break;
        }
    }
    if (std::find(c.to.begin(), c.to.end(), "@?@") != c.to.end()) c.kind = Call::AllCall;
    else if (std::find(c.to.begin(), c.to.end(), "@@?") != c.to.end()) c.kind = Call::AnyCall;
    else if (!c.thru.empty()) c.kind = Call::Group;
    else if (!c.to.empty()) c.kind = Call::Individual;
    else if (!c.from.empty()) c.kind = Call::Sounding;
    fields_.clear();
    open_ = false;
    words_ = 0;
    return c;
}

} // namespace ale
} // namespace dect2
