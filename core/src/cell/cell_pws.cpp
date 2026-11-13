// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Cell towers: public warning message content (TS 23.041, TS 23.038).
#include "dect2/cell/cell_pws.h"
#include <cstdio>

namespace dect2::cell {

namespace {

const char* const kGsm7[128] = {
    "@", "\xC2\xA3", "$", "\xC2\xA5", "\xC3\xA8", "\xC3\xA9", "\xC3\xB9", "\xC3\xAC", "\xC3\xB2", "\xC3\x87", "\n", "\xC3\x98", "\xC3\xB8", "\r", "\xC3\x85", "\xC3\xA5",
    "\xCE\x94", "_", "\xCE\xA6", "\xCE\x93", "\xCE\x9B", "\xCE\xA9", "\xCE\xA0", "\xCE\xA8", "\xCE\xA3", "\xCE\x98", "\xCE\x9E", "", "\xC3\x86", "\xC3\xA6", "\xC3\x9F", "\xC3\x89",
    " ", "!", "\"", "#", "\xC2\xA4", "%", "&", "'", "(", ")", "*", "+", ",", "-", ".", "/",
    "0", "1", "2", "3", "4", "5", "6", "7", "8", "9", ":", ";", "<", "=", ">", "?",
    "\xC2\xA1", "A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M", "N", "O",
    "P", "Q", "R", "S", "T", "U", "V", "W", "X", "Y", "Z", "\xC3\x84", "\xC3\x96", "\xC3\x91", "\xC3\x9C", "\xC2\xA7",
    "\xC2\xBF", "a", "b", "c", "d", "e", "f", "g", "h", "i", "j", "k", "l", "m", "n", "o",
    "p", "q", "r", "s", "t", "u", "v", "w", "x", "y", "z", "\xC3\xA4", "\xC3\xB6", "\xC3\xB1", "\xC3\xBC", "\xC3\xA0",
};

const char* gsm7Ext(int c) {
    switch (c) {
        case 0x0A: return "\f";
        case 0x14: return "^";
        case 0x28: return "{";
        case 0x29: return "}";
        case 0x2F: return "\\";
        case 0x3C: return "[";
        case 0x3D: return "~";
        case 0x3E: return "]";
        case 0x40: return "|";
        case 0x65: return "\xE2\x82\xAC";
        default: return " ";          // an unknown extension shows as a space (23.038 sec 6.2.1.1)
    }
}

std::vector<int> gsm7Septets(const uint8_t* d, size_t n) {
    std::vector<int> s;
    const size_t nChars = n * 8 / 7;
    s.reserve(nChars);
    for (size_t i = 0; i < nChars; i++) {
        const size_t bit = i * 7, byte = bit / 8;
        const int sh = (int)(bit % 8);
        int v = d[byte] >> sh;
        if (sh > 1 && byte + 1 < n) v |= d[byte + 1] << (8 - sh);
        s.push_back(v & 0x7f);
    }
    // n a multiple of 7 leaves 7 spare bits at the end when the text stops one septet short: they are zero
    if (n % 7 == 0 && !s.empty() && s.back() == 0) s.pop_back();
    return s;
}

void appendUtf8(std::string& o, uint32_t c) {
    if (c < 0x80) o.push_back((char)c);
    else if (c < 0x800) { o.push_back((char)(0xC0 | (c >> 6))); o.push_back((char)(0x80 | (c & 63))); }
    else { o.push_back((char)(0xE0 | (c >> 12))); o.push_back((char)(0x80 | ((c >> 6) & 63))); o.push_back((char)(0x80 | (c & 63))); }
}

std::string ucs2ToUtf8(const uint8_t* d, size_t n) {
    std::string o;
    for (size_t i = 0; i + 1 < n; i += 2) {
        const uint32_t c = ((uint32_t)d[i] << 8) | d[i + 1];
        if (c == 0) break;
        if (c >= 0xD800 && c <= 0xDBFF && i + 3 < n) {  // a UTF-16 pair: networks send these although the table says UCS2
            const uint32_t lo = ((uint32_t)d[i + 2] << 8) | d[i + 3];
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                const uint32_t cp = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00);
                o.push_back((char)(0xF0 | (cp >> 18))); o.push_back((char)(0x80 | ((cp >> 12) & 63)));
                o.push_back((char)(0x80 | ((cp >> 6) & 63))); o.push_back((char)(0x80 | (cp & 63)));
                i += 2;
                continue;
            }
        }
        appendUtf8(o, c);
    }
    return o;
}

