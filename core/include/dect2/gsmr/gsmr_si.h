// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// GSM-R: the System Information messages of the broadcast control channel (3GPP TS 44.018 9.1.31 - 9.1.36), parsed and, for the test
// signal, encoded. Each is one 23 byte layer 2 frame: L2 pseudo length, protocol discriminator (RR, 0x06), message type, the fields.
// Only the cell's public broadcast is read: its identity, the location area (operator code and area), the access parameters and the
// channel lists. No subscriber or device identity exists in these messages.
#pragma once
#include <array>
#include <cstdint>
#include <vector>

namespace dect2::gsmr {

using Frame23 = std::array<uint8_t, 23>;

constexpr int kSi1 = 0x19, kSi2 = 0x1A, kSi2bis = 0x02, kSi2ter = 0x03, kSi2quater = 0x07, kSi3 = 0x1B, kSi4 = 0x1C, kSi13 = 0x00;

struct Lai { int mcc = 0, mnc = 0, mncDigits = 2; uint32_t lac = 0; };

struct Si3 {                      // fields of System Information 3 (4 carries the same LAI and selection parameters)
    uint32_t cellId = 0;
    Lai lai;
    bool attach = true;
    int agBlks = 1, ccchConf = 0, pagMfrms = 3, t3212 = 10;
    int rlTimeout = 11, dtx = 2, pwrc = 0;
    int reselHystDb = 4, msTxPwrMax = 5, rxLevAccessMin = 12;
    bool neci = true, acs = false;
    int maxRetrans = 3, txInteger = 9;
    bool cellBar = false;
    uint16_t acBarred = 0;
};

struct Bcch {
    int type = -1;                // the RR message type
    bool ok = false;              // a well formed system information frame
    bool hasLai = false, hasSi3 = false, hasList = false;
    bool listUnparsed = false;    // a frequency list in a format other than the bit map
    Si3 si3;                      // lai from SI3 / SI4, the rest from SI3
    std::vector<int> arfcns;      // SI1: the cell allocation, SI2 / SI2bis: the BCCH neighbours
    int nccPermitted = 0;
};

bool parseBcch(const uint8_t* m23, Bcch& out);

// The 16 octet frequency list of SI1 / SI2 (44.018 10.5.2.13, 10.5.2.22 and Annex J): the bit map for ARFCN 1 - 124, the 1024, 512, 256 and
// 128 range formats (the tree of offsets W(1) ...), and the variable bit map. The ARFCNs come out sorted; false for a format that is not one
// of these.
bool decodeFreqList(const uint8_t* cd16, std::vector<int>& arfcns);
// The encoder of the test signal: the bit map when every ARFCN is 1 - 124, else the 1024 range format (up to 17 channels, within about
// 500 channels of the first); false when the list does not fit
bool encodeFreqList(const std::vector<int>& arfcns, uint8_t* cd16);

Frame23 encodeSi1(const std::vector<int>& cellAlloc, const Si3& s);
Frame23 encodeSi2(const std::vector<int>& neighbours, int nccPermitted, const Si3& s);
Frame23 encodeSi3(const Si3& s);
Frame23 encodeSi4(const Si3& s);
Frame23 encodeSi2ter();           // a frame of the right type with filler, so that a receiver sees the type
Frame23 idleFrame();              // 0x2B fill

const char* siName(int type);

// The common control channel (paging and access grant blocks): only the kind of each message is counted, as a measure of the cell's load.
// The contents (the identities in a paging request, the channel in an assignment) are never parsed or kept.
enum class CcchKind { Paging1, Paging2, Paging3, ImmAssign, ImmAssignExt, ImmAssignReject, Idle, Other };
CcchKind ccchKind(const uint8_t* m23);
struct CcchCount {
    uint64_t paging = 0, assign = 0, reject = 0, idle = 0, other = 0, bad = 0;
    void add(CcchKind k) {
        switch (k) {
        case CcchKind::Paging1: case CcchKind::Paging2: case CcchKind::Paging3: paging++; break;
        case CcchKind::ImmAssign: case CcchKind::ImmAssignExt: assign++; break;
        case CcchKind::ImmAssignReject: reject++; break;
        case CcchKind::Idle: idle++; break;
        default: other++;
        }
    }
    void merge(const CcchCount& o) { paging += o.paging; assign += o.assign; reject += o.reject; idle += o.idle; other += o.other; bad += o.bad; }
    uint64_t blocks() const { return paging + assign + reject + idle + other; }
};
Frame23 encodePagingKind(int type);   // the test signal: a frame of that RR message type with an all-zero (no identity) body

} // namespace dect2::gsmr
