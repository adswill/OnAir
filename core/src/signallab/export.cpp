// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Signal Lab exports: the signal description JSON shared with Mode Builder (format in docs/SIGNAL-LAB.md), CSV, hex dump, the
// saved analysis (description + frames + notes) and a small JSON reader for loading it back.
#include "dect2/signallab/signallab.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace dect2::signallab {

std::string bitsToString(const Bits& b) { std::string s(b.size(), '0'); for (size_t i = 0; i < b.size(); i++) s[i] = (char)('0' + (b[i] & 1)); return s; }
Bits stringToBits(const std::string& s) { Bits b; for (char c : s) if (c == '0' || c == '1') b.push_back((uint8_t)(c - '0')); return b; }
std::string bitsToHex(const Bits& b) {
    std::string o;
    static const char* hx = "0123456789ABCDEF";
    for (size_t i = 0; i < b.size(); i += 4) {
        int v = 0;
        for (size_t k = 0; k < 4; k++) v = (v << 1) | (i + k < b.size() ? b[i + k] : 0);
        o += hx[v];
    }
    return o;
}

namespace {
std::string esc(const std::string& s) {
    std::string o = "\"";
    for (unsigned char c : s) {
        switch (c) {
            case '"': o += "\\\""; break; case '\\': o += "\\\\"; break; case '\n': o += "\\n"; break; case '\r': o += "\\r"; break; case '\t': o += "\\t"; break;
            default: if (c < 0x20) { char t[8]; snprintf(t, sizeof t, "\\u%04x", c); o += t; } else o += (char)c;
        }
    }
    return o + "\"";
}
std::string num(double v) { char t[40]; snprintf(t, sizeof t, "%.10g", std::isfinite(v) ? v : 0.0); return t; }
std::string hex(uint32_t v, int w) { char t[16]; snprintf(t, sizeof t, "0x%0*X", std::max(2, (w + 3) / 4), v); return t; }
const char* lineId(LineCode c, Mod m) {
    switch (c) {
        case LineCode::NRZ: return "nrz";
        case LineCode::NRZI: return m == Mod::PSK ? "differential" : "nrzi";
        case LineCode::Differential: return "differential";
        case LineCode::Manchester: return "manchester";
        case LineCode::ManchesterInv: return "manchester-inverted";
        case LineCode::DiffManchester: return "differential-manchester";
        case LineCode::Miller: return "miller";
    }
    return "nrz";
}
LineCode lineFromId(const std::string& s) {
    if (s == "nrzi") return LineCode::NRZI;
    if (s == "differential") return LineCode::Differential;
    if (s == "manchester") return LineCode::Manchester;
    if (s == "manchester-inverted") return LineCode::ManchesterInv;
    if (s == "differential-manchester") return LineCode::DiffManchester;
    if (s == "miller") return LineCode::Miller;
    return LineCode::NRZ;
}
Mod modFromName(const std::string& s) {
    for (Mod m : {Mod::OOK, Mod::ASK, Mod::FSK, Mod::MSK, Mod::GMSK, Mod::PSK, Mod::QAM, Mod::OFDM}) if (s == modName(m)) return m;
    return Mod::Unknown;
}

void description(std::ostringstream& o, const Analysis& a, const char* ind) {
    const ModGuess g = a.mod.best();
    const double conf = a.usedMod == g.mod ? g.confidence : 0;
    o << ind << "\"format\": \"onair-signal-description\",\n" << ind << "\"version\": 1,\n";
    o << ind << "\"name\": " << esc(a.name) << ",\n" << ind << "\"source\": \"signal-lab\",\n";
    o << ind << "\"modulation\": {\"family\": " << esc(modName(a.usedMod)) << ", \"order\": " << a.usedOrder << ", \"symbol_rate\": " << num(a.rate.baud)
      << ", \"bandwidth_hz\": " << num(a.channel.bw99Hz) << ", \"carrier_offset_hz\": " << num(a.channel.offsetHz);
    if (a.usedMod == Mod::FSK || a.usedMod == Mod::MSK || a.usedMod == Mod::GMSK)
        o << ", \"deviation_hz\": " << num(a.mod.deviationHz) << ", \"tone_spacing_hz\": " << num(a.mod.toneSpacingHz) << ", \"modulation_index\": " << num(a.mod.modIndex);
    o << ", \"gray\": true, \"confidence\": " << num(conf) << ", \"rate_confidence\": " << num(a.rate.confidence) << "},\n";
    o << ind << "\"bursts\": {\"continuous\": " << (a.channel.continuous ? "true" : "false") << ", \"duty_cycle\": " << num(a.channel.dutyCycle)
      << ", \"period_s\": " << num(a.channel.burstPeriodSec) << ", \"mean_length_s\": " << num(a.channel.meanBurstSec) << "},\n";
    o << ind << "\"line_code\": " << esc(lineId(a.line.code, a.usedMod)) << ",\n";
    o << ind << "\"polarity\": " << esc(a.line.inverted ? "inverted" : "normal") << ",\n";
    if (a.line.scrambler >= 0) {
        const Scrambler& s = scramblers()[(size_t)a.line.scrambler];
        o << ind << "\"scrambler\": {\"name\": " << esc(s.name) << ", \"taps\": " << esc(hex(s.taps, s.degree)) << ", \"degree\": " << s.degree
          << ", \"type\": " << esc(s.additive ? "additive" : "multiplicative") << ", \"seed\": " << esc(hex((1u << s.degree) - 1, s.degree))
          << ", \"applies_to\": " << esc(s.additive ? "frame" : "stream") << "},\n";
    } else o << ind << "\"scrambler\": null,\n";
    const bool lsb = !a.checks.empty() && a.checks[0].lsbFirst;
    o << ind << "\"bit_order\": " << esc(lsb ? "lsb-first" : "msb-first") << ",\n";
    o << ind << "\"preamble\": {\"bits\": " << a.frame.preambleBits << ", \"pattern\": \"10\"},\n";
    if (!a.frame.sync.pattern.empty())
        o << ind << "\"sync_word\": {\"bits\": " << a.frame.sync.pattern.size() << ", \"hex\": " << esc(bitsToHex(a.frame.sync.pattern)) << ", \"binary\": " << esc(bitsToString(a.frame.sync.pattern))
          << ", \"max_errors\": " << a.frame.sync.pattern.size() / 12 << "},\n";
    else o << ind << "\"sync_word\": null,\n";
    o << ind << "\"frame\": {\"length_bits\": " << a.frame.frameBits << ", \"variable_length\": " << (a.frame.variableLength ? "true" : "false") << ", \"counts_from\": \"after-sync\"},\n";
    o << ind << "\"fields\": [";
    for (size_t i = 0; i < a.fields.size(); i++) {
        const Field& f = a.fields[i];
        o << (i ? ",\n" : "\n") << ind << "  {\"name\": " << esc(f.name) << ", \"start_bit\": " << f.start << ", \"length_bits\": " << f.length << ", \"type\": " << esc(fieldTypeName(f.type))
          << ", \"example\": " << esc(f.value) << "}";
    }
    o << (a.fields.empty() ? "" : "\n") << (a.fields.empty() ? "" : ind) << "],\n";
    if (!a.checks.empty()) {
        const ChecksumHit& h = a.checks[0];
        const CheckAlgo& c = checkAlgos()[(size_t)h.algo];
        const char* kind = c.kind == CheckKind::CRC ? "crc" : c.kind == CheckKind::Fletcher16 ? "fletcher16" : c.kind == CheckKind::Sum8 ? "sum8" : c.kind == CheckKind::Xor8 ? "xor8" : "sum16";
        o << ind << "\"checksum\": {\"algorithm\": " << esc(c.name) << ", \"kind\": " << esc(kind) << ", \"width\": " << c.width;
        if (c.kind == CheckKind::CRC)
            o << ", \"poly\": " << esc(hex(c.poly, c.width)) << ", \"init\": " << esc(hex(c.init, c.width)) << ", \"refin\": " << (c.refin ? "true" : "false")
              << ", \"refout\": " << (c.refout ? "true" : "false") << ", \"xorout\": " << esc(hex(c.xorout, c.width));
        o << ", \"covers_sync\": " << (h.coversSync ? "true" : "false") << ", \"covers_from_bit\": " << h.coverStart;
        if (h.lengthFromEnd) o << ", \"from_end\": true, \"tail_bits\": " << h.tailBits;
        else o << ", \"from_end\": false, \"position_bit\": " << h.coverEnd;
        o << ", \"byte_order\": " << esc(h.littleEndian ? "little-endian" : "big-endian") << ", \"bit_order\": " << esc(h.lsbFirst ? "lsb-first" : "msb-first")
          << ", \"inverted\": " << (h.inverted ? "true" : "false") << ", \"matched\": " << h.matched << ", \"tested\": " << h.tested << "},\n";
    } else o << ind << "\"checksum\": null,\n";
    o << ind << "\"notes\": " << esc(a.notes);
}
}

