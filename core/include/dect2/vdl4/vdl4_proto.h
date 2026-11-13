// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// VDL Mode 4 link layer and PHY helpers (GFSK 19.2 kbit/s, ISO/IEC 13239 framing). Receive only.
//
// Facts from ICAO Annex 10 Vol III §6.9.5 (GFSK, NRZI, training sequence), ETSI EN 301 842-2 (burst layout, sync burst,
// CPR fields), ICAO Doc 9816 (CPR encode/decode for 12+14 bit lat/lon).
#pragma once
#include "vdl4_tel.h"
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace dect2 {

constexpr double kVdl4BitRate = 19200.0;
constexpr int kVdl4RampBits = 16;
constexpr int kVdl4SyncBits = 24;
constexpr int kVdl4DataStartBit = 40;   // first link-layer bit after nominal burst start (Annex 10 §6.9.5.2.3)

extern const uint8_t kVdl4SyncPattern[kVdl4SyncBits];   // 0101... training sequence before the data segment

// ---- PHY bit layer ----
std::vector<uint8_t> vdl4NrziEncode(const std::vector<uint8_t>& dataBits, uint8_t level = 1);
std::vector<uint8_t> vdl4NrziDecode(const std::vector<uint8_t>& lineBits);
std::vector<uint8_t> vdl4BurstLineBits(const std::vector<uint8_t>& hdlcBits);   // ramp + sync + NRZI(data)
bool vdl4BurstDataBits(const std::vector<uint8_t>& lineBits, std::vector<uint8_t>& dataBits);

// ---- ISO/IEC 13239 (same polynomial as VDL2 / X.25 FCS) ----
uint16_t vdl4Fcs(const uint8_t* p, size_t n);
bool vdl4FcsOk(const uint8_t* p, size_t n);
std::vector<uint8_t> vdl4HdlcBits(const std::vector<std::vector<uint8_t>>& frames);
std::vector<std::vector<uint8_t>> vdl4HdlcFrames(const std::vector<uint8_t>& bits);

// ---- link burst (between flags, FCS included in frame passed to parse) ----
struct Vdl4BurstFields {
    bool autonomous = true;
    int rid = 0;
    int ver = 0;
    uint32_t src = 0;
    int srcType = 0;
    int burstId = 0;
    int msgId = 0;
    std::vector<uint8_t> info;    // octets between message ID and CRC (reservation + user fields)
};

std::vector<uint8_t> vdl4PackBurst(const Vdl4BurstFields& f);
bool vdl4ParseBurst(const uint8_t* frame, size_t n, Vdl4BurstFields& f);   // frame without FCS
bool vdl4DecodeMessage(const Vdl4BurstFields& f, double freqHz, Vdl4Message& m);

// CPR for the sync-burst fixed field (12-bit lat, 14-bit lon, ETSI table 5.55 / Doc 9816 §4.3)
void vdl4CprEncode(double lat, double lon, bool odd, int& latCpr, int& lonCpr);
bool vdl4CprDecodeLocal(int latCpr, int lonCpr, bool odd, double refLat, double refLon, double& lat, double& lon);
bool vdl4CprDecodeGlobal(int latE, int lonE, int latO, int lonO, bool oddLatest, double& lat, double& lon);

std::vector<uint8_t> vdl4SyncBurstInfo(double lat, double lon, int altFt, bool odd, uint32_t src, int srcType);
std::vector<uint8_t> vdl4DlsUserInfo(const std::string& text);

const char* vdl4AddrTypeName(int type);
const char* vdl4MsgIdName(int msgId);

} // namespace dect2
