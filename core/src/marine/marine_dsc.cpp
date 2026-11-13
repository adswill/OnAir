// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/marine/marine_dsc.h"
#include "dect2/marine/marine_mmsi.h"
#include <algorithm>
#include <cmath>
#include <functional>
#include <cstdio>

namespace dect2 {
namespace marine {

namespace {
int pop7(unsigned v) { int n = 0; for (int i = 0; i < 7; i++) n += (v >> i) & 1; return n; }
bool isEos(int s) { return s == 117 || s == 122 || s == 127; }
// phasing sequence (M.493 Annex 1 section 3): slot 0, 2, .. 10 are DX = 125; odd slots 1..15 carry RX symbols 111 .. 104
int phasingAt(int slot) {
    if (slot < 0 || slot > 15) return -2;
    if (slot % 2 == 0) return slot < 12 ? 125 : -2;      // slots 12 and 14 are the format specifier
    return 111 - (slot - 1) / 2;
}
} // namespace

unsigned dscSymbolBits(int sym) {
    unsigned v = 0;
    for (int i = 0; i < 7; i++) v = (v << 1) | (unsigned)((sym >> i) & 1);
    const int zeros = 7 - pop7((unsigned)sym & 127);
    return (v << 3) | (unsigned)zeros;
}

bool dscSymbolDecode(unsigned bits10, int& sym) {
    int s = 0;
    for (int i = 0; i < 7; i++) s |= (int)((bits10 >> (9 - i)) & 1) << i;
    if ((int)(bits10 & 7) != 7 - pop7((unsigned)s)) return false;
    sym = s;
    return true;
}

const char* dscFormatName(int s) {
    switch (s) {
    case 112: return "Distress alert";
    case 116: return "All ships";
    case 114: return "Group call";
    case 120: return "Individual call";
    case 102: return "Geographic area call";
    case 123: return "Individual call (automatic)";
    default: return "Unknown format";
    }
}
const char* dscCategoryName(int s) {
    switch (s) {
    case 100: return "Routine";
    case 106: return "Ship's business";
    case 108: return "Safety";
    case 110: return "Urgency";
    case 112: return "Distress";
    default: return "Unknown";
    }
}
const char* dscNatureName(int s) {
    switch (s) {
    case 100: return "Fire, explosion";
    case 101: return "Flooding";
    case 102: return "Collision";
    case 103: return "Grounding";
    case 104: return "Listing, danger of capsizing";
    case 105: return "Sinking";
    case 106: return "Disabled and adrift";
    case 107: return "Undesignated distress";
    case 108: return "Abandoning ship";
    case 109: return "Piracy, armed robbery";
    case 110: return "Man overboard";
    case 112: return "EPIRB emission";
    default: return "Unknown";
    }
}
const char* dscTelecmd1Name(int s) {
    switch (s) {
    case 100: return "F3E/G3E simplex telephony";
    case 101: return "F3E/G3E duplex telephony";
    case 103: return "Polling";
    case 104: return "Unable to comply";
    case 105: return "End of call";
    case 106: return "Data";
    case 109: return "J3E telephony";
    case 110: return "Distress acknowledgement";
    case 111: return "H3E telephony";
    case 112: return "Distress relay";
    case 113: return "F1B/J2B TTY-FEC";
    case 115: return "F1B/J2B TTY-ARQ";
    case 116: return "F1B/J2B TTY receive";
    case 118: return "Test";
    case 119: return "F1B/J2B TTY";
    case 120: case 123: return "A1A Morse";
    case 121: return "Position update";
    case 124: return "F1C/F2C/F3C fax";
    case 126: return "No information";
    default: return "";
    }
}
const char* dscTelecmd2Name(int s) {
    switch (s) {
    case 100: return "No reason given";
    case 101: return "Congestion at switching centre";
    case 102: return "Busy";
    case 103: return "Queue indication";
    case 104: return "Station barred";
    case 105: return "No operator available";
    case 106: return "Operator temporarily unavailable";
    case 107: return "Equipment disabled";
    case 108: return "Unable to use proposed channel";
    case 109: return "Unable to use proposed mode";
    case 110: return "Ships and aircraft (Res. 18)";
    case 111: return "Medical transports";
    case 112: return "Pay-phone, public call office";
    case 113: return "Fax or data (M.1081)";
    case 126: return "No information";
    default: return "";
    }
}

std::string dscMmsiFromSymbols(const int* s) {
    char b[16];
    int n = 0;
    for (int i = 0; i < 5; i++) {
        if (s[i] < 0 || s[i] > 99) return "";
        n += snprintf(b + n, sizeof b - (size_t)n, "%02d", s[i]);
    }
    b[9] = 0;                      // the tenth digit is 0 (M.493 section 5.2)
    return b;
}

std::string dscMmsiLenient(const int* s) {
    std::string r;
    char b[4];
    for (int i = 0; i < 5; i++) {
        if (s[i] < 0 || s[i] > 99) r += "~~"; else { snprintf(b, sizeof b, "%02d", s[i]); r += b; }
    }
    r.resize(9);
    return r.find_first_not_of('~') == std::string::npos ? std::string() : r;
}

bool dscMmsiToSymbols(const std::string& m, int out[5]) {
    if (m.size() != 9) return false;
    for (char c : m) if (c < '0' || c > '9') return false;
    const std::string t = m + "0";
    for (int i = 0; i < 5; i++) out[i] = (t[2 * i] - '0') * 10 + (t[2 * i + 1] - '0');
    return true;
}

bool dscPositionFromSymbols(const int* s, double& lat, double& lon, std::string* text) {
    char d[11];
    bool all9 = true;
    for (int i = 0; i < 5; i++) {
        if (s[i] < 0 || s[i] > 99) return false;
        d[2 * i] = (char)('0' + s[i] / 10); d[2 * i + 1] = (char)('0' + s[i] % 10);
        if (s[i] != 99) all9 = false;
    }
    d[10] = 0;
    if (all9) return false;
    const int q = d[0] - '0';
    if (q > 3) return false;
    const int latD = (d[1] - '0') * 10 + (d[2] - '0'), latM = (d[3] - '0') * 10 + (d[4] - '0');
    const int lonD = (d[5] - '0') * 100 + (d[6] - '0') * 10 + (d[7] - '0'), lonM = (d[8] - '0') * 10 + (d[9] - '0');
    if (latD > 90 || latM > 59 || lonD > 180 || lonM > 59) return false;
    lat = latD + latM / 60.0; lon = lonD + lonM / 60.0;
    const bool south = q == 2 || q == 3, west = q == 1 || q == 3;
    if (south) lat = -lat;
    if (west) lon = -lon;
    if (text) {
        char b[48];
        snprintf(b, sizeof b, "%02dd%02d'%c %03dd%02d'%c", latD, latM, south ? 'S' : 'N', lonD, lonM, west ? 'W' : 'E');
        *text = b;
    }
    return true;
}

void dscPositionToSymbols(double lat, double lon, int out[5]) {
    const bool south = lat < 0, west = lon < 0;
    const double al = std::fabs(lat), ao = std::fabs(lon);
    int latD = (int)al, latM = (int)std::lround((al - latD) * 60.0);
    int lonD = (int)ao, lonM = (int)std::lround((ao - lonD) * 60.0);
    if (latM == 60) { latD++; latM = 0; }
    if (lonM == 60) { lonD++; lonM = 0; }
    const int q = (south ? 2 : 0) + (west ? 1 : 0);
    // the digits: q, latD (2), latM (2), lonD (3), lonM (2)
    char d[11];
    snprintf(d, sizeof d, "%d%02d%02d%03d%02d", q, latD % 100, latM, lonD % 1000, lonM);
    for (int i = 0; i < 5; i++) out[i] = (d[2 * i] - '0') * 10 + (d[2 * i + 1] - '0');
}

bool dscTimeFromSymbols(int a, int b, int& h, int& m) {
    if (a < 0 || a > 99 || b < 0 || b > 99) return false;
    if (a == 88 && b == 88) return false;
    if (a > 23 || b > 59) return false;
    h = a; m = b;
    return true;
}

std::string dscFrequencyText(const int* s) {
    if (s[0] == 126 && s[1] == 126 && s[2] == 126) return "";
    for (int i = 0; i < 3; i++) if (s[i] < 0 || s[i] > 99) return "";
    // s is in the order sent; M.493-15 Table 13 note 2: character 1 (units and tens of 100 Hz) is the last one sent
    const int c3 = s[0], c2 = s[1], c1 = s[2];
    char b[48];
    const int hm = c3 / 10;
    if (hm == 3) { snprintf(b, sizeof b, "MF/HF channel %d", (c3 % 10) * 10000 + c2 * 100 + c1); return b; }
    if (hm == 9) {
        const int m = c2 / 10;
        snprintf(b, sizeof b, "VHF ch %d%s", (c2 % 10) * 100 + c1, m == 1 ? " (ship simplex)" : m == 2 ? " (coast simplex)" : "");
        return b;
    }
    snprintf(b, sizeof b, "%.1f kHz", (c3 * 10000 + c2 * 100 + c1) / 10.0);
    return b;
}

void dscFrequencyToSymbols(long v, int out[3]) {
    if (v < 0) { out[0] = out[1] = out[2] = 126; return; }
    out[0] = (int)((v / 10000) % 100); out[1] = (int)((v / 100) % 100); out[2] = (int)(v % 100);   // sent HM TM first, T U last
}

// ---------------------------------------------------------------------------------------------------------------------------------
// builders

static void put(std::vector<int>& v, const int* a, int n) { for (int i = 0; i < n; i++) v.push_back(a[i]); }

static void putMmsi(std::vector<int>& v, const std::string& m) {
    int s[5] = {126, 126, 126, 126, 126};
    dscMmsiToSymbols(m, s);
    put(v, s, 5);
}

static void putDistressBlock(std::vector<int>& v, int nature, double lat, double lon, int h, int mi, int subsequent) {
    v.push_back(nature);
    int p[5];
    if (std::isnan(lat)) for (int& x : p) x = 99; else dscPositionToSymbols(lat, lon, p);
    put(v, p, 5);
    if (h < 0) { v.push_back(88); v.push_back(88); } else { v.push_back(h); v.push_back(mi); }
    v.push_back(subsequent);
}

std::vector<int> dscBuildDistress(const std::string& mmsi, int nature, double lat, double lon, int h, int mi, int subsequent) {
    std::vector<int> v;
    v.push_back(112);
    putMmsi(v, mmsi);
    putDistressBlock(v, nature, lat, lon, h, mi, subsequent);
    return v;
}

static void putFreqs(std::vector<int>& v, long rx, long tx) {
    int f[3];
    dscFrequencyToSymbols(rx, f); put(v, f, 3);
    dscFrequencyToSymbols(tx, f); put(v, f, 3);
}

std::vector<int> dscBuildAllShips(const std::string& mmsi, int cat, int tc1, int tc2, long rx, long tx) {
    std::vector<int> v;
    v.push_back(116); v.push_back(cat);
    putMmsi(v, mmsi);
    v.push_back(tc1); v.push_back(tc2);
    putFreqs(v, rx, tx);
    return v;
}

std::vector<int> dscBuildIndividual(const std::string& to, int cat, const std::string& from, int tc1, int tc2, long rx, long tx) {
    std::vector<int> v;
    v.push_back(120);
    putMmsi(v, to);
    v.push_back(cat);
    putMmsi(v, from);
    v.push_back(tc1); v.push_back(tc2);
    putFreqs(v, rx, tx);
    return v;
}

std::vector<int> dscBuildDistressAck(const std::string& from, const std::string& dm, int nature, double lat, double lon, int h, int mi, int sub) {
    std::vector<int> v;
    v.push_back(116); v.push_back(112);
    putMmsi(v, from);
    v.push_back(110);
    putMmsi(v, dm);
    putDistressBlock(v, nature, lat, lon, h, mi, sub);
    return v;
}

int dscEcc(const std::vector<int>& body, int eos) {
    int e = eos & 127;
    for (int s : body) e ^= (s & 127);
    return e;
}

std::vector<int> dscFrameSymbols(const std::vector<int>& body, int eos) {
    std::vector<int> dx;
    dx.push_back(body[0]); dx.push_back(body[0]);
    for (size_t i = 1; i < body.size(); i++) dx.push_back(body[i]);
    dx.push_back(eos); dx.push_back(dscEcc(body, eos)); dx.push_back(eos); dx.push_back(eos);
    const size_t last = 12 + 2 * (dx.size() - 3) + 5;     // the last place: the retransmission of the ECC (Fig. 1b of M.493 ends with it)
    std::vector<int> slots(last + 1, 126);
    for (int s = 0; s < 16; s++) { const int p = phasingAt(s); if (p >= 0) slots[(size_t)s] = p; }
    for (size_t i = 0; i < dx.size(); i++) {
        slots[12 + 2 * i] = dx[i];
        const size_t rx = 12 + 2 * i + 5;
        if (rx <= last && rx >= 16) slots[rx] = dx[i];
    }
    return slots;
}

std::vector<uint8_t> dscFrameBits(const std::vector<int>& body, int eos, int dotBits) {
    std::vector<uint8_t> bits;
    for (int i = 0; i < dotBits; i++) bits.push_back((uint8_t)(i % 2 == 0 ? 1 : 0));   // Y B Y B ... ending on B, so that 125 starts with Y
    for (int s : dscFrameSymbols(body, eos)) {
        const unsigned b = dscSymbolBits(s);
        for (int i = 9; i >= 0; i--) bits.push_back((uint8_t)((b >> i) & 1));
    }
    return bits;
}

// ---------------------------------------------------------------------------------------------------------------------------------
// parsing

// What the call is, from the format specifier, category, first telecommand and the end of sequence (M.493 sections 4 and 6, M.541).
std::string dscCallKind(const DscCall& c) {
    const bool ack = c.eos == 122;
    if (c.format == 112) return "Distress alert";
    if (c.category == 112 && c.telecmd1 == 110) return c.format == 116 ? "Distress alert acknowledgement" : "Distress alert relay acknowledgement";
    if (c.category == 112 && c.telecmd1 == 112) return c.format == 116 ? "Distress alert relay (all ships)" : c.format == 102 ? "Distress alert relay (area)" : "Distress alert relay";
    std::string who = c.format == 116 ? "All ships" : c.format == 102 ? "Area" : c.format == 114 ? "Group" : (c.format == 120 || c.format == 123) ? "Individual" : "";
    if (who.empty()) return c.formatName;
    if (c.telecmd1 == 118) return ack ? "Test acknowledgement" : "Test call";
    if (c.telecmd1 == 121) return "Position update";
    if (c.telecmd1 == 103) return ack ? "Position reply" : "Position request (polling)";
    if (ack && c.hasPos) return "Position reply";
    if (ack) return who + " call acknowledgement";
    if (c.category == 110) return who + " urgency call";
    if (c.category == 108) return who + " safety call";
    return who + " call";
}

DscCall dscParseCall(const std::vector<int>& b, int eos, bool eccOk, int erasures) {
    DscCall c;
    c.eos = eos; c.eccOk = eccOk; c.erasures = erasures;
    size_t i = 0;
    auto get = [&](size_t k) { return k < b.size() ? b[k] : -1; };
    auto take5 = [&](int* s) { for (int k = 0; k < 5; k++) s[k] = get(i + (size_t)k); i += 5; };
    c.format = get(i++);
    c.formatName = dscFormatName(c.format);
    int s5[5];
    std::string toText;
    if (c.format == 112) {
        c.distress = true; c.category = 112;
    } else {
        if (c.format == 116) toText = "all ships";
        else if (c.format == 120 || c.format == 114 || c.format == 123) {
            take5(s5);
            toText = dscMmsiLenient(s5);
        } else if (c.format == 102) {
            take5(s5);
            bool okd = true; char d[11];
            for (int k = 0; k < 5; k++) { if (s5[k] < 0 || s5[k] > 99) okd = false; else { d[2 * k] = (char)('0' + s5[k] / 10); d[2 * k + 1] = (char)('0' + s5[k] % 10); } }
            if (okd) {
                d[10] = 0;
                const int q = d[0] - '0';
                char t[96];
                // digits: quadrant, latitude (2), longitude (3), latitude span (2), longitude span (2); the reference point is the NW corner
                snprintf(t, sizeof t, "area ref %c%c%c %c%c%c%c, %c%c deg x %c%c deg", q == 2 || q == 3 ? 'S' : 'N', d[1], d[2], q == 1 || q == 3 ? 'W' : 'E', d[3], d[4], d[5], d[6], d[7], d[8], d[9]);
                toText = t;
            }
        } else toText = "";
        c.category = get(i++);
    }
    c.categoryName = dscCategoryName(c.category);
    take5(s5);
    c.fromMmsi = dscMmsiLenient(s5);
    auto distressBlock = [&]() {
        c.nature = get(i++);
        c.natureName = dscNatureName(c.nature);
        int p[5]; take5(p);
        c.hasPos = dscPositionFromSymbols(p, c.lat, c.lon, &c.posText);
        const int a = get(i), bb = get(i + 1); i += 2;
        c.hasTime = dscTimeFromSymbols(a, bb, c.utcHour, c.utcMin);
        c.telecmd2 = get(i++);      // the type of communication wanted afterwards
    };
    if (c.format == 112) {
        distressBlock();
        c.telecmd1 = -1;
        c.to = "all stations";
    } else {
        c.to = toText;
        c.telecmd1 = get(i++);
        if (c.category == 112 && (c.telecmd1 == 110 || c.telecmd1 == 112)) {
            c.distress = true;
            int s[5]; take5(s);
            c.distressMmsi = dscMmsiLenient(s);
            distressBlock();
        } else {
            c.telecmd2 = get(i++);
            if (c.category == 110 || c.category == 112) c.distress = c.category == 112;
            int f[6];
            for (int k = 0; k < 6; k++) f[k] = get(i + (size_t)k);
            const size_t left = b.size() > i ? b.size() - i : 0;
            if (left >= 6 && f[0] == 55) {
                int p[5] = {f[1], f[2], f[3], f[4], f[5]};
                c.hasPos = dscPositionFromSymbols(p, c.lat, c.lon, &c.posText);
                if (left >= 8) c.hasTime = dscTimeFromSymbols(get(i + 6), get(i + 7), c.utcHour, c.utcMin);
            } else if (left >= 3) {
                c.freqRx = dscFrequencyText(f);
                if (left >= 6) c.freqTx = dscFrequencyText(f + 3);
            }
        }
    }
    if (c.format == 112 || c.category == 112) c.distress = true;
    {
        std::string t;
        if (c.telecmd1 >= 0) t = dscTelecmd1Name(c.telecmd1);
        if (c.format == 112 || (c.distress && c.telecmd1 >= 0)) {
            // telecmd2 holds the type of communication proposed
            const char* n = dscTelecmd1Name(c.telecmd2);
            if (*n) { if (!t.empty()) t += "; "; t += std::string("then ") + n; }
        } else if (c.telecmd2 >= 0) {
            const char* n = dscTelecmd2Name(c.telecmd2);
            if (*n && c.telecmd2 != 126) { if (!t.empty()) t += "; "; t += n; }
        }
        c.telecmdText = t;
    }
    c.symbols = b;
    {
        const MmsiInfo f = mmsiInfo(c.fromMmsi), d = mmsiInfo(c.distressMmsi);
        c.fromCountry = f.country; c.fromType = mmsiKindName(f.kind);
        c.distressCountry = d.country;
        if (!c.to.empty() && c.to.size() == 9 && c.to.find_first_not_of("0123456789~") == std::string::npos) {
            const MmsiInfo t = mmsiInfo(c.to);
            c.toCountry = t.country; c.toType = mmsiKindName(t.kind);
        }
        c.kind = dscCallKind(c);
    }
    char line[400];
    std::string pos = c.hasPos ? " at " + c.posText : "";
    std::string tm;
    if (c.hasTime) { char q[16]; snprintf(q, sizeof q, " %02d:%02d UTC", c.utcHour, c.utcMin); tm = q; }
    snprintf(line, sizeof line, "%s from %s to %s%s%s%s%s", c.formatName.c_str(), c.fromMmsi.empty() ? "?" : c.fromMmsi.c_str(), c.to.empty() ? "?" : c.to.c_str(),
             c.nature >= 0 ? (" - " + c.natureName).c_str() : "", pos.c_str(), tm.c_str(), c.eccOk ? "" : " (check failed)");
    c.text = line;
    return c;
}

// ---------------------------------------------------------------------------------------------------------------------------------
// decoder

DscDecoder::DscDecoder(bool invert, bool vhf) : invert_(invert), vhf_(vhf) {}

void DscDecoder::reset() {
    reg_ = 0; for (float& c : regConf_) c = 0;
    bitCount_ = 0; for (auto& h : hist_) h.clear();
    locked_ = false; slots_.clear();
}

void DscDecoder::abortLock() {
    locked_ = false; slots_.clear();
    for (auto& h : hist_) h.clear();
}

void DscDecoder::pushBit(float soft) {
    int bit = soft > 0 ? 1 : 0;
    if (invert_) bit ^= 1;
    float conf = std::fabs(soft); if (conf > 1.f) conf = 1.f;
    reg_ = ((reg_ << 1) | (unsigned)bit) & 0x3FF;
    for (int i = 0; i < 9; i++) regConf_[i] = regConf_[i + 1];
    regConf_[9] = conf;
    bitCount_++;
    if (bitCount_ < 10) return;
    const int ph = (int)(bitCount_ % 10);
    int s = -1;
    int sv;
    if (dscSymbolDecode(reg_, sv)) s = sv;
    float cmin = 1.f; for (float c : regConf_) cmin = std::min(cmin, c);
    if (locked_) {
        if (ph != lockPhase_) return;
        onSymbol(s, cmin, reg_);
        return;
    }
    auto& h = hist_[ph];
    Sym sy; sy.s = s; sy.conf = cmin; sy.raw = reg_;
    h.push_back(sy);
    if (h.size() > 24) h.erase(h.begin());
    // phasing hunt: three symbols of the phasing sequence in their places (M.493 section 3.3)
    if (s < 0) return;
    const int n = (int)h.size();
    int bestSlot = -1, bestCount = 0;
    for (int slot = 0; slot < 16 && slot < n; slot++) {
        if (phasingAt(slot) != s) continue;
        int cnt = 0;
        for (int j = 0; j <= slot; j++) if (h[(size_t)(n - 1 - j)].s == phasingAt(slot - j)) cnt++;
        if (cnt > bestCount) { bestCount = cnt; bestSlot = slot; }
    }
    if (bestCount >= 3) {
        locked_ = true; lockPhase_ = ph;
        slots_.assign(h.end() - (bestSlot + 1), h.end());
        for (auto& hh : hist_) hh.clear();
        tryFinish();
    }
}

void DscDecoder::onSymbol(int s, float conf, unsigned raw) {
    Sym sy; sy.s = s; sy.conf = conf; sy.raw = raw;
    slots_.push_back(sy);
    tryFinish();
}

void DscDecoder::tryFinish() {
    const size_t N = slots_.size();
    if (N > 12 + 2 * 140) { abortLock(); return; }
    // carrier gone: the last 30 slots hold almost no valid symbol
    if (N >= 60) {
        int bad = 0;
        for (size_t k = N - 30; k < N; k++) if (slots_[k].s < 0) bad++;
        if (bad >= 24) {
            // a call cut short is reported as a bad call when enough of it arrived
            const bool partial = N >= 12 + 2 * 8 + 30;
            if (partial && cb_) {
                std::vector<int> body;
                const size_t lastGood = N - 30;
                int era = 0;
                const int f0 = slots_.size() > 12 ? slots_[12].s : -1;
                body.push_back(f0);
                for (size_t i = 2; 12 + 2 * i < lastGood; i++) { const int d = slots_[12 + 2 * i].s; const int r = 12 + 2 * i + 5 < N ? slots_[12 + 2 * i + 5].s : -1; const int m = d >= 0 ? d : r; if (m < 0) era++; body.push_back(m); }
                DscCall c = dscParseCall(body, -1, false, era);
                c.vhf = vhf_;
                bad_++;
                cb_(c);
            }
            abortLock();
            return;
        }
    }
    if (N < 12 + 2 * 3) return;
    auto tok = [&](size_t i, bool& pending, bool& conflict, int& alt) {
        const size_t dxs = 12 + 2 * i, rxs = dxs + 5;
        pending = rxs >= N;
        const int d = dxs < N ? slots_[dxs].s : -1;
        const int r = rxs < N ? slots_[rxs].s : -1;
        conflict = d >= 0 && r >= 0 && d != r;
        alt = r;
        if (d >= 0) return d;
        return r;
    };
    // the end of sequence: the first of 117, 122, 127 among the information symbols after the two format specifiers
    size_t eosIdx = 0; bool found = false;
    const size_t maxTok = (N - 1 - 12) / 2;
    for (size_t i = 2; i <= maxTok; i++) {
        bool pend, conf; int alt;
        const int m = tok(i, pend, conf, alt);
        if (pend) break;
        if (isEos(m)) { eosIdx = i; found = true; break; }
    }
    if (!found) return;
    bool pend, conf; int alt;
    tok(eosIdx + 1, pend, conf, alt);
    if (pend) return;                         // wait for the retransmission of the ECC
    // merge: per information symbol the candidates (the valid copy of the DX place first, then the RX place), then the check character
    // decides: fewest changes to the first choices that make the even parity right (symbol-level correction with time diversity)
    struct Cand { std::vector<int> v; float conf = 0; };
    auto cand = [&](size_t dxs, size_t rxs, Cand& cd) {
        const Sym* d = dxs < N ? &slots_[dxs] : nullptr;
        const Sym* r = rxs < N ? &slots_[rxs] : nullptr;
        if (d && d->s >= 0) { cd.v.push_back(d->s); cd.conf = d->conf; }
        if (r && r->s >= 0 && (cd.v.empty() || r->s != cd.v[0])) { cd.v.push_back(r->s); if (cd.v.size() == 1) cd.conf = r->conf; }
        if (cd.v.empty()) {   // no valid copy: unknown (the check character may give it back when it is the only one)
            cd.conf = 0;
            return false;
        }
        return true;
    };
    std::vector<Cand> cands;                // body positions, the check character last
    std::vector<bool> known;
    {
        Cand c0;                            // the format specifier is sent twice in each place: four copies
        bool k0 = false;
        for (size_t dxs : {size_t(12), size_t(14)}) {
            Cand t; const bool ok = cand(dxs, dxs + 5, t);
            if (ok) { k0 = true; for (int v : t.v) if (std::find(c0.v.begin(), c0.v.end(), v) == c0.v.end()) c0.v.push_back(v); if (c0.conf == 0) c0.conf = t.conf; }
        }
        if (!k0) { Cand t; cand(12, 17, t); c0 = t; }
        cands.push_back(c0); known.push_back(k0);
    }
    for (size_t i = 2; i < eosIdx; i++) { Cand cd; const bool ok = cand(12 + 2 * i, 12 + 2 * i + 5, cd); cands.push_back(cd); known.push_back(ok); }
    const size_t eccPos = cands.size();
    { Cand cd; const bool ok = cand(12 + 2 * (eosIdx + 1), 12 + 2 * (eosIdx + 1) + 5, cd); cands.push_back(cd); known.push_back(ok); }
    const int eos = tok(eosIdx, pend, conf, alt);
    int erasures = 0;
    std::vector<size_t> unk;
    for (size_t k = 0; k < eccPos; k++) if (!known[k]) { erasures++; unk.push_back(k); }
    std::vector<int> pick(cands.size(), 0);
    bool eccOk = false;
    int repaired = 0;
    bool recovered = false;
    std::vector<int> body(eccPos, -1);
    auto eccOf = [&](const std::vector<int>& bd) { return dscEcc(bd, eos); };
    if (known[eccPos] || !cands[eccPos].v.empty()) {
        // positions that may change, the least certain first
        std::vector<size_t> order;
        for (size_t k = 0; k <= eccPos; k++) if (cands[k].v.size() > 1) order.push_back(k);
        std::stable_sort(order.begin(), order.end(), [&](size_t x, size_t y) { return cands[x].conf < cands[y].conf; });
        auto build = [&](std::vector<int>& bd, int& ecc) {
            bd.assign(eccPos, -1);
            for (size_t k = 0; k < eccPos; k++) if (!cands[k].v.empty()) bd[k] = cands[k].v[(size_t)pick[k]];
            ecc = cands[eccPos].v.empty() ? -1 : cands[eccPos].v[(size_t)pick[eccPos]];
        };
        bool done = false;
        if (unk.empty()) {
            // iterative deepening over the number of changed positions (the unknown symbols count as changes of their own)
            std::vector<size_t> flex = order;
            const size_t nu = unk.size();
            for (int maxDev = 0; maxDev <= 3 && !done; maxDev++) {
                std::function<void(size_t, int)> dfs = [&](size_t idx, int left) {
                    if (done) return;
                    if (idx == flex.size()) {
                        std::vector<int> bd; int ecc;
                        build(bd, ecc);
                        for (int v : bd) if (v < 0) return;
                        if (ecc >= 0 && eccOf(bd) == ecc) { done = true; body = bd; repaired = maxDev - left; }
                        return;
                    }
                    const size_t k = flex[idx];
                    const bool isUnknown = k < eccPos && !known[k];
                    for (size_t j = 0; j < cands[k].v.size(); j++) {
                        const int cost = (j > 0 && !isUnknown) ? 1 : 0;
                        if (cost > left) break;
                        pick[k] = (int)j;
                        dfs(idx + 1, left - cost);
                        if (done) return;
                    }
                    pick[k] = 0;
                };
                // positions with one candidate or none are fixed; unknown ones with no candidates make the search empty
                bool hopeless = false;
                for (size_t u : unk) if (cands[u].v.empty()) hopeless = true;
                if (!hopeless) dfs(0, maxDev);
                (void)nu;
            }
            if (done) eccOk = true;
        }
        if (!done && unk.size() == 1 && known[eccPos]) {
            // one symbol lost in both copies and no single wrong bit explains it (or the check refused them): the check character gives it back
            bool conflicts = false;
            for (size_t k = 0; k <= eccPos; k++) if (k != unk[0] && cands[k].v.size() > 1) conflicts = true;
            if (!conflicts) {
                std::vector<int> bd(eccPos, -1);
                int x = eos & 127;
                for (size_t k = 0; k < eccPos; k++) if (k != unk[0]) { bd[k] = cands[k].v[0]; x ^= bd[k] & 127; }
                x ^= cands[eccPos].v[0] & 127;
                bd[unk[0]] = x;
                body = bd; eccOk = true; recovered = true; repaired = 1; done = true;
            }
        }
        if (!done) {   // nothing fits: show the first choices, the check failed
            std::fill(pick.begin(), pick.end(), 0);
            std::vector<int> bd; int ecc;
            build(bd, ecc);
            body = bd;
        }
    } else {
        for (size_t k = 0; k < eccPos; k++) body[k] = cands[k].v.empty() ? -1 : cands[k].v[0];
    }
    DscCall c = dscParseCall(body, eos, eccOk, recovered ? 0 : erasures);
    c.repaired = repaired; c.recovered = recovered;
    c.vhf = vhf_;
    if (eccOk) ok_++; else bad_++;
    abortLock();
    if (cb_) cb_(c);
}

} // namespace marine
} // namespace dect2