std::string toModeJson(const Analysis& a) {
    std::ostringstream o;
    o << "{\n";
    description(o, a, "  ");
    o << "\n}\n";
    return o.str();
}

std::string saveAnalysisJson(const Analysis& a) {
    std::ostringstream o;
    o << "{\n";
    description(o, a, "  ");
    o << ",\n  \"analysis\": {\n    \"sample_rate\": " << num(a.fs) << ",\n    \"frames\": [";
    for (size_t i = 0; i < a.frame.frames.size(); i++) o << (i ? ",\n      " : "\n      ") << esc(bitsToString(a.frame.frames[i]));
    o << "\n    ],\n    \"streams\": [";
    size_t tot = 0;
    for (size_t i = 0; i < a.bits.size() && tot < 4000000; i++) { o << (i ? ",\n      " : "\n      ") << esc(bitsToString(a.bits[i])); tot += a.bits[i].size(); }
    o << "\n    ],\n    \"log\": [";
    for (size_t i = 0; i < a.log.size(); i++) o << (i ? ",\n      " : "\n      ") << esc(a.log[i]);
    o << "\n    ]\n  }\n}\n";
    return o.str();
}

std::string toCsv(const Analysis& a) {
    std::ostringstream o;
    o << "frame,stream,start_bit,length_bits,bits_hex";
    for (const Field& f : a.fields) o << "," << f.name << "@" << f.start;
    o << "\n";
    for (size_t i = 0; i < a.frame.frames.size(); i++) {
        const Bits& b = a.frame.frames[i];
        o << i << "," << (i < a.frame.frameStream.size() ? a.frame.frameStream[i] : 0) << "," << (i < a.frame.frameStart.size() ? a.frame.frameStart[i] : 0) << "," << b.size() << "," << bitsToHex(b);
        for (const Field& f : a.fields) {
            std::string v = f.start + f.length > b.size() ? "" : f.type == FieldType::Ascii ? interpretAscii(b, f.start, f.length)
                          : f.length <= 32 ? interpretDecimal(b, f.start, f.length) : interpretHex(b, f.start, f.length);
            if (v.find_first_of(",\"") != std::string::npos) { std::string q = "\""; for (char c : v) { if (c == '"') q += '"'; q += c; } v = q + "\""; }
            o << "," << v;
        }
        o << "\n";
    }
    return o.str();
}