void trimPad(std::string& s) { while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == '\0')) s.pop_back(); }

enum Coding { kCode7, kCode8, kCodeUcs2 };

struct Dcs { Coding code = kCode7; std::string lang; bool lang7Prefix = false, langUcs2Prefix = false, compressed = false; };

Dcs parseDcs(int dcs) {
    Dcs d;
    if (dcs < 0) return d;
    static const char* const g0[16] = {"German", "English", "Italian", "French", "Spanish", "Dutch", "Swedish", "Danish", "Portuguese", "Finnish", "Norwegian", "Greek", "Turkish", "Hungarian", "Polish", ""};
    static const char* const g2[5] = {"Czech", "Hebrew", "Arabic", "Russian", "Icelandic"};
    const int grp = dcs >> 4, lo = dcs & 15;
    if (grp == 0) d.lang = g0[lo];
    else if (grp == 1) { if (lo == 0) d.lang7Prefix = true; else if (lo == 1) { d.code = kCodeUcs2; d.langUcs2Prefix = true; } }
    else if (grp == 2) { if (lo < 5) d.lang = g2[lo]; }
    else if ((grp & 12) == 4) {
        d.compressed = (dcs & 0x20) != 0;
        const int cs = (dcs >> 2) & 3;
        if (cs == 1) d.code = kCode8; else if (cs == 2) d.code = kCodeUcs2;
    } else if (grp == 15) d.code = (dcs & 4) ? kCode8 : kCode7;
    return d;
}

std::string decodePage(const uint8_t* p, size_t n, const Dcs& d) {
    if (d.code == kCodeUcs2) return ucs2ToUtf8(p, n);
    if (d.code == kCode8) { std::string o; for (size_t i = 0; i < n && p[i]; i++) appendUtf8(o, p[i]); return o; }
    return gsm7ToUtf8(p, n);
}

} // namespace

std::string gsm7ToUtf8(const uint8_t* d, size_t n) {
    std::string o;
    const std::vector<int> s = gsm7Septets(d, n);
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == 0x1B) { if (i + 1 < s.size()) o += gsm7Ext(s[++i]); continue; }
        o += kGsm7[s[i]];
    }
    trimPad(o);
    return o;
}

const char* etwsTypeName(uint32_t warningType) {
    switch ((warningType >> 9) & 0x7f) {
        case 0: return "earthquake";
        case 1: return "tsunami";
        case 2: return "earthquake and tsunami";
        case 3: return "test";
        case 4: return "other";
        default: return "reserved";
    }
}

std::string pwsCategory(uint32_t id, bool* etws) {
    if (etws) *etws = false;
    if (id >= 4352 && id <= 4359) {
        if (etws) *etws = true;
        static const char* const n[8] = {"ETWS earthquake", "ETWS tsunami", "ETWS earthquake and tsunami", "ETWS test", "ETWS other emergency", "ETWS future use", "ETWS future use", "ETWS future use"};
        return n[id - 4352];
    }
    static const char* const c[13] = {
        "CMAS presidential level / EU-Alert level 1",
        "CMAS extreme: immediate, observed / EU-Alert level 2",
        "CMAS extreme: immediate, likely / EU-Alert level 2",
        "CMAS extreme: expected, observed / EU-Alert level 2",
        "CMAS extreme: expected, likely / EU-Alert level 2",
        "CMAS severe: immediate, observed / EU-Alert level 2",
        "CMAS severe: immediate, likely / EU-Alert level 2",
        "CMAS severe: expected, observed / EU-Alert level 3",
        "CMAS severe: expected, likely / EU-Alert level 3",
        "CMAS child abduction (AMBER)",
        "CMAS required monthly test",
        "CMAS exercise",
        "CMAS operator defined",
    };
    if (id >= 4370 && id <= 4382) return c[id - 4370];
    if (id >= 4383 && id <= 4395) return std::string(c[id - 4383]) + " (additional language)";
    if (id >= 4396 && id <= 4399) return "CMAS public safety / local";
    return "";
}

