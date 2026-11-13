// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Mode Builder frames: decoding the bits after the sync word into a frame (descramble, deinterleave, FEC, length), the check value,
// the fields with their units, the display template, and the CSV / JSON export.
#include "dect2/modebuilder/modebuilder.h"
#include "dect2/automation/json_mini.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace dect2::modebuilder {
namespace aj = dect2::automation::json;

namespace {

uint64_t getBits(const std::vector<uint8_t>& b, int off, int w, bool& ok) {
    ok = off >= 0 && w >= 1 && (size_t)(off + w) <= b.size() * 8;
    if (!ok) return 0;
    uint64_t v = 0;
    for (int i = 0; i < w; i++) {
        const int p = off + i;
        v = (v << 1) | (uint64_t)((b[(size_t)(p >> 3)] >> (7 - (p & 7))) & 1);
    }
    return v;
}

void crcGeometry(const Definition& d, size_t len, int& pos, int& from, int& to) {
    const int cb = d.crcBytes();
    pos = d.crcPosition >= 0 ? d.crcPosition : (int)len - cb;
    from = d.crcFrom;
    if (!d.crcToSet || d.crcTo == 0) to = pos;
    else to = d.crcTo > 0 ? d.crcTo : pos + d.crcTo;
    to = std::clamp(to, 0, (int)len);
    from = std::clamp(from, 0, to);
}

uint32_t crcOver(const Definition& d, const uint8_t* p, size_t n) {
    if (d.crcKind == CrcKind::Sum8) { unsigned s = 0; for (size_t i = 0; i < n; i++) s += p[i]; return s & 0xFF; }
    if (d.crcKind == CrcKind::Xor8) { unsigned s = 0; for (size_t i = 0; i < n; i++) s ^= p[i]; return s & 0xFF; }
    return crcCompute(d.crc, p, n);
}

std::string fmtNum(double v, const std::string& f) {
    char b[64];
    if (f.size() >= 3 && f[0] == '%') std::snprintf(b, sizeof b, f.c_str(), v);
    else if (std::floor(v) == v && std::fabs(v) < 1e15) std::snprintf(b, sizeof b, "%.0f", v);
    else std::snprintf(b, sizeof b, "%.6g", v);
    return b;
}

std::string hexBytes(const std::vector<uint8_t>& b, size_t from = 0, size_t n = (size_t)-1) {
    static const char* hx = "0123456789ABCDEF";
    std::string s;
    for (size_t i = from; i < b.size() && i - from < n; i++) { s += hx[b[i] >> 4]; s += hx[b[i] & 15]; }
    return s;
}

}   // namespace

void checkCrc(const Definition& d, const std::vector<uint8_t>& bytes, bool& present, bool& ok) {
    present = d.crcKind != CrcKind::None;
    ok = true;
    if (!present) return;
    const int cb = d.crcBytes();
    int pos, from, to;
    crcGeometry(d, bytes.size(), pos, from, to);
    if (pos < 0 || (size_t)(pos + cb) > bytes.size()) { ok = false; return; }
    uint32_t got = 0;
    for (int i = 0; i < cb; i++) {
        const uint8_t v = bytes[(size_t)(pos + (d.crcLittle ? cb - 1 - i : i))];
        got = (got << 8) | v;
    }
    ok = got == crcOver(d, bytes.data() + from, (size_t)(to - from));
}

void fillCrc(const Definition& d, std::vector<uint8_t>& bytes) {
    if (d.crcKind == CrcKind::None) return;
    const int cb = d.crcBytes();
    int pos, from, to;
    crcGeometry(d, bytes.size(), pos, from, to);
    if (pos < 0 || (size_t)(pos + cb) > bytes.size()) return;
    const uint32_t v = crcOver(d, bytes.data() + from, (size_t)(to - from));
    for (int i = 0; i < cb; i++) {
        const uint8_t byte = (uint8_t)(v >> (8 * (cb - 1 - i)));
        bytes[(size_t)(pos + (d.crcLittle ? cb - 1 - i : i))] = byte;
    }
}