std::string toHexDump(const Analysis& a) {
    std::ostringstream o;
    char t[32];
    for (size_t i = 0; i < a.frame.frames.size(); i++) {
        const Bits& b = a.frame.frames[i];
        snprintf(t, sizeof t, "%04zu  ", i);
        o << t << interpretHex(b, 0, b.size()) << "  |" << interpretAscii(b, 0, b.size() - b.size() % 8) << "|\n";
    }
    return o.str();
}

// ---------------------------------------------------------------- JSON reader
namespace {
const Json& nullJson() { static const Json n; return n; }
struct P {
    const std::string& s; size_t i = 0; std::string err;
    void ws() { while (i < s.size() && (s[i] == ' ' || s[i] == '\n' || s[i] == '\r' || s[i] == '\t')) i++; }
    bool fail(const char* m) { if (err.empty()) { char t[80]; snprintf(t, sizeof t, "%s at %zu", m, i); err = t; } return false; }
    bool str(std::string& o) {
        if (s[i] != '"') return fail("expected a string");
        i++;
        while (i < s.size() && s[i] != '"') {
            char c = s[i++];
            if (c == '\\' && i < s.size()) {
                const char e = s[i++];
                switch (e) {
                    case 'n': o += '\n'; break; case 't': o += '\t'; break; case 'r': o += '\r'; break; case 'b': o += '\b'; break; case 'f': o += '\f'; break;
                    case 'u': {
                        if (i + 4 > s.size()) return fail("bad escape");
                        const unsigned cp = (unsigned)strtoul(s.substr(i, 4).c_str(), nullptr, 16);
                        i += 4;
                        if (cp < 0x80) o += (char)cp;
                        else if (cp < 0x800) { o += (char)(0xC0 | (cp >> 6)); o += (char)(0x80 | (cp & 0x3F)); }
                        else { o += (char)(0xE0 | (cp >> 12)); o += (char)(0x80 | ((cp >> 6) & 0x3F)); o += (char)(0x80 | (cp & 0x3F)); }
                        break;
                    }
                    default: o += e;
                }
            } else o += c;
        }
        if (i >= s.size()) return fail("unterminated string");
        i++;
        return true;
    }
    bool val(Json& j, int depth) {
        if (depth > 64) return fail("too deep");
        ws();
        if (i >= s.size()) return fail("unexpected end");
        const char c = s[i];
        if (c == '{') {
            j.t = Json::Obj; i++; ws();
            if (i < s.size() && s[i] == '}') { i++; return true; }
            for (;;) {
                ws();
                std::string k;
                if (i >= s.size() || !str(k)) return fail("expected a key");
                ws();
                if (i >= s.size() || s[i] != ':') return fail("expected ':'");
                i++;
                if (!val(j.o[k], depth + 1)) return false;
                ws();
                if (i < s.size() && s[i] == ',') { i++; continue; }
                if (i < s.size() && s[i] == '}') { i++; return true; }
                return fail("expected ',' or '}'");
            }
        }
        if (c == '[') {
            j.t = Json::Arr; i++; ws();
            if (i < s.size() && s[i] == ']') { i++; return true; }
            for (;;) {
                j.a.emplace_back();
                if (!val(j.a.back(), depth + 1)) return false;
                ws();
                if (i < s.size() && s[i] == ',') { i++; continue; }
                if (i < s.size() && s[i] == ']') { i++; return true; }
                return fail("expected ',' or ']'");
            }
        }
        if (c == '"') { j.t = Json::Str; return str(j.s); }
        if (s.compare(i, 4, "true") == 0) { j.t = Json::Bool; j.b = true; i += 4; return true; }
        if (s.compare(i, 5, "false") == 0) { j.t = Json::Bool; j.b = false; i += 5; return true; }
        if (s.compare(i, 4, "null") == 0) { j.t = Json::Null; i += 4; return true; }
        char* e = nullptr;
        const double v = strtod(s.c_str() + i, &e);
        if (e == s.c_str() + i) return fail("unexpected character");
        j.t = Json::Num; j.n = v; i = (size_t)(e - s.c_str());
        return true;
    }
};
}

