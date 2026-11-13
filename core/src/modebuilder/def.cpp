// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Mode definitions: reading the JSON (docs/MODE-BUILDER.md) with a message for every problem, the checks of meaning, and writing it back.
#include "dect2/modebuilder/modebuilder.h"
#include "dect2/automation/json_mini.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <functional>
#include <set>

namespace dect2::modebuilder {
namespace aj = dect2::automation::json;

const char* modName(Mod m) {
    switch (m) {
    case Mod::OOK: return "OOK";
    case Mod::ASK: return "ASK";
    case Mod::FSK: return "FSK";
    case Mod::GFSK: return "GFSK";
    case Mod::GMSK: return "GMSK";
    case Mod::PSK: return "PSK";
    }
    return "?";
}
const char* lineName(Line l) {
    switch (l) {
    case Line::NRZ: return "NRZ";
    case Line::NRZI: return "NRZI";
    case Line::Manchester: return "Manchester";
    case Line::ManchesterThomas: return "Manchester-Thomas";
    case Line::DiffManchester: return "Differential-Manchester";
    case Line::Miller: return "Miller";
    case Line::B4B5: return "4b5b";
    case Line::B8B10: return "8b10b";
    }
    return "?";
}
const char* fecName(Fec f) {
    switch (f) {
    case Fec::None: return "none";
    case Fec::Repetition: return "repetition";
    case Fec::Hamming: return "hamming";
    case Fec::Convolutional: return "convolutional";
    case Fec::ReedSolomon: return "reed_solomon";
    }
    return "?";
}
const char* fieldTypeName(FieldType t) {
    switch (t) {
    case FieldType::Uint: return "uint";
    case FieldType::Int: return "int";
    case FieldType::Bool: return "bool";
    case FieldType::Enum: return "enum";
    case FieldType::Bitflags: return "bitflags";
    case FieldType::Ascii: return "ascii";
    case FieldType::Bcd: return "bcd";
    case FieldType::Float32: return "float32";
    case FieldType::Lat: return "lat";
    case FieldType::Lon: return "lon";
    case FieldType::Hex: return "hex";
    }
    return "?";
}

std::vector<uint8_t> BitPattern::toBits() const {
    std::vector<uint8_t> b((size_t)std::max(0, bits));
    for (int i = 0; i < bits; i++) b[(size_t)i] = (uint8_t)((value >> (bits - 1 - i)) & 1);
    return b;
}

int Definition::levels() const {
    if (mod == Mod::OOK || mod == Mod::GMSK) return 2;
    return std::clamp(order, 2, 8);
}
int Definition::bitsPerSymbol() const { const int m = levels(); return m >= 8 ? 3 : m >= 4 ? 2 : 1; }
double Definition::deviation() const {
    if (mod == Mod::GMSK) return symbolRate / 4;
    return deviationHz > 0 ? deviationHz : symbolRate / 2;
}
double Definition::bandwidth() const {
    if (bandwidthHz > 0) return bandwidthHz;
    if (mod == Mod::FSK || mod == Mod::GFSK || mod == Mod::GMSK) return 2 * (deviation() + symbolRate);
    return 2.5 * symbolRate;
}
Pulse Definition::pulseShape() const {
    if (pulseSet) return pulse;
    return mod == Mod::GFSK || mod == Mod::GMSK ? Pulse::Gaussian : Pulse::None;
}
int Definition::crcBytes() const {
    if (crcKind == CrcKind::None) return 0;
    if (crcKind == CrcKind::Sum8 || crcKind == CrcKind::Xor8) return 1;
    return (crc.width + 7) / 8;
}
int Definition::longestFrameBytes() const { return lenMode == LenMode::Fixed ? fixedBytes : maxBytes; }

std::string makeId(const std::string& name) {
    std::string s;
    for (char c : name) {
        const unsigned char u = (unsigned char)c;
        if (std::isalnum(u)) s += (char)std::tolower(u);
        else if (!s.empty() && s.back() != '-') s += '-';
        if (s.size() >= 48) break;
    }
    while (!s.empty() && s.back() == '-') s.pop_back();
    return s.empty() ? "custom-mode" : s;
}

namespace {

std::string lower(std::string s) { for (char& c : s) c = (char)std::tolower((unsigned char)c); return s; }

struct Ctx {
    std::vector<Issue>& out;
    void err(const std::string& w, const std::string& m) { out.push_back({true, w, m}); }
    void warn(const std::string& w, const std::string& m) { out.push_back({false, w, m}); }
    void keys(const aj::Value& o, const std::string& path, std::initializer_list<const char*> known) {
        if (o.type != aj::Value::Obj) return;
        for (const auto& kv : o.o) {
            bool k = false;
            for (const char* n : known) if (kv.first == n) { k = true; break; }
            if (!k) warn(path, "unknown key \"" + kv.first + "\" (ignored)");
        }
    }
};

std::string join(const std::string& a, const std::string& b) { return a.empty() ? b : a + "." + b; }

// a whole number written as a number or a string "0x..", "0b..", "0o..", "123"
bool readUint(const aj::Value& v, uint64_t& out) {
    if (v.type == aj::Value::Num) {
        if (v.n < 0 || v.n > 1.8e19 || std::floor(v.n) != v.n) return false;
        out = (uint64_t)v.n;
        return true;
    }
    if (v.type != aj::Value::Str) return false;
    std::string s;
    for (char c : v.s) if (c != '_' && c != ' ') s += c;
    if (s.empty()) return false;
    int base = 10;
    size_t i = 0;
    if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { base = 16; i = 2; }
    else if (s.size() > 2 && s[0] == '0' && (s[1] == 'b' || s[1] == 'B')) { base = 2; i = 2; }
    else if (s.size() > 2 && s[0] == '0' && (s[1] == 'o' || s[1] == 'O')) { base = 8; i = 2; }
    uint64_t r = 0;
    for (; i < s.size(); i++) {
        const char c = (char)std::tolower((unsigned char)s[i]);
        const int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : 99;
        if (d >= base) return false;
        if (r > (UINT64_MAX - (uint64_t)d) / (uint64_t)base) return false;
        r = r * (uint64_t)base + (uint64_t)d;
    }
    out = r;
    return true;
}

// a bit pattern: "0x2DD4" (4 bits per digit), "0b1011" or "1011" (bits), a number (bits from "bits")
bool readPattern(const aj::Value& v, int bitsKey, BitPattern& p, std::string& why) {
    int natural = 0;
    uint64_t val = 0;
    if (v.type == aj::Value::Str) {
        std::string s;
        for (char c : v.s) if (c != '_' && c != ' ') s += c;
        const bool hex = s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X');
        const bool bin = s.size() > 2 && s[0] == '0' && (s[1] == 'b' || s[1] == 'B');
        const bool bitsOnly = !s.empty() && s.find_first_not_of("01") == std::string::npos;
        if (hex) natural = (int)(s.size() - 2) * 4;
        else if (bin) natural = (int)(s.size() - 2);
        else if (bitsOnly) { natural = (int)s.size(); s = "0b" + s; }
        else { why = "a pattern is \"0x..\" hex, \"0b..\" or \"1011..\" bits, or a number"; return false; }
        if (natural > 64) { why = "at most 64 bits"; return false; }
        aj::Value t = aj::Value::string(s);
        if (!readUint(t, val)) { why = "not a valid pattern"; return false; }
    } else if (v.type == aj::Value::Num) {
        if (!readUint(v, val)) { why = "a whole number"; return false; }
        while (natural < 64 && (val >> natural)) natural++;
        if (bitsKey <= 0) { why = "a number pattern needs \"bits\""; return false; }
    } else { why = "a string or a number"; return false; }
    const int bits = bitsKey > 0 ? bitsKey : natural;
    if (bits < 1 || bits > 64) { why = "1 to 64 bits"; return false; }
    if (bits < 64 && (val >> bits)) { why = "the value has more than " + std::to_string(bits) + " bits"; return false; }
    p.value = val;
    p.bits = bits;
    return true;
}

double num(Ctx& c, const aj::Value& o, const std::string& path, const char* k, double def, double lo, double hi, bool required = false) {
    const aj::Value* v = o.get(k);
    if (!v) { if (required) c.err(join(path, k), "required"); return def; }
    if (v->type != aj::Value::Num) { c.err(join(path, k), "must be a number"); return def; }
    if (!(v->n >= lo && v->n <= hi)) {
        char b[96];
        std::snprintf(b, sizeof b, "must be %g to %g", lo, hi);
        c.err(join(path, k), b);
        return def;
    }
    return v->n;
}
int inum(Ctx& c, const aj::Value& o, const std::string& path, const char* k, int def, int lo, int hi, bool required = false) {
    const aj::Value* v = o.get(k);
    if (!v) { if (required) c.err(join(path, k), "required"); return def; }
    if (v->type != aj::Value::Num || std::floor(v->n) != v->n) { c.err(join(path, k), "must be a whole number"); return def; }
    if (v->n < lo || v->n > hi) { c.err(join(path, k), "must be " + std::to_string(lo) + " to " + std::to_string(hi)); return def; }
    return (int)v->n;
}
bool flag(Ctx& c, const aj::Value& o, const std::string& path, const char* k, bool def) {
    const aj::Value* v = o.get(k);
    if (!v) return def;
    if (v->type != aj::Value::Bool) { c.err(join(path, k), "must be true or false"); return def; }
    return v->b;
}
std::string str(Ctx& c, const aj::Value& o, const std::string& path, const char* k, const std::string& def) {
    const aj::Value* v = o.get(k);
    if (!v) return def;
    if (v->type != aj::Value::Str) { c.err(join(path, k), "must be a string"); return def; }
    return v->s;
}
uint32_t u32(Ctx& c, const aj::Value& o, const std::string& path, const char* k, uint32_t def) {
    const aj::Value* v = o.get(k);
    if (!v) return def;
    uint64_t r = 0;
    if (!readUint(*v, r) || r > 0xFFFFFFFFull) { c.err(join(path, k), "must be a whole number up to 32 bits (\"0x1021\", 4129)"); return def; }
    return (uint32_t)r;
}
const aj::Value* obj(Ctx& c, const aj::Value& root, const std::string& path, const char* k) {
    const aj::Value* v = root.get(k);
    if (!v) return nullptr;
    if (v->type != aj::Value::Obj) { c.err(join(path, k), "must be an object { ... }"); return nullptr; }
    return v;
}

struct CrcPresetRow { const char* name; int w; uint32_t poly, init; bool ri, ro; uint32_t xo; };
const CrcPresetRow kCrcPresets[] = {
    {"CRC-8", 8, 0x07, 0x00, false, false, 0x00},
    {"CRC-8/SMBUS", 8, 0x07, 0x00, false, false, 0x00},
    {"CRC-8/MAXIM", 8, 0x31, 0x00, true, true, 0x00},
    {"CRC-8/MAXIM-DOW", 8, 0x31, 0x00, true, true, 0x00},
    {"CRC-8/AUTOSAR", 8, 0x2F, 0xFF, false, false, 0xFF},
    {"CRC-16/CCITT-FALSE", 16, 0x1021, 0xFFFF, false, false, 0x0000},
    {"CRC-16/IBM-3740", 16, 0x1021, 0xFFFF, false, false, 0x0000},
    {"CRC-16/CCITT", 16, 0x1021, 0x0000, true, true, 0x0000},
    {"CRC-16/KERMIT", 16, 0x1021, 0x0000, true, true, 0x0000},
    {"CRC-16/XMODEM", 16, 0x1021, 0x0000, false, false, 0x0000},
    {"CRC-16/X-25", 16, 0x1021, 0xFFFF, true, true, 0xFFFF},
    {"CRC-16/IBM", 16, 0x8005, 0x0000, true, true, 0x0000},
    {"CRC-16/ARC", 16, 0x8005, 0x0000, true, true, 0x0000},
    {"CRC-16/MODBUS", 16, 0x8005, 0xFFFF, true, true, 0x0000},
    {"CRC-16/DNP", 16, 0x3D65, 0x0000, true, true, 0xFFFF},
    {"CRC-24/OPENPGP", 24, 0x864CFB, 0xB704CE, false, false, 0x000000},
    {"CRC-32", 32, 0x04C11DB7, 0xFFFFFFFF, true, true, 0xFFFFFFFF},
    {"CRC-32/ISO-HDLC", 32, 0x04C11DB7, 0xFFFFFFFF, true, true, 0xFFFFFFFF},
    {"CRC-32C", 32, 0x1EDC6F41, 0xFFFFFFFF, true, true, 0xFFFFFFFF},
    {"CRC-32/BZIP2", 32, 0x04C11DB7, 0xFFFFFFFF, false, false, 0xFFFFFFFF},
};

}   // namespace

bool crcPreset(const std::string& name, CrcModel& out) {
    std::string n;
    for (char c : name) n += (char)std::toupper((unsigned char)c);
    for (const auto& r : kCrcPresets)
        if (n == r.name) { out = CrcModel{r.name, r.w, r.poly, r.init, r.xo, r.ri, r.ro}; return true; }
    return false;
}
std::vector<std::string> crcPresetNames() {
    std::vector<std::string> v;
    for (const auto& r : kCrcPresets) v.push_back(r.name);
    v.push_back("SUM8");
    v.push_back("XOR8");
    return v;
}

ParseResult parseDefinition(const std::string& text) {
    ParseResult r;
    Ctx c{r.issues};
    aj::Value root;
    std::string jerr;
    if (!aj::parse(text, root, jerr)) { c.err("", "JSON: " + jerr); return r; }
    if (root.type != aj::Value::Obj) { c.err("", "the definition must be a JSON object { ... }"); return r; }
    Definition& d = r.def;
    c.keys(root, "", {"onair_mode", "id", "name", "description", "frequency_mhz", "sample_rate", "modulation", "filter", "line_code", "polarity",
                      "bit_order", "preamble", "sync", "scrambler", "frame", "fec", "interleaver", "crc", "fields", "display", "show_bad_frames",
                      "squelch_db", "comment", "source"});
    const aj::Value* ver = root.get("onair_mode");
    if (!ver) c.err("onair_mode", "required: the format version, 1 (\"onair_mode\": 1)");
    else if (ver->type != aj::Value::Num || ver->n < 1 || std::floor(ver->n) != ver->n) c.err("onair_mode", "must be the format version, 1");
    else if (ver->n > kFormatVersion) c.err("onair_mode", "version " + std::to_string((int)ver->n) + " is newer than this OnAir reads (" + std::to_string(kFormatVersion) + "): update OnAir");
    d.name = str(c, root, "", "name", "");
    if (d.name.empty()) c.err("name", "required");
    d.id = str(c, root, "", "id", "");
    if (d.id.empty()) d.id = makeId(d.name);
    else {
        bool okId = d.id.size() <= 48;
        for (char ch : d.id) if (!(std::islower((unsigned char)ch) || std::isdigit((unsigned char)ch) || ch == '-' || ch == '_')) okId = false;
        if (!okId) c.err("id", "only a-z 0-9 - _, at most 48 characters");
    }
    d.description = str(c, root, "", "description", "");
    d.freqMhz = num(c, root, "", "frequency_mhz", 433.92, 0.001, 300000);
    d.sampleRate = num(c, root, "", "sample_rate", 250000, 1000, 100e6);
    d.showBadFrames = flag(c, root, "", "show_bad_frames", false);
    d.squelchDb = num(c, root, "", "squelch_db", 6, 0, 60);
    {
        const std::string p = lower(str(c, root, "", "polarity", "auto"));
        if (p == "normal") d.polarity = Polarity::Normal;
        else if (p == "inverted") d.polarity = Polarity::Inverted;
        else if (p == "auto") d.polarity = Polarity::Auto;
        else c.err("polarity", "normal, inverted or auto");
        const std::string b = lower(str(c, root, "", "bit_order", "msb"));
        if (b == "msb") d.lsbFirst = false;
        else if (b == "lsb") d.lsbFirst = true;
        else c.err("bit_order", "msb or lsb");
    }
    // modulation
    if (const aj::Value* m = obj(c, root, "", "modulation")) {
        const std::string P = "modulation";
        c.keys(*m, P, {"type", "order", "symbol_rate", "deviation_hz", "bt", "differential", "gray", "symbol_map", "offset_hz"});
        const std::string t = lower(str(c, *m, P, "type", ""));
        if (t == "ook") d.mod = Mod::OOK;
        else if (t == "ask") d.mod = Mod::ASK;
        else if (t == "fsk" || t == "2fsk" || t == "4fsk" || t == "8fsk") d.mod = Mod::FSK;
        else if (t == "gfsk") d.mod = Mod::GFSK;
        else if (t == "gmsk" || t == "msk") d.mod = Mod::GMSK;
        else if (t == "psk" || t == "bpsk" || t == "qpsk" || t == "8psk") d.mod = Mod::PSK;
        else c.err(P + ".type", t.empty() ? "required: OOK, ASK, FSK, GFSK, GMSK or PSK" : "\"" + t + "\" is not one of OOK, ASK, FSK, GFSK, GMSK, PSK");
        int defOrder = t == "4fsk" || t == "qpsk" ? 4 : t == "8fsk" || t == "8psk" ? 8 : 2;
        d.order = inum(c, *m, P, "order", defOrder, 2, 8);
        if (d.order != 2 && d.order != 4 && d.order != 8) { c.err(P + ".order", "2, 4 or 8"); d.order = 2; }
        if ((d.mod == Mod::OOK || d.mod == Mod::GMSK) && d.order != 2) c.err(P + ".order", std::string(modName(d.mod)) + " has 2 levels");
        if (d.mod == Mod::ASK && d.order == 8) c.err(P + ".order", "ASK: 2 or 4 levels");
        d.symbolRate = num(c, *m, P, "symbol_rate", 0, 1, 10e6, true);
        d.deviationHz = num(c, *m, P, "deviation_hz", 0, 0, 10e6);
        d.bt = num(c, *m, P, "bt", d.mod == Mod::GMSK ? 0.3 : 0.5, 0.1, 2);
        d.differential = flag(c, *m, P, "differential", false);
        if (d.differential && d.mod != Mod::PSK) c.err(P + ".differential", "only for PSK");
        d.gray = flag(c, *m, P, "gray", true);
        d.offsetHz = num(c, *m, P, "offset_hz", 0, -50e6, 50e6);
        if (const aj::Value* sm = m->get("symbol_map")) {
            if (sm->type != aj::Value::Arr) c.err(P + ".symbol_map", "a list of numbers");
            else {
                for (const auto& e : sm->a) d.symbolMap.push_back(e.type == aj::Value::Num ? (int)e.n : -1);
                const int M = d.levels();
                std::set<int> seen(d.symbolMap.begin(), d.symbolMap.end());
                if ((int)d.symbolMap.size() != M || (int)seen.size() != M || *seen.begin() < 0 || *seen.rbegin() >= M)
                    c.err(P + ".symbol_map", "must list each of 0.." + std::to_string(M - 1) + " once (" + std::to_string(M) + " levels)");
            }
        }
    } else c.err("modulation", "required");
    if (const aj::Value* f = obj(c, root, "", "filter")) {
        const std::string P = "filter";
        c.keys(*f, P, {"bandwidth_hz", "pulse", "rolloff"});
        d.bandwidthHz = num(c, *f, P, "bandwidth_hz", 0, 0, 50e6);
        if (f->get("pulse")) {
            const std::string p = lower(str(c, *f, P, "pulse", "none"));
            d.pulseSet = true;
            if (p == "none" || p == "rect" || p == "rectangular") d.pulse = Pulse::None;
            else if (p == "rrc") d.pulse = Pulse::Rrc;
            else if (p == "rc") d.pulse = Pulse::Rc;
            else if (p == "gaussian") d.pulse = Pulse::Gaussian;
            else { c.err(P + ".pulse", "none, rrc, rc or gaussian"); d.pulseSet = false; }
        }
        d.rolloff = num(c, *f, P, "rolloff", 0.35, 0.05, 1);
    }
    if (const aj::Value* l = root.get("line_code")) {
        const std::string P = "line_code";
        std::string t;
        if (l->type == aj::Value::Str) t = l->s;
        else if (l->type == aj::Value::Obj) {
            c.keys(*l, P, {"type", "toggle_on"});
            t = str(c, *l, P, "type", "NRZ");
            d.nrziToggleOn = inum(c, *l, P, "toggle_on", 0, 0, 1);
        } else c.err(P, "an object { \"type\": ... }");
        t = lower(t);
        if (t == "nrz" || t.empty()) d.line = Line::NRZ;
        else if (t == "nrzi" || t == "nrz-s") d.line = Line::NRZI;
        else if (t == "differential" || t == "nrz-m") { d.line = Line::NRZI; d.nrziToggleOn = 1; }
        else if (t == "manchester" || t == "manchester-ieee") d.line = Line::Manchester;
        else if (t == "manchester-thomas") d.line = Line::ManchesterThomas;
        else if (t == "differential-manchester") d.line = Line::DiffManchester;
        else if (t == "miller") d.line = Line::Miller;
        else if (t == "4b5b") d.line = Line::B4B5;
        else if (t == "8b10b") d.line = Line::B8B10;
        else c.err(P + ".type", "\"" + t + "\" is not NRZ, NRZI, Differential, Manchester, Manchester-Thomas, Differential-Manchester, Miller, 4b5b or 8b10b");
    }
    if (const aj::Value* p = obj(c, root, "", "preamble")) {
        c.keys(*p, "preamble", {"pattern", "bits"});
        const int bits = inum(c, *p, "preamble", "bits", 0, 0, 4096);
        if (const aj::Value* pv = p->get("pattern")) {
            std::string why;
            // a long preamble: the pattern repeats to "bits"
            BitPattern one;
            if (!readPattern(*pv, 0, one, why)) c.err("preamble.pattern", why);
            else {
                d.preamble = one;
                d.preamble.bits = bits > 0 ? bits : one.bits;
                if (d.preamble.bits != one.bits) {   // repeat the pattern
                    const std::vector<uint8_t> pb = one.toBits();
                    uint64_t v = 0;
                    const int nb = std::min(64, d.preamble.bits);
                    for (int i = 0; i < nb; i++) v = (v << 1) | pb[(size_t)i % pb.size()];
                    d.preamble.value = v;
                    d.preamble.bits = nb;
                    if (bits > 64) c.warn("preamble.bits", "the generator sends at most 64 preamble bits");
                }
            }
        } else if (bits > 0) { for (int i = 0; i < std::min(64, bits); i++) d.preamble.value = (d.preamble.value << 1) | (uint64_t)((i + 1) & 1); d.preamble.bits = std::min(64, bits); }
    } else {
        d.preamble.bits = 32;
        d.preamble.value = 0xAAAAAAAAull;
    }
    if (const aj::Value* s = obj(c, root, "", "sync")) {
        c.keys(*s, "sync", {"word", "bits", "max_errors"});
        const int bits = inum(c, *s, "sync", "bits", 0, 0, 64);
        if (const aj::Value* w = s->get("word")) {
            std::string why;
            if (!readPattern(*w, bits, d.sync, why)) c.err("sync.word", why);
        } else c.err("sync.word", "required");
        d.syncMaxErrors = inum(c, *s, "sync", "max_errors", 0, 0, 16);
    } else c.err("sync", "required: { \"word\": \"0x2DD4\", \"bits\": 16 }");
    if (const aj::Value* s = obj(c, root, "", "scrambler")) {
        const std::string P = "scrambler";
        c.keys(*s, P, {"type", "taps", "poly", "seed", "length"});
        const std::string t = lower(str(c, *s, P, "type", "none"));
        if (t == "none") d.scrambler = Scrambler::None;
        else if (t == "additive") d.scrambler = Scrambler::Additive;
        else if (t == "multiplicative" || t == "self-synchronising" || t == "self-synchronizing") d.scrambler = Scrambler::Multiplicative;
        else c.err(P + ".type", "none, additive or multiplicative");
        if (const aj::Value* tp = s->get("taps")) {
            if (tp->type != aj::Value::Arr) c.err(P + ".taps", "a list like [7, 4]");
            else for (const auto& e : tp->a) {
                if (e.type != aj::Value::Num || e.n < 1 || e.n > 32 || std::floor(e.n) != e.n) { c.err(P + ".taps", "taps are 1 to 32"); break; }
                d.taps.push_back((int)e.n);
            }
        } else if (s->get("poly")) {
            const uint32_t poly = u32(c, *s, P, "poly", 0);
            for (int t2 = 32; t2 >= 1; t2--) if (t2 < 32 ? (poly >> t2) & 1 : 0) d.taps.push_back(t2);
            // a poly with bit 32 cannot be written in 32 bits; taps of 1..31
        }
        std::sort(d.taps.begin(), d.taps.end(), std::greater<int>());
        d.taps.erase(std::unique(d.taps.begin(), d.taps.end()), d.taps.end());
        if (d.scrambler != Scrambler::None && d.taps.empty()) c.err(P + ".taps", "required: the feedback taps, e.g. [7, 4] or \"poly\": \"0x90\"");
        d.seed = u32(c, *s, P, "seed", 0xFFFFFFFFu);
    }
    if (const aj::Value* f = obj(c, root, "", "frame")) {
        c.keys(*f, "frame", {"length"});
        const aj::Value* l = f->get("length");
        if (!l || l->type != aj::Value::Obj) c.err("frame.length", "required: { \"mode\": \"fixed\", \"bytes\": 8 }");
        else {
            const std::string P = "frame.length";
            c.keys(*l, P, {"mode", "bytes", "offset_bits", "width_bits", "multiplier", "adjust", "min_bytes", "max_bytes", "flag", "flag_bits", "bit_stuffing"});
            const std::string m = lower(str(c, *l, P, "mode", "fixed"));
            if (m == "fixed") { d.lenMode = LenMode::Fixed; d.fixedBytes = inum(c, *l, P, "bytes", 8, 1, 4096, true); }
            else if (m == "length_field") {
                d.lenMode = LenMode::LengthField;
                d.lenOffsetBits = inum(c, *l, P, "offset_bits", 0, 0, 32768, true);
                d.lenWidthBits = inum(c, *l, P, "width_bits", 8, 1, 24, true);
                d.lenMultiplier = num(c, *l, P, "multiplier", 1, 0.001, 1024);
                d.lenAdjust = inum(c, *l, P, "adjust", 0, -4096, 4096);
                d.minBytes = inum(c, *l, P, "min_bytes", 1, 1, 4096);
                d.maxBytes = inum(c, *l, P, "max_bytes", 255, 1, 4096, true);
            } else if (m == "end_flag") {
                d.lenMode = LenMode::EndFlag;
                const int fb = inum(c, *l, P, "flag_bits", 0, 0, 64);
                if (const aj::Value* fv = l->get("flag")) { std::string why; if (!readPattern(*fv, fb, d.flag, why)) c.err(P + ".flag", why); }
                else c.err(P + ".flag", "required: the end flag pattern, e.g. \"0x7E\"");
                d.maxBytes = inum(c, *l, P, "max_bytes", 255, 1, 4096);
                d.minBytes = inum(c, *l, P, "min_bytes", 1, 1, 4096);
                d.bitStuffing = flag(c, *l, P, "bit_stuffing", false);
            } else c.err(P + ".mode", "fixed, length_field or end_flag");
        }
    } else c.err("frame", "required: { \"length\": { \"mode\": \"fixed\", \"bytes\": 8 } }");
    if (const aj::Value* f = obj(c, root, "", "fec")) {
        const std::string P = "fec";
        c.keys(*f, P, {"type", "n", "k", "code", "polys", "tail", "symbol_bits", "poly", "fcr", "prim"});
        const std::string t = lower(str(c, *f, P, "type", "none"));
        if (t == "none") d.fec = Fec::None;
        else if (t == "repetition") { d.fec = Fec::Repetition; d.repN = inum(c, *f, P, "n", 3, 2, 15); }
        else if (t == "hamming") {
            d.fec = Fec::Hamming;
            std::string code = str(c, *f, P, "code", "7,4");
            code.erase(std::remove(code.begin(), code.end(), ' '), code.end());
            static const struct { const char* s; int n, k; bool ext; } codes[] = {{"7,4", 7, 4, false}, {"15,11", 15, 11, false}, {"31,26", 31, 26, false}, {"8,4", 8, 4, true}, {"16,11", 16, 11, true}};
            bool found = false;
            for (const auto& h : codes) if (code == h.s) { d.hamN = h.n; d.hamK = h.k; d.hamExtended = h.ext; found = true; }
            if (!found) c.err(P + ".code", "\"" + code + "\" is not one of 7,4 15,11 31,26 8,4 16,11");
        } else if (t == "convolutional") {
            d.fec = Fec::Convolutional;
            d.convK = inum(c, *f, P, "k", 7, 3, 9);
            if (const aj::Value* pv = f->get("polys")) {
                d.convPolys.clear();
                if (pv->type != aj::Value::Arr) c.err(P + ".polys", "a list of 2 or 3 generator polynomials");
                else for (const auto& e : pv->a) {
                    uint64_t g = 0;
                    if (!readUint(e, g) || g == 0 || g >= (1ull << d.convK)) { c.err(P + ".polys", "each polynomial is a number below 2^k (\"0x79\", \"0o171\")"); break; }
                    d.convPolys.push_back((uint32_t)g);
                }
                if (d.convPolys.size() != 2 && d.convPolys.size() != 3) { c.err(P + ".polys", "2 (rate 1/2) or 3 (rate 1/3) polynomials"); d.convPolys = {0x79, 0x5B}; }
            } else if (d.convK != 7) c.err(P + ".polys", "required when k is not 7");
            d.convTail = flag(c, *f, P, "tail", true);
        } else if (t == "reed_solomon" || t == "rs" || t == "reed-solomon") {
            d.fec = Fec::ReedSolomon;
            d.rsM = inum(c, *f, P, "symbol_bits", 8, 3, 8);
            d.rsN = inum(c, *f, P, "n", 255, 3, 255, true);
            d.rsK = inum(c, *f, P, "k", 223, 1, 254, true);
            d.rsPoly = u32(c, *f, P, "poly", d.rsM == 8 ? 0x11D : 0);
            d.rsFcr = inum(c, *f, P, "fcr", 0, 0, 255);
            d.rsPrim = inum(c, *f, P, "prim", 1, 1, 255);
            if (d.rsN > (1 << d.rsM) - 1) c.err(P + ".n", "at most 2^symbol_bits - 1 = " + std::to_string((1 << d.rsM) - 1));
            if (d.rsK >= d.rsN) c.err(P + ".k", "must be less than n");
            if (d.rsPoly < (1u << d.rsM) || d.rsPoly >= (2u << d.rsM)) c.err(P + ".poly", "the field polynomial of degree symbol_bits (0x11D for 8 bits)");
            else if (!ReedSolomon(d.rsM, d.rsPoly, d.rsN, std::min(d.rsK, d.rsN - 1), d.rsFcr, d.rsPrim).valid()) c.err(P + ".poly", "not a primitive polynomial (or prim is not coprime with 2^m - 1)");
        } else c.err(P + ".type", "none, repetition, hamming, convolutional or reed_solomon");
    }
    if (const aj::Value* il = obj(c, root, "", "interleaver")) {
        c.keys(*il, "interleaver", {"rows", "cols"});
        d.ilRows = inum(c, *il, "interleaver", "rows", 0, 0, 1024);
        d.ilCols = inum(c, *il, "interleaver", "cols", 0, 0, 1024);
        if ((d.ilRows == 0) != (d.ilCols == 0)) c.err("interleaver", "rows and cols both above 0, or both 0 (none)");
    }
    if (const aj::Value* cr = obj(c, root, "", "crc")) {
        const std::string P = "crc";
        c.keys(*cr, P, {"preset", "width", "poly", "init", "xorout", "reflect_in", "reflect_out", "from_byte", "to_byte", "position", "byte_order"});
        const std::string pre = str(c, *cr, P, "preset", "");
        const std::string pu = lower(pre);
        d.crcKind = CrcKind::Crc;
        if (pu == "none") d.crcKind = CrcKind::None;
        else if (pu == "sum8") d.crcKind = CrcKind::Sum8;
        else if (pu == "xor8") d.crcKind = CrcKind::Xor8;
        else if (!pre.empty()) { if (!crcPreset(pre, d.crc)) c.err(P + ".preset", "\"" + pre + "\" is not a known model (CRC-16/CCITT-FALSE, CRC-16/IBM, CRC-32, ...)"); }
        else if (!cr->get("width") || !cr->get("poly")) c.err(P, "a \"preset\", or \"width\" and \"poly\"");
        if (d.crcKind == CrcKind::Crc) {
            const bool custom = cr->get("width") || cr->get("poly") || cr->get("init") || cr->get("xorout") || cr->get("reflect_in") || cr->get("reflect_out");
            d.crc.width = inum(c, *cr, P, "width", d.crc.width, 1, 32);
            const uint32_t mask = d.crc.width >= 32 ? 0xFFFFFFFFu : (1u << d.crc.width) - 1;
            d.crc.poly = u32(c, *cr, P, "poly", d.crc.poly) & mask;
            d.crc.init = u32(c, *cr, P, "init", pre.empty() ? 0 : d.crc.init) & mask;
            d.crc.xorout = u32(c, *cr, P, "xorout", pre.empty() ? 0 : d.crc.xorout) & mask;
            d.crc.refin = flag(c, *cr, P, "reflect_in", d.crc.refin);
            d.crc.refout = flag(c, *cr, P, "reflect_out", d.crc.refout);
            if (custom && !pre.empty()) d.crc.name = pre + " (changed)";
            if (d.crc.poly == 0) c.err(P + ".poly", "must not be 0");
        }
        d.crcFrom = inum(c, *cr, P, "from_byte", 0, 0, 4096);
        if (cr->get("to_byte")) { d.crcTo = inum(c, *cr, P, "to_byte", 0, -4096, 4096); d.crcToSet = true; }
        if (const aj::Value* pv = cr->get("position")) {
            if (pv->type == aj::Value::Str && lower(pv->s) == "end") d.crcPosition = -1;
            else d.crcPosition = inum(c, *cr, P, "position", -1, 0, 4096);
        }
        const std::string bo = lower(str(c, *cr, P, "byte_order", "big"));
        if (bo == "big") d.crcLittle = false;
        else if (bo == "little") d.crcLittle = true;
        else c.err(P + ".byte_order", "big or little");
    }
    if (const aj::Value* fl = root.get("fields")) {
        if (fl->type != aj::Value::Arr) c.err("fields", "a list of fields [ {...}, ... ]");
        else for (size_t i = 0; i < fl->a.size(); i++) {
            const aj::Value& e = fl->a[i];
            const std::string P = "fields[" + std::to_string(i) + "]";
            if (e.type != aj::Value::Obj) { c.err(P, "must be an object"); continue; }
            c.keys(e, P, {"name", "offset_bits", "width_bits", "type", "endian", "scale", "offset", "units", "enum", "flags", "format", "comment"});
            Field f;
            f.name = str(c, e, P, "name", "");
            bool okName = !f.name.empty() && f.name.size() <= 40 && f.name[0] != '_';
            for (char ch : f.name) if (!(std::isalnum((unsigned char)ch) || ch == '_')) okName = false;
            if (!okName) c.err(P + ".name", "required: letters, digits and _ (not starting with _)");
            f.offsetBits = inum(c, e, P, "offset_bits", 0, 0, 32768, true);
            const std::string t = lower(str(c, e, P, "type", "uint"));
            static const struct { const char* s; FieldType t; } types[] = {{"uint", FieldType::Uint}, {"int", FieldType::Int}, {"bool", FieldType::Bool}, {"enum", FieldType::Enum},
                {"bitflags", FieldType::Bitflags}, {"ascii", FieldType::Ascii}, {"bcd", FieldType::Bcd}, {"float32", FieldType::Float32}, {"lat", FieldType::Lat}, {"lon", FieldType::Lon}, {"hex", FieldType::Hex}};
            bool found = false;
            for (const auto& ty : types) if (t == ty.s) { f.type = ty.t; found = true; }
            if (!found) c.err(P + ".type", "\"" + t + "\" is not uint, int, bool, enum, bitflags, ascii, bcd, float32, lat, lon or hex");
            const bool longType = f.type == FieldType::Ascii || f.type == FieldType::Hex;
            f.widthBits = inum(c, e, P, "width_bits", f.type == FieldType::Float32 ? 32 : f.type == FieldType::Bool ? 1 : 8, 1, longType ? 32768 : 64);
            if (longType && f.widthBits % 8) c.err(P + ".width_bits", "ascii and hex fields are whole bytes (a multiple of 8)");
            if (f.type == FieldType::Float32 && f.widthBits != 32) c.err(P + ".width_bits", "float32 is 32 bits");
            if (f.type == FieldType::Bcd && f.widthBits % 4) c.err(P + ".width_bits", "BCD is 4 bits per digit");
            const std::string en = lower(str(c, e, P, "endian", "big"));
            if (en == "little") f.little = true;
            else if (en != "big") c.err(P + ".endian", "big or little");
            if (f.little && (f.widthBits % 8 || f.offsetBits % 8)) c.err(P + ".endian", "little-endian fields must be whole bytes on a byte boundary");
            f.scale = num(c, e, P, "scale", 1, -1e12, 1e12);
            f.offset = num(c, e, P, "offset", 0, -1e15, 1e15);
            f.units = str(c, e, P, "units", "");
            f.format = str(c, e, P, "format", "");
            if (!f.format.empty() && f.format != "dec" && f.format != "hex") {
                const std::string& s = f.format;
                const bool okF = s.size() >= 3 && s.size() <= 6 && s[0] == '%' && s.back() == 'f' && s.find_first_not_of("%.0123456789f") == std::string::npos;
                if (!okF) c.err(P + ".format", "dec, hex, or \"%.2f\"");
            }
            if (const aj::Value* et = e.get("enum")) {
                if (et->type != aj::Value::Obj) c.err(P + ".enum", "an object { \"0\": \"off\", \"1\": \"on\" }");
                else for (const auto& kv : et->o) {
                    aj::Value kvv = aj::Value::string(kv.first);
                    uint64_t k = 0;
                    if (!readUint(kvv, k) || kv.second.type != aj::Value::Str) { c.err(P + ".enum", "keys are numbers (\"0\", \"0x1F\") and values strings"); break; }
                    f.enumTable.push_back({(int64_t)k, kv.second.s});
                }
                std::sort(f.enumTable.begin(), f.enumTable.end());
            } else if (f.type == FieldType::Enum) c.err(P + ".enum", "required for type enum");
            if (const aj::Value* ft = e.get("flags")) {
                if (ft->type != aj::Value::Obj) c.err(P + ".flags", "an object { \"0\": \"battery low\" }");
                else for (const auto& kv : ft->o) {
                    aj::Value kvv = aj::Value::string(kv.first);
                    uint64_t k = 0;
                    if (!readUint(kvv, k) || k >= 64 || kv.second.type != aj::Value::Str) { c.err(P + ".flags", "keys are bit numbers 0..63 and values strings"); break; }
                    f.flags.push_back({(int)k, kv.second.s});
                }
                std::sort(f.flags.begin(), f.flags.end());
            } else if (f.type == FieldType::Bitflags) c.err(P + ".flags", "required for type bitflags");
            d.fields.push_back(std::move(f));
        }
    }
    if (const aj::Value* ds = root.get("display")) {
        if (ds->type == aj::Value::Str) d.display.push_back({"", 0, ds->s});
        else if (ds->type == aj::Value::Arr) {
            for (size_t i = 0; i < ds->a.size(); i++) {
                const aj::Value& e = ds->a[i];
                const std::string P = "display[" + std::to_string(i) + "]";
                if (e.type == aj::Value::Str) { d.display.push_back({"", 0, e.s}); continue; }
                if (e.type != aj::Value::Obj) { c.err(P, "a template string or { \"when\": ..., \"template\": ... }"); continue; }
                c.keys(e, P, {"when", "template"});
                DisplayRule rule;
                rule.tmpl = str(c, e, P, "template", "");
                if (const aj::Value* w = obj(c, e, P, "when")) {
                    rule.whenField = str(c, *w, P + ".when", "field", "");
                    const aj::Value* eq = w->get("equals");
                    if (!eq || eq->type != aj::Value::Num) c.err(P + ".when.equals", "required, a number");
                    else rule.equals = eq->n;
                    if (rule.whenField.empty()) c.err(P + ".when.field", "required");
                }
                d.display.push_back(rule);
            }
        } else c.err("display", "a template string, or a list of them");
    }
    if (r.issues.empty() || std::none_of(r.issues.begin(), r.issues.end(), [](const Issue& i) { return i.error; })) {
        for (Issue& i : validate(d)) r.issues.push_back(i);
    }
    r.ok = std::none_of(r.issues.begin(), r.issues.end(), [](const Issue& i) { return i.error; });
    return r;
}

std::vector<Issue> validate(const Definition& d) {
    std::vector<Issue> out;
    Ctx c{out};
    if (d.symbolRate <= 0) c.err("modulation.symbol_rate", "required, a number above 0");
    if (d.sync.bits < 1) c.err("sync.word", "required");
    if (d.syncMaxErrors * 4 > d.sync.bits) c.err("sync.max_errors", "at most a quarter of the sync word's bits (" + std::to_string(d.sync.bits / 4) + ")");
    if (d.sync.bits > 0 && d.sync.bits < 8) c.warn("sync.word", "a sync word under 8 bits is found by chance in noise very often");
    const double bw = d.bandwidth();
    if (d.sampleRate < bw * 1.1) c.err("sample_rate", "must be above the filter bandwidth (" + std::to_string((int)bw) + " Hz)");
    if (d.sampleRate < 4 * d.symbolRate) c.err("sample_rate", "must be at least 4 x the symbol rate");
    if (std::fabs(d.offsetHz) + bw / 2 > d.sampleRate / 2) c.err("modulation.offset_hz", "the signal does not fit in the sample rate");
    if ((d.mod == Mod::FSK || d.mod == Mod::GFSK) && d.deviation() * 2 > bw) c.warn("filter.bandwidth_hz", "narrower than the FSK tones (2 x deviation): the outer tones are cut");
    if (d.pulseShape() == Pulse::Gaussian && d.mod == Mod::PSK) c.err("filter.pulse", "gaussian is for FSK; PSK takes none, rrc or rc");
    if ((d.pulseShape() == Pulse::Rrc || d.pulseShape() == Pulse::Rc) && (d.mod == Mod::FSK || d.mod == Mod::GFSK || d.mod == Mod::GMSK)) c.err("filter.pulse", "rrc and rc are for PSK and ASK; FSK takes none or gaussian");
    if (d.line == Line::B8B10 && ((d.sync.bits % 8) || (d.preamble.bits % 8))) c.err("line_code", "8b10b codes whole bytes: the preamble and sync word must be multiples of 8 bits");
    if (d.line == Line::B4B5 && ((d.sync.bits % 4) || (d.preamble.bits % 4))) c.err("line_code", "4b5b codes nibbles: the preamble and sync word must be multiples of 4 bits");
    for (int t : d.taps) if (t > 32) c.err("scrambler.taps", "at most 32");
    const int maxB = d.longestFrameBytes();
    if (d.lenMode == LenMode::LengthField) {
        if (d.minBytes > d.maxBytes) c.err("frame.length.min_bytes", "more than max_bytes");
        if (d.lenOffsetBits + d.lenWidthBits > d.maxBytes * 8) c.err("frame.length.offset_bits", "the length field is outside the longest frame");
    }
    if (d.lenMode == LenMode::EndFlag) {
        if (d.fec != Fec::None) c.err("fec", "FEC cannot be used with an end flag (the length is not known before decoding)");
        if (d.ilRows > 0) c.err("interleaver", "an interleaver cannot be used with an end flag");
        if (d.flag.bits < 4) c.err("frame.length.flag", "at least 4 bits");
    }
    if (d.fec == Fec::ReedSolomon && d.rsK >= d.rsN) c.err("fec.k", "must be less than n");
    if (d.fec == Fec::Repetition && d.repN % 2 == 0) c.warn("fec.n", "an even count cannot break a tie: ties are read as 0");
    const int cb = d.crcBytes();
    if (cb > 0) {
        const int minB = d.lenMode == LenMode::Fixed ? d.fixedBytes : d.minBytes;
        if (cb > minB) c.err("crc", "the check value (" + std::to_string(cb) + " bytes) does not fit in the shortest frame (" + std::to_string(minB) + " bytes)");
        if (d.crcPosition >= 0 && d.crcPosition + cb > minB) c.err("crc.position", "outside the shortest frame");
        if (d.crcFrom >= minB) c.err("crc.from_byte", "outside the shortest frame");
    }
    for (size_t i = 0; i < d.fields.size(); i++) {
        const Field& f = d.fields[i];
        const std::string P = "fields[" + std::to_string(i) + "]";
        if (f.offsetBits + f.widthBits > maxB * 8) c.err(P, "\"" + f.name + "\" ends at bit " + std::to_string(f.offsetBits + f.widthBits) + ", after the longest frame (" + std::to_string(maxB * 8) + " bits)");
        for (size_t j = 0; j < i; j++) if (d.fields[j].name == f.name) c.err(P + ".name", "\"" + f.name + "\" is used twice");
    }
    for (size_t i = 0; i < d.display.size(); i++) {
        const std::string& t = d.display[i].tmpl;
        size_t p = 0;
        while ((p = t.find('{', p)) != std::string::npos) {
            const size_t e = t.find('}', p);
            if (e == std::string::npos) { c.err("display", "a { without }"); break; }
            std::string key = t.substr(p + 1, e - p - 1);
            const size_t dot = key.find('.');
            if (dot != std::string::npos) key = key.substr(0, dot);
            if (key != "_crc" && key != "_len" && key != "_hex" && std::none_of(d.fields.begin(), d.fields.end(), [&](const Field& f) { return f.name == key; }))
                c.warn("display", "{" + key + "} is not a field");
            p = e + 1;
        }
        if (!d.display[i].whenField.empty() && std::none_of(d.fields.begin(), d.fields.end(), [&](const Field& f) { return f.name == d.display[i].whenField; }))
            c.err("display[" + std::to_string(i) + "].when.field", "\"" + d.display[i].whenField + "\" is not a field");
    }
    return out;
}

// ---------------------------------------------------------------- writing
namespace {
std::string q(const std::string& s) { return "\"" + aj::escape(s) + "\""; }
std::string hexOf(uint64_t v, int digits) { char b[32]; std::snprintf(b, sizeof b, "\"0x%0*llX\"", digits, (unsigned long long)v); return b; }
std::string numStr(double v) {
    char b[48];
    if (std::floor(v) == v && std::fabs(v) < 1e15) std::snprintf(b, sizeof b, "%.0f", v);
    else std::snprintf(b, sizeof b, "%.10g", v);
    return b;
}
std::string pattern(const BitPattern& p) {
    if (p.bits % 4 == 0) return hexOf(p.value, p.bits / 4);
    std::string s = "\"";
    for (uint8_t b : p.toBits()) s += (char)('0' + b);
    return s + "\"";
}
}   // namespace

bool parsePattern(const std::string& text, int bits, BitPattern& out, std::string& err) {
    return readPattern(aj::Value::string(text), bits, out, err);
}
std::string patternText(const BitPattern& p) {
    const std::string q = pattern(p);
    return q.size() >= 2 ? q.substr(1, q.size() - 2) : q;
}

std::string toJson(const Definition& d) {
    std::string o = "{\n";
    auto line = [&](const std::string& k, const std::string& v, bool comma = true) { o += "  " + q(k) + ": " + v + (comma ? ",\n" : "\n"); };
    line("onair_mode", std::to_string(kFormatVersion));
    line("id", q(d.id));
    line("name", q(d.name));
    if (!d.description.empty()) line("description", q(d.description));
    line("frequency_mhz", numStr(d.freqMhz));
    line("sample_rate", numStr(d.sampleRate));
    {
        std::string m = "{ \"type\": " + q(modName(d.mod));
        if (d.mod == Mod::FSK || d.mod == Mod::PSK || d.mod == Mod::ASK || d.mod == Mod::GFSK) m += ", \"order\": " + std::to_string(d.order);
        m += ", \"symbol_rate\": " + numStr(d.symbolRate);
        if ((d.mod == Mod::FSK || d.mod == Mod::GFSK) && d.deviationHz > 0) m += ", \"deviation_hz\": " + numStr(d.deviationHz);
        if (d.mod == Mod::GFSK || d.mod == Mod::GMSK) m += ", \"bt\": " + numStr(d.bt);
        if (d.differential) m += ", \"differential\": true";
        if (!d.gray) m += ", \"gray\": false";
        if (!d.symbolMap.empty()) { m += ", \"symbol_map\": ["; for (size_t i = 0; i < d.symbolMap.size(); i++) m += (i ? ", " : "") + std::to_string(d.symbolMap[i]); m += "]"; }
        if (d.offsetHz != 0) m += ", \"offset_hz\": " + numStr(d.offsetHz);
        line("modulation", m + " }");
    }
    {
        std::string f = "{ \"bandwidth_hz\": " + numStr(d.bandwidthHz);
        if (d.pulseSet) {
            static const char* pn[] = {"none", "rrc", "rc", "gaussian"};
            f += ", \"pulse\": " + q(pn[(int)d.pulse]);
            if (d.pulse == Pulse::Rrc || d.pulse == Pulse::Rc) f += ", \"rolloff\": " + numStr(d.rolloff);
        }
        line("filter", f + " }");
    }
    {
        std::string l = "{ \"type\": " + q(d.line == Line::NRZI && d.nrziToggleOn ? "Differential" : lineName(d.line)) + " }";
        line("line_code", l);
    }
    line("polarity", q(d.polarity == Polarity::Normal ? "normal" : d.polarity == Polarity::Inverted ? "inverted" : "auto"));
    line("bit_order", q(d.lsbFirst ? "lsb" : "msb"));
    if (d.preamble.bits > 0) line("preamble", "{ \"pattern\": " + pattern(d.preamble) + ", \"bits\": " + std::to_string(d.preamble.bits) + " }");
    line("sync", "{ \"word\": " + pattern(d.sync) + ", \"bits\": " + std::to_string(d.sync.bits) + ", \"max_errors\": " + std::to_string(d.syncMaxErrors) + " }");
    if (d.scrambler != Scrambler::None) {
        std::string s = "{ \"type\": " + q(d.scrambler == Scrambler::Additive ? "additive" : "multiplicative") + ", \"taps\": [";
        for (size_t i = 0; i < d.taps.size(); i++) s += (i ? ", " : "") + std::to_string(d.taps[i]);
        s += "], \"seed\": " + hexOf(d.seed, 0) + " }";
        line("scrambler", s);
    } else line("scrambler", "{ \"type\": \"none\" }");
    {
        std::string f;
        if (d.lenMode == LenMode::Fixed) f = "{ \"mode\": \"fixed\", \"bytes\": " + std::to_string(d.fixedBytes) + " }";
        else if (d.lenMode == LenMode::LengthField) {
            f = "{ \"mode\": \"length_field\", \"offset_bits\": " + std::to_string(d.lenOffsetBits) + ", \"width_bits\": " + std::to_string(d.lenWidthBits);
            if (d.lenMultiplier != 1) f += ", \"multiplier\": " + numStr(d.lenMultiplier);
            f += ", \"adjust\": " + std::to_string(d.lenAdjust) + ", \"min_bytes\": " + std::to_string(d.minBytes) + ", \"max_bytes\": " + std::to_string(d.maxBytes) + " }";
        } else {
            f = "{ \"mode\": \"end_flag\", \"flag\": " + pattern(d.flag) + ", \"flag_bits\": " + std::to_string(d.flag.bits) + ", \"min_bytes\": " + std::to_string(d.minBytes) + ", \"max_bytes\": " + std::to_string(d.maxBytes);
            if (d.bitStuffing) f += ", \"bit_stuffing\": true";
            f += " }";
        }
        line("frame", "{ \"length\": " + f + " }");
    }
    {
        std::string f = "{ \"type\": " + q(fecName(d.fec));
        if (d.fec == Fec::Repetition) f += ", \"n\": " + std::to_string(d.repN);
        if (d.fec == Fec::Hamming) f += ", \"code\": \"" + std::to_string(d.hamN) + "," + std::to_string(d.hamK) + "\"";
        if (d.fec == Fec::Convolutional) {
            f += ", \"k\": " + std::to_string(d.convK) + ", \"polys\": [";
            for (size_t i = 0; i < d.convPolys.size(); i++) f += (i ? ", " : "") + hexOf(d.convPolys[i], 2);
            f += std::string("], \"tail\": ") + (d.convTail ? "true" : "false");
        }
        if (d.fec == Fec::ReedSolomon) f += ", \"n\": " + std::to_string(d.rsN) + ", \"k\": " + std::to_string(d.rsK) + ", \"symbol_bits\": " + std::to_string(d.rsM) + ", \"poly\": " + hexOf(d.rsPoly, 0) + ", \"fcr\": " + std::to_string(d.rsFcr) + ", \"prim\": " + std::to_string(d.rsPrim);
        line("fec", f + " }");
    }
    line("interleaver", "{ \"rows\": " + std::to_string(d.ilRows) + ", \"cols\": " + std::to_string(d.ilCols) + " }");
    if (d.crcKind != CrcKind::None) {
        std::string c = "{ ";
        if (d.crcKind == CrcKind::Sum8) c += "\"preset\": \"SUM8\"";
        else if (d.crcKind == CrcKind::Xor8) c += "\"preset\": \"XOR8\"";
        else {
            CrcModel pm;
            if (!d.crc.name.empty() && crcPreset(d.crc.name, pm) && pm.width == d.crc.width && pm.poly == d.crc.poly && pm.init == d.crc.init && pm.xorout == d.crc.xorout && pm.refin == d.crc.refin && pm.refout == d.crc.refout)
                c += "\"preset\": " + q(d.crc.name);
            else {
                const int dg = (d.crc.width + 3) / 4;
                c += "\"width\": " + std::to_string(d.crc.width) + ", \"poly\": " + hexOf(d.crc.poly, dg) + ", \"init\": " + hexOf(d.crc.init, dg) + ", \"xorout\": " + hexOf(d.crc.xorout, dg) +
                     ", \"reflect_in\": " + (d.crc.refin ? "true" : "false") + ", \"reflect_out\": " + (d.crc.refout ? "true" : "false");
            }
        }
        c += ", \"from_byte\": " + std::to_string(d.crcFrom);
        if (d.crcToSet) c += ", \"to_byte\": " + std::to_string(d.crcTo);
        if (d.crcPosition >= 0) c += ", \"position\": " + std::to_string(d.crcPosition);
        if (d.crcLittle) c += ", \"byte_order\": \"little\"";
        line("crc", c + " }");
    }
    if (d.showBadFrames) line("show_bad_frames", "true");
    if (d.squelchDb != 6) line("squelch_db", numStr(d.squelchDb));
    o += "  \"fields\": [";
    for (size_t i = 0; i < d.fields.size(); i++) {
        const Field& f = d.fields[i];
        o += i ? ",\n    " : "\n    ";
        o += "{ \"name\": " + q(f.name) + ", \"offset_bits\": " + std::to_string(f.offsetBits) + ", \"width_bits\": " + std::to_string(f.widthBits) + ", \"type\": " + q(fieldTypeName(f.type));
        if (f.little) o += ", \"endian\": \"little\"";
        if (f.scale != 1) o += ", \"scale\": " + numStr(f.scale);
        if (f.offset != 0) o += ", \"offset\": " + numStr(f.offset);
        if (!f.units.empty()) o += ", \"units\": " + q(f.units);
        if (!f.format.empty()) o += ", \"format\": " + q(f.format);
        if (!f.enumTable.empty()) {
            o += ", \"enum\": {";
            for (size_t k = 0; k < f.enumTable.size(); k++) o += (k ? ", " : " ") + q(std::to_string(f.enumTable[k].first)) + ": " + q(f.enumTable[k].second);
            o += " }";
        }
        if (!f.flags.empty()) {
            o += ", \"flags\": {";
            for (size_t k = 0; k < f.flags.size(); k++) o += (k ? ", " : " ") + q(std::to_string(f.flags[k].first)) + ": " + q(f.flags[k].second);
            o += " }";
        }
        o += " }";
    }
    o += d.fields.empty() ? "],\n" : "\n  ],\n";
    if (d.display.size() == 1 && d.display[0].whenField.empty()) o += "  \"display\": " + q(d.display[0].tmpl) + "\n";
    else {
        o += "  \"display\": [";
        for (size_t i = 0; i < d.display.size(); i++) {
            const DisplayRule& r = d.display[i];
            o += i ? ",\n    " : "\n    ";
            if (r.whenField.empty()) o += "{ \"template\": " + q(r.tmpl) + " }";
            else o += "{ \"when\": { \"field\": " + q(r.whenField) + ", \"equals\": " + numStr(r.equals) + " }, \"template\": " + q(r.tmpl) + " }";
        }
        o += d.display.empty() ? "]\n" : "\n  ]\n";
    }
    return o + "}\n";
}

}   // namespace dect2::modebuilder
