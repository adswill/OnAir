// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Hytera XPT (eXtended Pseudo Trunk, DMR Tier II with a trunking layer): the manufacturer-specific control blocks FID 0x68 that list the site's repeaters. Read only; the fields follow
// DSD-FME's findings (no document from the maker is public; DSD-FME calls this work experimental). Detected inside the DMR receiver, shown in its panel.
#pragma once
#include "dect2/dmr/dmr_proto.h"
#include <cstdint>
#include <string>
#include <vector>

namespace dect2 {

using dmr::Csbk;
using dmr::FullLc;

constexpr int kXptFid = 0x68, kXptSiteStatusOp = 0x0A, kXptAdjacentOp = 0x0B, kXptCallSetupFlco = 0x09;

struct XptLsn {            // one logical slot (LCN = (lsn - 1) / 2 + 1, repeater slot = (lsn - 1) % 2 + 1)
    int status = -1;       // 0 idle, 1 unknown, 2 private or data, 3 group
    int tg = 0;            // an 8 bit talkgroup, or the hash of a private target
    double lastSec = -1e9;
};

struct XptAdjacent { int site = 0, freeLcn = 0; double lastSec = 0; };

struct XptCall { int lcn = 0, freeLcn = 0, handshake = 0; uint32_t target = 0, source = 0; double sec = 0; };

struct XptState {
    bool seen = false;
    double lastSec = -1e9;
    int freeLcn = -1;
    XptLsn lsn[24];
    std::vector<XptAdjacent> adjacent;
    std::vector<XptCall> calls;   // the newest first, at most 32
};

// One block with FID 0x68 and one of the two opcodes: updates st and returns true. sn: the two bits lb, pf (a sequence number on this block).
bool xptParseCsbk(const Csbk& c, double now, XptState& st);
// Full link control with FID 0x68, FLCO 0x09 (call set-up)
bool xptParseCall(const FullLc& f, double now, XptState& st);
// A block as the receiver expects it (tests, generator)
Csbk xptSiteStatusCsbk(int seq, int freeLcn, const int status[6], const int tg[6]);
FullLc xptCallLc(int lcn, int freeLcn, int handshake, uint32_t target, uint32_t source);

std::string xptSummary(const XptState& st);

} // namespace dect2
