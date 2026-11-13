// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// HF digital receiver telemetry, shared by the receiver and the UI: the receiver's own members, then each decoder's telemetry by value
// (defined in hfdig_rtty.h, hfdig_sstv.h and hfdig_freedv.h).
// The first members are copied into RxTelemetry (history plot, top bar): keep them and their meaning.
#pragma once
#include "hfdig_rtty.h"
#include "hfdig_sstv.h"
#include "hfdig_freedv.h"
#include "hfdig_ftx.h"
#include "hfdig_multiband.h"
#include "hfdig_hamdrm.h"
#include "hfdig_kbd.h"
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace dect2 {

struct HfdigTelemetry {
    uint64_t seq = 0;            // grows with every report
    int state = 0;               // 0 no decoder yet (the audio is made and handed to the decoders)
    double cfoHz = 0;            // carrier error
    float snrDb = 0;
    float levelDb = -200;        // power of the whole input, dBFS (-200 = nothing measured yet)
    bool dataValid = false;      // something has been decoded
    uint64_t blocksOk = 0;       // good and bad blocks of the decoders
    uint64_t blocksBad = 0;
    // set-up
    double timeSec = 0;          // signal seconds since the start (now)
    double inputRate = 0;
    float audioDb = -200;        // power of the upper sideband (200 .. 3800 Hz) before the AGC, dBFS
    // the decoders
    HfdigRttyTelemetry rtty;
    std::vector<RttyWideSignal> rttyWide;   // whole-span decoding: every RTTY signal of the span (empty when it is off)
    HfdigSstvTelemetry sstv;
    HfdigFreedvTelemetry freedv;
    HfdigFtxTelemetry ftx;
    HfdigHamdrmTelemetry hamdrm;
    HfdigKbdTelemetry kbd;
    std::string rsidMode;        // the last RSID heard (fldigi's mode identifier)
    double rsidAudioHz = 0;
    float rsidAgeSec = 0;
    MbStatus multiBand;                     // multi-band monitor (hfdig_multiband.h): what is covered, dropped, the load
    bool multiBandOn = false;
};

// One short line about the three decoders, e.g. "RTTY 45.45/170 locked · SSTV Robot 36 112/240 · FreeDV no sync"; "listening" when none is active.
inline std::string hfdigSummary(const HfdigTelemetry& t) {
    std::string out;
    auto add = [&](const char* s) { if (!out.empty()) out += " \xC2\xB7 "; out += s; };
    char b[96];
    if (t.rtty.state >= 1) {
        snprintf(b, sizeof b, "RTTY %g/%g %s", t.rtty.baud, t.rtty.shiftHz, t.rtty.state == 2 ? "receiving" : "locked");
        add(b);
    }
    if (t.sstv.state == 1) {
        snprintf(b, sizeof b, "SSTV %s %d/%d", t.sstv.mode.c_str(), t.sstv.lines, t.sstv.height);
        add(b);
    }
    const bool fdv = t.freedv.mode >= 0;
    if (fdv) { snprintf(b, sizeof b, "FreeDV %s sync", freedvModeName(t.freedv.mode)); add(b); }
    else if (!out.empty()) add(t.freedv.libFound ? "FreeDV no sync" : "FreeDV no library");
    if (t.ftx.total > 0) {
        std::string m;
        for (int k = 0; k < kFtxModes; k++)
            if (t.ftx.perMode[k]) { snprintf(b, sizeof b, "%s%s %d", m.empty() ? "" : " ", ftxModeName(k), t.ftx.lastSlotCount[k]); m += b; }
        add(m.c_str());
    }
    if (t.hamdrm.state == 1 && t.hamdrm.mode >= 0) {
        if (t.hamdrm.haveFile && t.hamdrm.segTotal > 0) snprintf(b, sizeof b, "HamDRM %s %d/%d", hamdrmModeName(t.hamdrm.mode), t.hamdrm.segHave, t.hamdrm.segTotal);
        else snprintf(b, sizeof b, "HamDRM %s", hamdrmModeName(t.hamdrm.mode));
        add(b);
    }
    if (t.kbd.state == 1) {
        char kb[48];
        snprintf(kb, sizeof kb, "%s locked", t.kbd.cfg.family == KbdOlivia || t.kbd.cfg.family == KbdContestia ? kbdFamilyName(t.kbd.cfg.family) : kbdVariantName(t.kbd.cfg.family, t.kbd.cfg.variant));
        add(kb);
    }
    return out.empty() ? "listening" : out;
}

} // namespace dect2
