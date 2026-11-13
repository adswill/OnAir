// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// LoRa APRS and MeshCom 4 packet layer: see mesh_proto.h.
// Sources (read 2026-10-10):
//   LoRa APRS: richonguzman/LoRa_APRS_iGate (GPL-3.0) src/lora_utils.cpp (prefix "\x3c\xff\x01" on transmit, checked and stripped on
//   receive, setCRC(true), RadioLib begin() defaults otherwise), data/igate_conf.json; richonguzman/LoRa_APRS_Tracker data/tracker_conf.json
//   (433.775 SF12 / 434.855 SF9 4/7 / 439.9125 SF12 / 915 SF12, all 125 kHz); lora-aprs/LoRa_APRS_iGate data/is-cfg.json (433.775 SF12
//   125 kHz 4/5); RadioLib SX1262.h (begin defaults: sync word RADIOLIB_SX126X_SYNC_WORD_PRIVATE 0x12, preamble 8).
//   MeshCom: icssw-org/MeshCom-Firmware (MIT), branch dev: src/aprs_functions.cpp (decodeAPRS, encodeAPRS, encodeLoRaAPRS),
//   src/lora_functions.cpp (handleACK), src/country_profile.cpp and src/configuration_default.h (frequencies, SF, BW, CR),
//   src/configuration_global.h (SYNC_WORD_SX127x 0x2b, DEFAULT_PREAMPLE_LENGTH 32, MSG_TYPE_*), src/lora_setchip.cpp (CRC on).
// Written from the descriptions above; no code was copied.
#include "mesh_proto_internal.h"
#include <cctype>
#include <cstring>

namespace dect2 {

std::vector<MeshLoraSettings> loraAprsPresets() {
    struct Row { const char* name; double mhz; int sf, cr; };
    static const Row rows[] = {
        {"LoRa APRS EU", 433.775, 12, 5}, {"LoRa APRS PL", 434.855, 9, 7}, {"LoRa APRS UK", 439.9125, 12, 5}, {"LoRa APRS 915", 915.0, 12, 5},
    };
    std::vector<MeshLoraSettings> v;
    for (const auto& r : rows) {
        MeshLoraSettings s;
        s.name = r.name; s.freqHz = r.mhz * 1e6; s.sf = r.sf; s.bwHz = 125000; s.cr = r.cr; s.preamble = 8; s.syncWord = 0x12;
        s.ldro = meshLoraLdro(s.sf, s.bwHz);
        v.push_back(s);
    }
    return v;
}

std::vector<MeshLoraSettings> meshcomPresets() {
    // country_profile.cpp (ESP32 columns): frequency, bandwidth, SF; CR 4/6 everywhere; preamble 32 for the default (EU), 8 in the table
    struct Row { const char* name; double mhz, bwKhz; int sf, pre; };
    static const Row rows[] = {
        {"MeshCom EU", 433.175, 250, 11, 32}, {"MeshCom UK", 439.9125, 125, 10, 8}, {"MeshCom LA", 433.925, 125, 10, 8},
        {"MeshCom 868", 869.525, 250, 11, 8}, {"MeshCom 915", 906.875, 250, 11, 8}, {"MeshCom VR2", 435.775, 250, 11, 8},
        {"MeshCom 435", 435.750, 250, 11, 8}, {"MeshCom 436", 436.250, 250, 11, 8}, {"MeshCom 442", 442.000, 250, 11, 8},
    };
    std::vector<MeshLoraSettings> v;
    for (const auto& r : rows) {
        MeshLoraSettings s;
        s.name = r.name; s.freqHz = r.mhz * 1e6; s.sf = r.sf; s.bwHz = r.bwKhz * 1e3; s.cr = 6; s.preamble = r.pre; s.syncWord = 0x2B;
        s.ldro = meshLoraLdro(s.sf, s.bwHz);
        v.push_back(s);
    }
    return v;
}

std::vector<uint8_t> loraAprsFrame(const std::string& tnc2) {
    std::vector<uint8_t> v = {0x3C, 0xFF, 0x01};
    v.insert(v.end(), tnc2.begin(), tnc2.end());
    return v;
}

std::vector<uint8_t> meshcomBuild(const MeshComFrame& f) {
    std::vector<uint8_t> b;
    b.push_back((uint8_t)f.type);
    for (int k = 0; k < 4; k++) b.push_back((uint8_t)(f.msgId >> (8 * k)));
    b.push_back((uint8_t)((f.maxHop & 0x0F) | (f.server ? 0x80 : 0) | (f.track ? 0x40 : 0) | (f.appOffline ? 0x20 : 0) | (f.mesh ? 0x10 : 0)));
    const std::string head = f.sourcePath + ">" + f.dest + f.type + f.payload;
    b.insert(b.end(), head.begin(), head.end());
    b.push_back(0);
    b.push_back(f.hw);
    b.push_back(f.mod);
    uint32_t sum = 0;
    for (uint8_t c : b) sum += c;
    b.push_back((uint8_t)(sum >> 8));
    b.push_back((uint8_t)sum);
    b.push_back(f.fw);
    b.push_back(f.lastHw);
    b.push_back(f.subVersion ? (uint8_t)f.subVersion : (uint8_t)'#');
    b.push_back(0x7E);
    return b;
}

} // namespace dect2
