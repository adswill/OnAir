// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// voiceWhy(): each stage of the chain gives its own verdict, and the per-mode adapters feed it from the telemetry.
#include "dect2/voice/voice_why_modes.h"
#include <cstdio>
using namespace dect2;
#include "../common/test_util.h"

int main() {
    VoiceWhyIn in; in.levelDb = -60;
    CHECK(voiceWhy(in) == VoiceWhy::NoSync, "silence with power: no sync");
    in.levelDb = -140; CHECK(voiceWhy(in) == VoiceWhy::NoInput, "no power");
    in.levelDb = -60; in.syncs = 40; in.idBad = 30; CHECK(voiceWhy(in) == VoiceWhy::IdUnreadable, "syncs, identity unreadable");
    in.idBad = 2; in.fecBad = 20; CHECK(voiceWhy(in) == VoiceWhy::FecFails, "identity reads, FEC fails");
    in.ok = 10; CHECK(voiceWhy(in) == VoiceWhy::Marginal, "two thirds lost");
    in.ok = 100; in.fecBad = 5; CHECK(voiceWhy(in) == VoiceWhy::Decoding, "decoding");
    DmrTelemetry d; d.levelDbfs = -50; CHECK(dmrWhy(d) == VoiceWhy::NoSync, "dmr empty");
    d.syncCount[0] = 5; d.bptcFail = 5; CHECK(dmrWhy(d) == VoiceWhy::FecFails, "dmr fec");
    d.blocksOk = 40; d.bptcFail = 2; CHECK(dmrWhy(d) == VoiceWhy::Decoding, "dmr ok");
    P25Telemetry p; p.levelDb = -50; p.frames = 10; p.nidBad = 10; CHECK(p25Why(p) == VoiceWhy::IdUnreadable, "p25 nid");
    NxdnTelemetry n; n.levelDb = -50; n.frames = 9; n.lichBad = 1; n.sacchBad = 9; CHECK(nxdnWhy(n) == VoiceWhy::FecFails, "nxdn sacch");
    DstarTelemetry s; s.levelDb = -50; s.headersBad = 3; CHECK(dstarWhy(s) == VoiceWhy::FecFails, "dstar header");
    TetraTelemetry t; t.levelDb = -50; t.blocksBad = 500; CHECK(tetraWhy(t) == VoiceWhy::NoSync, "tetra searching");
    return fails ? 1 : 0;
}