bool decodeFrameBits(const Definition& d, const std::vector<uint8_t>& bits, size_t start, bool final, Frame& out, size_t& used) {
    out = Frame{};
    used = 0;
    auto decodeLen = [&](int bytes, Frame& f, size_t& u) -> bool {
        const size_t dataBits = (size_t)bytes * 8;
        const size_t cb = stage::codedBits(d, dataBits);
        if (start + cb > bits.size() && !final) return false;
        std::vector<uint8_t> chunk(cb, 0);
        for (size_t i = 0; i < cb && start + i < bits.size(); i++) chunk[i] = bits[start + i];
        stage::scramble(chunk, d, false);
        stage::interleave(chunk, d.ilRows, d.ilCols, false);
        std::vector<uint8_t> data;
        int corr = 0;
        const bool ok = stage::fecDecode(d, chunk, dataBits, data, corr);
        f.bytes = stage::bitsToBytes(data, d.lsbFirst);
        f.fecCorrected = corr;
        f.fecFailed = !ok;
        u = cb;
        return true;
    };
    if (d.lenMode == LenMode::Fixed) {
        if (!decodeLen(d.fixedBytes, out, used)) return false;
    } else if (d.lenMode == LenMode::LengthField) {
        Frame first;
        size_t u1 = 0;
        if (!decodeLen(d.maxBytes, first, u1)) return false;
        bool ok = false;
        const uint64_t v = getBits(first.bytes, d.lenOffsetBits, d.lenWidthBits, ok);
        const int L = std::clamp((int)std::lround((double)v * d.lenMultiplier) + d.lenAdjust, d.minBytes, d.maxBytes);
        if (L == d.maxBytes) { out = first; used = u1; }
        else if (!decodeLen(L, out, used)) return false;
    } else {
        const size_t maxRaw = (size_t)d.maxBytes * 8 + (d.bitStuffing ? (size_t)d.maxBytes * 8 / 5 + 1 : 0) + (size_t)d.flag.bits * 9;
        if (start + maxRaw > bits.size() && !final) return false;
        std::vector<uint8_t> raw(bits.begin() + (std::ptrdiff_t)std::min(start, bits.size()), bits.begin() + (std::ptrdiff_t)std::min(bits.size(), start + maxRaw));
        stage::scramble(raw, d, false);
        const std::vector<uint8_t> fl = d.flag.toBits();
        std::vector<uint8_t> body;
        int run = 0;
        bool found = false;
        size_t i0 = 0;   // more flags (HDLC sends several): the frame starts after the last
        while (i0 + fl.size() <= raw.size() && std::equal(fl.begin(), fl.end(), raw.begin() + (std::ptrdiff_t)i0)) i0 += fl.size();
        for (size_t i = i0; i + fl.size() <= raw.size(); i++) {
            if (body.size() >= (size_t)d.minBytes * 8 && std::equal(fl.begin(), fl.end(), raw.begin() + (std::ptrdiff_t)i)) { found = true; used = i + fl.size(); break; }
            const uint8_t b = raw[i] & 1;
            if (d.bitStuffing && run == 5) { run = 0; if (b == 0) continue; }   // the stuffed 0 (a 1 here is an abort, kept)
            body.push_back(b);
            run = b ? run + 1 : 0;
            if (body.size() > (size_t)d.maxBytes * 8) break;
        }
        if (!found) { used = 1; out.fecFailed = true; return true; }
        body.resize(body.size() / 8 * 8);
        out.bytes = stage::bitsToBytes(body, d.lsbFirst);
    }
    checkCrc(d, out.bytes, out.hasCrc, out.crcOk);
    interpretFrame(d, out);
    return true;
}

