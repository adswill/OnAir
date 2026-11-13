// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Meteosat HRDCP message format and coding (see hrdcp_proto.h). Written from EUMETSAT TD 16; the Reed-Solomon code, the randomiser and the
// convolutional code are the CCSDS ones already in core/src/ccsds and core/src/meteor.
#include "dect2/goesdcs/hrdcp_proto.h"
#include "dect2/goesdcs/goesdcs_proto.h"
#include "dect2/ccsds/ccsds_tm.h"
#include "dect2/meteor/meteor_proto.h"
#include <algorithm>
#include <cstdio>

namespace dect2 {
namespace hrdcp {

uint32_t crc32(const uint8_t* p, size_t n) {
    uint32_t c = 0;
    for (size_t i = 0; i < n; i++) {
        c ^= (uint32_t)p[i] << 24;
        for (int k = 0; k < 8; k++) c = (c & 0x80000000u) ? (c << 1) ^ 0x741B8CD7u : c << 1;
    }
    return c;
}

void putHeader(const Header& h, uint8_t o[kHeaderBytes]) {
    const uint32_t a = ((h.address & 0x7FFFFFFFu) << 1) | (h.reserved ? 1u : 0u);
    o[0] = (uint8_t)(a >> 24); o[1] = (uint8_t)(a >> 16); o[2] = (uint8_t)(a >> 8); o[3] = (uint8_t)a;
    o[4] = (uint8_t)(h.length >> 8); o[5] = (uint8_t)h.length;
    o[6] = (uint8_t)(h.sequence >> 8); o[7] = (uint8_t)h.sequence;
    const uint16_t e = (uint16_t)(((h.version & 7) << 13) | ((h.type & 1) << 12) | ((h.compression & 3) << 10) | (h.health & 0x3FF));
    o[8] = (uint8_t)(e >> 8); o[9] = (uint8_t)e;
    o[10] = (uint8_t)(h.spare >> 8); o[11] = (uint8_t)h.spare;
}

Header getHeader(const uint8_t in[kHeaderBytes]) {
    Header h;
    const uint32_t a = (uint32_t)in[0] << 24 | (uint32_t)in[1] << 16 | (uint32_t)in[2] << 8 | in[3];
    h.address = a >> 1; h.reserved = (a & 1) != 0;
    h.length = (uint16_t)(in[4] << 8 | in[5]);
    h.sequence = (uint16_t)(in[6] << 8 | in[7]);
    const uint16_t e = (uint16_t)(in[8] << 8 | in[9]);
    h.version = e >> 13; h.type = (e >> 12) & 1; h.compression = (e >> 10) & 3; h.health = e & 0x3FF;
    h.spare = (uint16_t)(in[10] << 8 | in[11]);
    return h;
}

std::vector<uint8_t> buildMessage(Header h, const std::vector<uint8_t>& data) {
    h.length = (uint16_t)std::min<size_t>(data.size(), kMaxData);
    std::vector<uint8_t> m(kHeaderBytes);
    putHeader(h, m.data());
    m.insert(m.end(), data.begin(), data.begin() + h.length);
    const uint32_t c = crc32(m.data(), m.size());
    for (int s = 24; s >= 0; s -= 8) m.push_back((uint8_t)(c >> s));
    return m;
}

int framesFor(size_t n) { return (int)((n + kFrameData - 1) / kFrameData); }

namespace {
ccsds::Format format() {
    ccsds::Format f;
    f.blockBytes = kFrameCoded; f.rs = true; f.randomised = true;
    f.layout.e = 16; f.layout.interleave = 3; f.layout.n = 255; f.layout.dual = true;
    return f;
}
} // namespace

std::vector<uint8_t> codedBytes(const std::vector<uint8_t>& msg, int tail) {
    const ccsds::Format f = format();
    std::vector<uint8_t> out;
    const int nf = std::max(1, framesFor(msg.size()));
    for (int i = 0; i < nf; i++) {
        std::vector<uint8_t> fr(kFrameData, 0);
        const size_t a = (size_t)i * kFrameData;
        for (size_t j = 0; j < (size_t)kFrameData && a + j < msg.size(); j++) fr[j] = msg[a + j];
        const std::vector<uint8_t> b = ccsds::encodeBlock(f, fr);
        out.insert(out.end(), b.begin(), b.end());
    }
    for (int i = 0; i < tail; i++) out.push_back(kTail);
    return out;
}

void convEncode(const std::vector<uint8_t>& bytes, std::vector<uint8_t>& iBits, std::vector<uint8_t>& qBits) {
    meteor::ConvEncoder e;
    iBits.clear(); qBits.clear();
    for (uint8_t v : bytes)
        for (int k = 7; k >= 0; k--) {
            uint8_t a, b;
            e.push((v >> k) & 1, a, b);
            iBits.push_back(a);
            qBits.push_back((uint8_t)(b ^ 1));
        }
}

bool decodeFrame(const uint8_t coded[kFrameCoded], uint8_t data[kFrameData], int& corrected) {
    std::vector<uint8_t> fr;
    const bool ok = format().decode(coded, kFrameCoded, fr, corrected);
    std::copy(fr.begin(), fr.begin() + std::min<size_t>(fr.size(), kFrameData), data);
    return ok;
}

int decodeMessage(const std::vector<uint8_t>& info, Decoded& d) {
    d = Decoded();
    const int have = (int)(info.size() / kFrameCoded);
    if (have < 1) return 1;
    std::vector<uint8_t> msg;
    uint8_t fr[kFrameData];
    int c = 0;
    const bool ok0 = decodeFrame(info.data(), fr, c);
    d.frames = 1; d.framesBad = ok0 ? 0 : 1; d.rsCorrected = ok0 ? c : 0;
    msg.insert(msg.end(), fr, fr + kFrameData);
    d.hdr = getHeader(msg.data());
    uint32_t cw; int ne;
    d.headerOk = ok0 && dcs::addressDecode(d.hdr.address, cw, ne) && d.hdr.version == 1;
    if (!d.headerOk) return 0;
    if (cw != d.hdr.address) { d.hdr.address = cw; }
    d.addressFixed = ne;
    const size_t total = (size_t)kHeaderBytes + d.hdr.length + kCrcBytes;
    const int need = framesFor(total);
    for (int i = 1; i < need && i < have; i++) {
        const bool ok = decodeFrame(info.data() + (size_t)i * kFrameCoded, fr, c);
        d.frames++;
        if (!ok) d.framesBad++; else d.rsCorrected += c;
        msg.insert(msg.end(), fr, fr + kFrameData);
    }
    const size_t got = std::min(msg.size(), total);
    if (got > (size_t)kHeaderBytes) d.data.assign(msg.begin() + kHeaderBytes, msg.begin() + (long)std::min(got, total - kCrcBytes));
    if (have < need) return need - have;
    d.complete = true;
    // the CRC is over the header as sent: with the BCH-corrected address put back
    std::vector<uint8_t> chk(msg.begin(), msg.begin() + (long)(total - kCrcBytes));
    putHeader(d.hdr, chk.data());
    const uint32_t rx = (uint32_t)msg[total - 4] << 24 | (uint32_t)msg[total - 3] << 16 | (uint32_t)msg[total - 2] << 8 | msg[total - 1];
    d.crcOk = crc32(chk.data(), chk.size()) == rx;
    return 0;
}

std::string healthText(int h) {
    std::string s;
    for (int i = 9; i >= 0; i--) s += (h >> i & 1) ? '1' : '0';
    return s;
}

std::string describe(const Decoded& d) {
    char b[200];
    if (!d.headerOk) return "HRDCP: header not decoded";
    snprintf(b, sizeof b, "HRDCP %s seq %u: %u bytes, %s, %s, CRC %s, %d frames (%d bad, %d symbols corrected)", dcs::addressHex(d.hdr.address).c_str(), d.hdr.sequence,
             d.hdr.length, d.hdr.type ? "alert" : "self-timed", d.hdr.compression == 1 ? "gzip" : d.hdr.compression ? "compressed" : "not compressed",
             !d.complete ? "missing" : d.crcOk ? "good" : "BAD", d.frames, d.framesBad, d.rsCorrected);
    return b;
}

} // namespace hrdcp
} // namespace dect2
