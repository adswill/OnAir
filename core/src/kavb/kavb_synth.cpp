// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// KAVB test signal for the engine's synthetic source (see kavb_gen.h).
#include "dect2/kavb/kavb_gen.h"
#include "dect2/kavb/kavb_txgen.h"
#include "dect2/engine/synth_loop.h"

namespace dect2 {

std::unique_ptr<ModeSynth> makeKavbSynth(const SynthConfig& s, double sampleRate) {
    if (sampleRate < 2e+06 - 1) return nullptr;
    int profile = s.modeOpt[0] > 0 ? s.modeOpt[0] - 1 : 0;
    if (profile < 0 || profile > 3) profile = 0;
    const double rate = sampleRate;
    auto synth = std::make_unique<TxLoopSynth>(rate, 3000.0 + s.cfoHz, s.snrDb, 1.0, [rate, profile](int pass) -> std::unique_ptr<TxGenerator> {
    TxRequest r;
    r.modeId = "kavb";
    r.callsign = "ONAIR";
    r.opt["kavb.profile"] = profile;
    r.opt["kavb.seconds"] = 3;
    (void)pass;
    std::string err;
    return makeKavbTxGenerator(r, rate, err);
    }, 0x4B415642ULL);
    if (!synth->ok()) return nullptr;
    return synth;
}

} // namespace dect2
