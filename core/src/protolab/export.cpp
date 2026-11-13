// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Protocol lab: exports. The protocol definition is a small JSON document (readable by people and by our own tools); the rtl_433 line is a
// best-effort starting point for a "flex" decoder, not a tested decoder; the bit text has one packet per line.
#include "dect2/protolab/protolab.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace dect2::protolab {

namespace {
std::string esc(const std::string& s) {
    std::string o;
    for (char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += c; }
        else if ((unsigned char)c < 0x20) o += ' ';
        else o += c;
    }
    return o;
}
std::string bitStr(const std::vector<uint8_t>& b) { std::string s; for (uint8_t x : b) s += (char)('0' + (x & 1)); return s; }
std::string num(double v) { char t[48]; snprintf(t, sizeof t, "%.6g", v); return t; }
const char* kindName(FieldKind k) {
    switch (k) {
        case FieldKind::Constant: return "constant";
        case FieldKind::Counter: return "counter";
        case FieldKind::Checksum: return "checksum";
        case FieldKind::Enum: return "enum";
        case FieldKind::Varying: return "varying";
    }
    return "?";
}
}   // namespace

std::string protocolJson(const Candidate& c, const FieldReport& f, const std::string& name, double fs) {
    std::string j = "{\n";
    j += "  \"format\": \"onair-protocol-lab\",\n  \"version\": 1,\n";
    j += "  \"name\": \"" + esc(name) + "\",\n";
    j += "  \"modulation\": \"" + esc(demodName(c.demod)) + "\",\n";
    j += "  \"symbol_rate_baud\": " + num(c.baud) + ",\n";
    j += "  \"levels\": " + std::to_string(c.levels) + ",\n";
    j += "  \"inverted\": " + std::string(c.invert ? "true" : "false") + ",\n";
    j += "  \"line_coding\": \"" + esc(codingName(c.coding)) + "\",\n";
    j += "  \"byte_order\": \"" + std::string(c.lsbFirst ? "lsb_first" : "msb_first") + "\",\n";
    j += "  \"sample_rate_hz\": " + num(fs) + ",\n";
    j += "  \"preamble\": { \"bits\": " + std::to_string(c.sync.preambleBits) + ", \"pattern\": \"" + bitStr(c.sync.preamblePattern) + "\" },\n";
    j += "  \"sync_word_bits\": \"" + bitStr(c.sync.sync) + "\",\n";
    j += "  \"whitening\": " + (c.whitening.empty() ? std::string("null") : "\"" + esc(c.whitening) + "\"") + ",\n";
    size_t mn = 0, mx = 0;
    if (!c.packets.empty()) { mn = mx = c.packets[0].size(); for (auto& p : c.packets) { mn = std::min(mn, p.size()); mx = std::max(mx, p.size()); } }
    j += "  \"length_bytes\": { \"min\": " + std::to_string(mn) + ", \"max\": " + std::to_string(mx) + ", \"fixed\": " + (mn == mx ? "true" : "false") + ", \"counts_from_sync_start\": true },\n";
    int bi = -1;
    for (size_t i = 0; i < c.crc.size(); i++) if (c.crc[i].total >= 3 && c.crc[i].matches * 10 >= c.crc[i].total * 6) { bi = (int)i; break; }
    if (bi < 0) j += "  \"crc\": null,\n";
    else {
        const CrcHit& h = c.crc[(size_t)bi];
        char t[64];
        j += "  \"crc\": { \"name\": \"" + esc(h.name) + "\", \"is_checksum\": " + (h.isChecksum ? "true" : "false") + ", \"width\": " + std::to_string(h.width);
        snprintf(t, sizeof t, "\"0x%X\"", h.poly); j += ", \"poly\": " + std::string(t);
        snprintf(t, sizeof t, "\"0x%X\"", h.init); j += ", \"init\": " + std::string(t);
        snprintf(t, sizeof t, "\"0x%X\"", h.xorout); j += ", \"xorout\": " + std::string(t);
        j += std::string(", \"refin\": ") + (h.refin ? "true" : "false") + ", \"refout\": " + (h.refout ? "true" : "false");
        j += std::string(", \"value_big_endian\": ") + (h.crcBigEndian ? "true" : "false") + ", \"first_covered_byte\": " + std::to_string(h.dataStart);
        j += ", \"bytes_after_crc\": " + std::to_string(h.endSlack) + ", \"verified_packets\": " + std::to_string(h.matches) + ", \"of\": " + std::to_string(h.total) + " },\n";
    }
    j += "  \"fields\": [";
    bool first = true;
    for (size_t i = 0; i < f.cols.size();) {
        size_t k = i;
        while (k + 1 < f.cols.size() && f.cols[k + 1].kind == f.cols[i].kind && f.cols[i].kind != FieldKind::Counter && f.cols[i].kind != FieldKind::Enum) k++;
        if (!first) j += ",";
        first = false;
        j += "\n    { \"offset_bytes\": " + std::to_string(i) + ", \"length_bytes\": " + std::to_string(k - i + 1) + ", \"kind\": \"" + kindName(f.cols[i].kind) + "\"";
        if (f.cols[i].kind == FieldKind::Constant) {
            char t[16];
            std::string v;
            for (size_t q = i; q <= k; q++) { snprintf(t, sizeof t, "%02X", f.cols[q].constantValue); v += t; }
            j += ", \"value_hex\": \"" + v + "\"";
        }
        if (f.cols[i].kind == FieldKind::Counter) j += ", \"step\": " + std::to_string(f.cols[i].counterStep);
        j += ", \"entropy_bits_per_byte\": " + num(f.cols[i].entropy) + " }";
        i = k + 1;
    }
    j += "\n  ],\n";
    j += "  \"notes\": \"Made by the OnAir Protocol lab from captured packets. Field kinds are guesses from the packets seen; nothing is decrypted.\"\n}\n";
    return j;
}