bool pwsDecodeText(const std::vector<uint8_t>& c, int dcs, std::string& text, std::string& lang) {
    text.clear(); lang.clear();
    if (c.empty()) return false;
    const Dcs d = parseDcs(dcs);
    lang = d.lang;
    if (d.compressed) { text = "[compressed text, not decoded]"; return true; }   // 23.042: not used by warning systems
    const int pages = c[0];
    const bool cbData = pages >= 1 && pages <= 15 && c.size() == (size_t)1 + (size_t)pages * 83;
    std::vector<std::vector<uint8_t>> blocks;
    if (cbData) {
        for (int i = 0; i < pages; i++) {
            const uint8_t* p = c.data() + 1 + (size_t)i * 83;
            size_t len = p[82];
            if (len > 82) len = 82;
            blocks.emplace_back(p, p + len);
        }
    } else blocks.push_back(c);
    bool first = true;
    for (auto& b : blocks) {
        if (first && d.lang7Prefix && b.size() >= 3) {
            // two characters, CR, then the text in the same septet stream
            std::string all = gsm7ToUtf8(b.data(), b.size());
            if (all.size() > 3 && all[2] == '\r') { lang = all.substr(0, 2); all = all.substr(3); }
            text += all;
        } else if (first && d.langUcs2Prefix && b.size() >= 2) {
            lang = gsm7ToUtf8(b.data(), 2);
            text += ucs2ToUtf8(b.data() + 2, b.size() - 2);
        } else text += decodePage(b.data(), b.size(), d);
        first = false;
    }
    trimPad(text);
    return true;
}

bool PwsAssembler::add(const LteWarnSeg& s, PwsMessage& m) {
    const uint64_t key = ((uint64_t)s.sib << 32) | (s.messageId << 16) | s.serial;
    Slot& sl = slots_[key];
    if (sl.done) return false;
    m = PwsMessage();
    m.messageId = s.messageId; m.serial = s.serial; m.sib = s.sib;
    m.geoScope = (int)(s.serial >> 14) & 3; m.messageCode = (int)(s.serial >> 4) & 0x3ff; m.updateNumber = (int)s.serial & 15;
    m.category = pwsCategory(s.messageId, &m.etws);
    if (s.sib == 10) {
        sl.done = true;
        m.complete = true; m.warningType = s.warningType; m.etws = true; m.segments = 1;
        char b[120];
        snprintf(b, sizeof b, "primary notification: %s%s%s", etwsTypeName(s.warningType), (s.warningType & 0x0100) ? ", user alert" : "", (s.warningType & 0x0080) ? ", popup" : "");
        m.text = b;
        return true;
    }
    if (s.segment < 0 || s.segment > 63) return false;
    if (s.dcs >= 0) sl.dcs = s.dcs;
    sl.segs[s.segment] = s.data;
    if (s.last) sl.last = s.segment;
    if (sl.last < 0) return false;
    for (int i = 0; i <= sl.last; i++) if (!sl.segs.count(i)) return false;
    std::vector<uint8_t> all;
    for (int i = 0; i <= sl.last; i++) { const auto& d = sl.segs[i]; all.insert(all.end(), d.begin(), d.end()); }
    m.complete = true; m.segments = sl.last + 1;
    if (!pwsDecodeText(all, sl.dcs, m.text, m.lang)) return false;
    sl.done = true;
    return true;
}

} // namespace dect2::cell
