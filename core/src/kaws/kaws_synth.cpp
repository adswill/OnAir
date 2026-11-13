// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// KAWS test signal for the engine's synthetic source (see kaws_gen.h).
#include "dect2/kaws/kaws_gen.h"
#include "dect2/kaws/kaws_txgen.h"
#include "dect2/engine/synth_loop.h"
#include "dect2/kaws/kaws_live.h"

namespace dect2 {

std::unique_ptr<ModeSynth> makeKawsSynth(const SynthConfig& s, double sampleRate) {
    if (sampleRate < 250000 - 1) return nullptr;
    int profile = s.modeOpt[0] > 0 ? s.modeOpt[0] - 1 : 3;
    if (profile < 0 || profile > 5) profile = 3;
    const double rate = sampleRate;
    auto synth = std::make_unique<TxLoopSynth>(rate, -kawsTuning().tuneOffsetHz + 30.0 + s.cfoHz, s.snrDb, 0.0, [rate, profile](int pass) -> std::unique_ptr<TxGenerator> {
    TxRequest r;
    r.modeId = "kaws";
    r.callsign = "ONAIR";
    r.grid = "KO24RM";
    r.dest = "CQ";
    r.text = "OnAir KAWS test signal: chat from the real transmitter.";
    r.opt["kaws.kind"] = (pass % 2 == 0) ? 0 : 1;   // chat, then a short beacon
    r.opt["kaws.profile"] = profile;
    r.opt["kaws.repeat"] = 1;
    r.opt["kaws.align"] = 1;   // wait for the next slot of the computer clock (kaws.utc stays 0)
    r.opt["kaws.msgid"] = pass % 16;
    std::string err;
    return makeKawsTxGenerator(r, rate, err);
    }, 0x4B415753ULL);
    if (!synth->ok()) return nullptr;
    return synth;
}

} // namespace dect2