const Json& Json::operator[](const std::string& k) const {
    if (t != Obj) return nullJson();
    auto it = o.find(k);
    return it == o.end() ? nullJson() : it->second;
}
const Json& Json::operator[](size_t i) const { return t == Arr && i < a.size() ? a[i] : nullJson(); }

bool parseJson(const std::string& text, Json& out, std::string& err) {
    P p{text, 0, {}};
    out = Json();
    if (!p.val(out, 0)) { err = p.err; return false; }
    p.ws();
    if (p.i != text.size()) { err = "trailing characters"; return false; }
    return true;
}

bool loadAnalysisJson(const std::string& text, Analysis& a, std::string& err) {
    Json j;
    if (!parseJson(text, j, err)) return false;
    if (j["format"].str() != "onair-signal-description") { err = "not a Signal Lab description"; return false; }
    a = Analysis();
    a.name = j["name"].str("unknown signal");
    a.notes = j["notes"].str();
    const Json& m = j["modulation"];
    a.usedMod = modFromName(m["family"].str());
    a.usedOrder = (int)m["order"].num(2);
    a.rate.baud = m["symbol_rate"].num();
    a.rate.confidence = m["rate_confidence"].num();
    a.channel.bw99Hz = m["bandwidth_hz"].num();
    a.channel.offsetHz = m["carrier_offset_hz"].num();
    a.mod.deviationHz = m["deviation_hz"].num();
    a.mod.toneSpacingHz = m["tone_spacing_hz"].num();
    a.mod.modIndex = m["modulation_index"].num();
    a.mod.ranked.push_back({a.usedMod, a.usedOrder, m["confidence"].num(), "loaded"});
    a.line.code = lineFromId(j["line_code"].str("nrz"));
    a.line.inverted = j["polarity"].str() == "inverted";
    if (j["scrambler"].t == Json::Obj) {
        const std::string n = j["scrambler"]["name"].str();
        for (size_t i = 0; i < scramblers().size(); i++) if (n == scramblers()[i].name) a.line.scrambler = (int)i;
    }
    a.frame.preambleBits = (size_t)j["preamble"]["bits"].num();
    a.frame.sync.pattern = stringToBits(j["sync_word"]["binary"].str());
    a.frame.frameBits = (size_t)j["frame"]["length_bits"].num();
    a.frame.variableLength = j["frame"]["variable_length"].t == Json::Bool && j["frame"]["variable_length"].b;
    const Json& an = j["analysis"];
    a.fs = an["sample_rate"].num();
    for (const Json& f : an["frames"].a) { a.frame.frames.push_back(stringToBits(f.str())); a.frame.frameStream.push_back(0); a.frame.frameStart.push_back(0); }
    for (const Json& f : an["streams"].a) a.bits.push_back(stringToBits(f.str()));
    for (const Json& f : an["log"].a) a.log.push_back(f.str());
    a.checks = findChecksums(a.frame.frames, a.frame.sync.pattern);
    a.fields.clear();
    for (const Json& f : j["fields"].a) {
        Field fd; fd.name = f["name"].str(); fd.start = (size_t)f["start_bit"].num(); fd.length = (size_t)f["length_bits"].num(); fd.value = f["example"].str();
        const std::string t = f["type"].str();
        for (FieldType ft : {FieldType::Constant, FieldType::Counter, FieldType::Checksum, FieldType::Ascii, FieldType::Enum, FieldType::Value, FieldType::Bytes}) if (t == fieldTypeName(ft)) fd.type = ft;
        a.fields.push_back(fd);
    }
    a.columns = columnStats(a.frame.frames);
    return true;
}

} // namespace dect2::signallab
