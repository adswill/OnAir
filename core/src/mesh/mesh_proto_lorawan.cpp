// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// LoRaWAN PHY payload headers (no keys, nothing decrypted): see mesh_proto.h.
// Facts: LoRaWAN L2 1.0.4 (TS001-1.0.4) chapters 4 (MAC frame formats) and 5 (MAC commands), LoRaWAN 1.1 (rejoin requests, the 1.1
// MAC commands), LoRaWAN Backend Interfaces 1.0 (TS002) 6.1 (NetID and DevAddr types), RP002-1.0.4 (EU863-870 and US902-928 channels).
#include "mesh_proto_internal.h"
#include <algorithm>
#include <cstring>
#include "dect2/common/endian.h"

namespace dect2 {

namespace {

void put32(std::vector<uint8_t>& v, uint32_t x) { dect2::endian::putLe32(v, x); }
void putMic(std::vector<uint8_t>& v, uint32_t mic) { for (int i = 3; i >= 0; i--) v.push_back((uint8_t)(mic >> (8 * i))); }


} // namespace


std::vector<MeshLoraSettings> lorawanPresets(int region) {
    std::vector<MeshLoraSettings> v;
    auto add = [&](double mhz, int sf, double bw) {
        MeshLoraSettings s;
        s.name = meshFmt("LoRaWAN %.1f", mhz);
        s.freqHz = mhz * 1e6; s.sf = sf; s.bwHz = bw; s.cr = 5; s.preamble = 8; s.syncWord = 0x34;
        s.ldro = meshLoraLdro(sf, bw);
        v.push_back(s);
    };
    if (region >= 2) return v;   // ANZ: AU915 is not covered
    if (region == 1) {
        for (int c = 0; c < 64; c++) for (int sf = 7; sf <= 10; sf++) add(902.3 + 0.2 * c, sf, 125e3);   // DR0 - DR3
        for (int c = 0; c < 8; c++) add(903.0 + 1.6 * c, 8, 500e3);                                        // DR4
    } else {
        static const double eu[] = {868.1, 868.3, 868.5, 867.1, 867.3, 867.5, 867.7, 867.9};
        for (double f : eu) for (int sf = 7; sf <= 12; sf++) add(f, sf, 125e3);                            // DR5 - DR0
        add(868.3, 7, 250e3);                                                                              // DR6
    }
    return v;
}

std::vector<uint8_t> lorawanBuild(const LorawanTx& t) {
    std::vector<uint8_t> v;
    v.push_back((uint8_t)((t.mtype & 7) << 5));
    if (t.mtype == 7) { v.insert(v.end(), t.frm.begin(), t.frm.end()); putMic(v, t.mic); return v; }
    const bool up = t.mtype == 2 || t.mtype == 4;
    put32(v, t.devAddr);
    const size_t fl = std::min<size_t>(15, t.fopts.size());
    v.push_back((uint8_t)((t.adr ? 0x80 : 0) | (up && t.adrAckReq ? 0x40 : 0) | (t.ack ? 0x20 : 0) | (!up && t.fpending ? 0x10 : 0) | fl));
    v.push_back((uint8_t)(t.fcnt & 0xFF)); v.push_back((uint8_t)(t.fcnt >> 8));
    v.insert(v.end(), t.fopts.begin(), t.fopts.begin() + (long)fl);
    if (t.fport >= 0) { v.push_back((uint8_t)t.fport); v.insert(v.end(), t.frm.begin(), t.frm.end()); }
    putMic(v, t.mic);
    return v;
}

std::vector<uint8_t> lorawanJoinRequest(uint64_t joinEui, uint64_t devEui, uint16_t devNonce, uint32_t mic) {
    std::vector<uint8_t> v;
    v.push_back(0x00);
    for (int i = 0; i < 8; i++) v.push_back((uint8_t)(joinEui >> (8 * i)));
    for (int i = 0; i < 8; i++) v.push_back((uint8_t)(devEui >> (8 * i)));
    v.push_back((uint8_t)(devNonce & 0xFF)); v.push_back((uint8_t)(devNonce >> 8));
    putMic(v, mic);
    return v;
}

} // namespace dect2