std::string rtl433Spec(const Candidate& c, const std::string& name) {
    if (c.demod == Demod::FSK4 || c.demod == Demod::PSK2 || c.demod == Demod::PSK4 || c.coding == Coding::Hdlc || c.coding == Coding::NRZI || c.coding == Coding::DiffManchester)
        return "# rtl_433 flex decoders cover plain two-level signals (PCM, PWM, Manchester); this one (" + std::string(demodName(c.demod)) + ", " + codingName(c.coding) +
               ") needs a C decoder: use the protocol JSON as the description.\n";
    const bool ook = c.demod == Demod::OOK || c.demod == Demod::AM;
    const bool man = c.coding == Coding::ManchesterIEEE || c.coding == Coding::ManchesterThomas;
    const double symUs = 1e6 / (c.baud > 0 ? c.baud : 1);
    std::string pre;
    if (!c.sync.sync.empty()) {
        std::vector<uint8_t> b = c.sync.sync;
        while (b.size() % 4) b.push_back(0);
        char t[8], hex[16] = {0};
        for (size_t i = 0; i < b.size(); i += 4) { snprintf(t, sizeof t, "%X", b[i] * 8 + b[i + 1] * 4 + b[i + 2] * 2 + b[i + 3]); strncat(hex, t, 1); }
        pre = ",preamble={" + std::to_string(c.sync.sync.size()) + "}" + hex;
    }
    char buf[256];
    std::string safe;
    for (char ch : name) safe += (ch == ',' || ch == ' ') ? '_' : ch;
    snprintf(buf, sizeof buf, "-X \"n=%s,m=%s,s=%.0f,l=%.0f,r=%.0f%s%s\"", safe.c_str(),
             man ? (ook ? "OOK_MC_ZEROBIT" : "FSK_MC_ZEROBIT") : (ook ? "OOK_PCM" : "FSK_PCM"),
             symUs, symUs, std::max(1000.0, symUs * 20), pre.c_str(),
             (c.invert || c.coding == Coding::ManchesterThomas) ? ",invert" : "");
    std::string s = buf;
    s += "\n# Best effort: s and l are the symbol length in microseconds, r the gap that ends a packet. Check the option names against the rtl_433 flex decoder\n"
         "# documentation of your version; nothing here has been run against rtl_433.\n";
    return s;
}

std::string packetText(const Candidate& c) {
    std::string s;
    for (const auto& p : c.packets) {
        for (uint8_t v : p) for (int k = 7; k >= 0; k--) s += (char)('0' + ((v >> k) & 1));
        s += '\n';
    }
    return s;
}

}   // namespace dect2::protolab