void interpretFrame(const Definition& d, Frame& f) {
    f.fields.clear();
    for (const Field& fd : d.fields) {
        FieldValue v;
        v.name = fd.name;
        v.units = fd.units;
        bool ok = true;
        if (fd.type == FieldType::Ascii || fd.type == FieldType::Hex) {
            std::vector<uint8_t> bs;
            for (int i = 0; i < fd.widthBits / 8 && ok; i++) bs.push_back((uint8_t)getBits(f.bytes, fd.offsetBits + i * 8, 8, ok));
            if (!ok) { f.fields.push_back(v); continue; }
            if (fd.type == FieldType::Hex) v.text = hexBytes(bs);
            else {
                for (uint8_t c : bs) v.text += c >= 32 && c < 127 ? (char)c : c == 0 ? '\0' : '.';
                while (!v.text.empty() && (v.text.back() == '\0' || v.text.back() == ' ')) v.text.pop_back();
                v.text.erase(std::remove(v.text.begin(), v.text.end(), '\0'), v.text.end());
            }
            f.fields.push_back(v);
            continue;
        }
        uint64_t raw = 0;
        if (fd.little) {
            for (int i = fd.widthBits / 8 - 1; i >= 0 && ok; i--) raw = (raw << 8) | getBits(f.bytes, fd.offsetBits + i * 8, 8, ok);
        } else raw = getBits(f.bytes, fd.offsetBits, fd.widthBits, ok);
        if (!ok) { f.fields.push_back(v); continue; }
        int64_t sraw = (int64_t)raw;
        const bool sign = fd.type == FieldType::Int || fd.type == FieldType::Lat || fd.type == FieldType::Lon;
        if (sign && fd.widthBits < 64 && (raw >> (fd.widthBits - 1)) & 1) sraw = (int64_t)(raw | (~0ull << fd.widthBits));
        v.raw = sign ? sraw : (int64_t)raw;
        switch (fd.type) {
        case FieldType::Uint:
        case FieldType::Int: {
            v.numeric = true;
            v.num = (double)v.raw * fd.scale + fd.offset;
            if (fd.format == "hex") { char b[32]; std::snprintf(b, sizeof b, "0x%0*llX", (fd.widthBits + 3) / 4, (unsigned long long)raw); v.text = b; }
            else v.text = fmtNum(v.num, fd.format);
            break;
        }
        case FieldType::Bool: v.numeric = true; v.num = raw ? 1 : 0; v.text = raw ? "true" : "false"; break;
        case FieldType::Enum: {
            v.numeric = true;
            v.num = (double)raw;
            v.text = "unknown (" + std::to_string(raw) + ")";
            for (const auto& e : fd.enumTable) if (e.first == (int64_t)raw) v.text = e.second;
            break;
        }
        case FieldType::Bitflags: {
            v.numeric = true;
            v.num = (double)raw;
            for (int b = 0; b < fd.widthBits; b++) {
                if (!((raw >> b) & 1)) continue;
                std::string n = "bit " + std::to_string(b);
                for (const auto& fl : fd.flags) if (fl.first == b) n = fl.second;
                v.text += (v.text.empty() ? "" : ", ") + n;
            }
            if (v.text.empty()) v.text = "-";
            break;
        }
        case FieldType::Bcd: {
            double n = 0;
            bool valid = true;
            for (int k = fd.widthBits / 4 - 1; k >= 0; k--) {
                const int dg = (int)((raw >> (k * 4)) & 15);
                if (dg > 9) valid = false;
                v.text += dg > 9 ? '?' : (char)('0' + dg);
                n = n * 10 + dg;
            }
            if (valid) {
                v.numeric = true;
                v.num = n * fd.scale + fd.offset;
                if (fd.scale != 1 || fd.offset != 0 || !fd.format.empty()) v.text = fmtNum(v.num, fd.format);
            }
            break;
        }
        case FieldType::Float32: {
            const uint32_t u = (uint32_t)raw;
            float x;
            std::memcpy(&x, &u, 4);
            v.numeric = std::isfinite(x);
            v.num = (double)x * fd.scale + fd.offset;
            v.text = v.numeric ? fmtNum(v.num, fd.format.empty() ? "%.6g" : fd.format) : "NaN";
            break;
        }
        case FieldType::Lat:
        case FieldType::Lon: {
            v.numeric = true;
            v.num = (double)sraw * fd.scale + fd.offset;
            char b[48];
            const bool lat = fd.type == FieldType::Lat;
            std::snprintf(b, sizeof b, "%.5f %c", std::fabs(v.num), lat ? (v.num < 0 ? 'S' : 'N') : (v.num < 0 ? 'W' : 'E'));
            v.text = b;
            break;
        }
        default: break;
        }
        f.fields.push_back(v);
    }
    // display
    const std::string* tmpl = nullptr;
    for (const DisplayRule& r : d.display) {
        if (r.whenField.empty()) { tmpl = &r.tmpl; break; }
        const auto it = std::find_if(f.fields.begin(), f.fields.end(), [&](const FieldValue& x) { return x.name == r.whenField; });
        if (it != f.fields.end() && it->numeric && std::fabs((double)it->raw - r.equals) < 1e-9) { tmpl = &r.tmpl; break; }
    }
    f.display.clear();
    if (!tmpl) {
        for (const FieldValue& v : f.fields) {
            if (v.text.empty()) continue;
            if (!f.display.empty()) f.display += "  ";
            f.display += v.name + "=" + v.text + (v.units.empty() ? "" : " " + v.units);
        }
        return;
    }
    const std::string& t = *tmpl;
    for (size_t i = 0; i < t.size(); i++) {
        if (t[i] != '{') { f.display += t[i]; continue; }
        const size_t e = t.find('}', i);
        if (e == std::string::npos) { f.display += t.substr(i); break; }
        std::string key = t.substr(i + 1, e - i - 1), part;
        const size_t dot = key.find('.');
        if (dot != std::string::npos) { part = key.substr(dot + 1); key = key.substr(0, dot); }
        i = e;
        if (key == "_crc") { f.display += !f.hasCrc ? "-" : f.crcOk ? "ok" : "bad"; continue; }
        if (key == "_len") { f.display += std::to_string(f.bytes.size()); continue; }
        if (key == "_hex") { f.display += hexBytes(f.bytes); continue; }
        const auto it = std::find_if(f.fields.begin(), f.fields.end(), [&](const FieldValue& x) { return x.name == key; });
        if (it == f.fields.end()) { f.display += "{" + key + "}"; continue; }
        if (part == "units") f.display += it->units;
        else if (part == "raw") f.display += std::to_string(it->raw);
        else f.display += it->text;
    }
}

