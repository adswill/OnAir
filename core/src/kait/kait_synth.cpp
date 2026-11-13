// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// KAIT test signal for the engine's synthetic source (see kait_gen.h).
#include "dect2/kait/kait_gen.h"
#include "dect2/kait/kait_txgen.h"
#include "dect2/engine/synth_loop.h"

namespace dect2 {

std::unique_ptr<ModeSynth> makeKaitSynth(const SynthConfig& s, double sampleRate) {
    if (sampleRate < 400000 - 1) return nullptr;
    int profile = s.modeOpt[0] > 0 ? s.modeOpt[0] - 1 : 0;
    if (profile < 0 || profile > 3) profile = 0;
    const double rate = sampleRate;
    auto synth = std::make_unique<TxLoopSynth>(rate, 500.0 + s.cfoHz, s.snrDb, 0.0, [rate, profile](int pass) -> std::unique_ptr<TxGenerator> {
    TxRequest r;
    r.modeId = "kait";
    r.callsign = "ONAIR";
    r.variant = profile;
    r.opt["kait.gap"] = 16;   // a receiver that decodes a burst on a busy machine is ready again for the next sweep
    (void)pass;
    std::string err;
    return makeKaitTxGenerator(r, rate, err);
    }, 0x4B414954ULL);
    if (!synth->ok()) return nullptr;
    return synth;
}

} // namespace dect2