std::string framesCsv(const Definition& d, const std::vector<Frame>& frames) {
    auto cell = [](const std::string& s) {
        if (s.find_first_of(",\"\n") == std::string::npos) return s;
        std::string o = "\"";
        for (char c : s) { if (c == '"') o += '"'; o += c; }
        return o + "\"";
    };
    std::string o = "time_s,crc,sync_errors,fec_corrected,snr_db";
    for (const Field& f : d.fields) o += "," + cell(f.units.empty() ? f.name : f.name + " (" + f.units + ")");
    o += ",display,hex\n";
    for (const Frame& fr : frames) {
        char b[160];
        std::snprintf(b, sizeof b, "%.6f,%s,%d,%d,%.1f", fr.timeSec, !fr.hasCrc ? "" : fr.crcOk ? "ok" : "bad", fr.syncErrors, fr.fecCorrected, fr.snrDb);
        o += b;
        for (size_t i = 0; i < d.fields.size(); i++) {
            const FieldValue* v = i < fr.fields.size() ? &fr.fields[i] : nullptr;
            o += ",";
            if (!v) continue;
            o += cell(v->numeric && d.fields[i].type != FieldType::Enum && d.fields[i].type != FieldType::Bitflags && d.fields[i].type != FieldType::Bool ? fmtNum(v->num, d.fields[i].format) : v->text);
        }
        o += "," + cell(fr.display) + "," + hexBytes(fr.bytes) + "\n";
    }
    return o;
}

std::string framesJson(const Definition& d, const std::vector<Frame>& frames) {
    std::string o = "{\n  \"mode\": \"" + aj::escape(d.id) + "\",\n  \"frames\": [";
    for (size_t k = 0; k < frames.size(); k++) {
        const Frame& fr = frames[k];
        char b[200];
        std::snprintf(b, sizeof b, "%s\n    { \"time_s\": %.6f, \"crc\": %s, \"sync_errors\": %d, \"fec_corrected\": %d, \"snr_db\": %.1f, \"hex\": \"", k ? "," : "", fr.timeSec,
                      !fr.hasCrc ? "null" : fr.crcOk ? "true" : "false", fr.syncErrors, fr.fecCorrected, fr.snrDb);
        o += b + hexBytes(fr.bytes) + "\", \"fields\": {";
        bool first = true;
        for (const FieldValue& v : fr.fields) {
            o += first ? " " : ", ";
            first = false;
            o += "\"" + aj::escape(v.name) + "\": ";
            const bool asNum = v.numeric && std::isfinite(v.num) && v.text.find_first_not_of("-0123456789.eE+") == std::string::npos && !v.text.empty();
            o += asNum ? fmtNum(v.num, "%.10g") : "\"" + aj::escape(v.text) + "\"";
        }
        o += " }, \"display\": \"" + aj::escape(fr.display) + "\" }";
    }
    return o + (frames.empty() ? "]\n}\n" : "\n  ]\n}\n");
}

}   // namespace dect2::modebuilder
