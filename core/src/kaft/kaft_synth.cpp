// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// KAFT test signal for the engine's synthetic source (see kaft_gen.h).
#include "dect2/kaft/kaft_gen.h"
#include "dect2/kaft/kaft_txgen.h"
#include "dect2/engine/synth_loop.h"

namespace dect2 {

std::unique_ptr<ModeSynth> makeKaftSynth(const SynthConfig& s, double sampleRate) {
    if (sampleRate < 1.25e+06 - 1) return nullptr;
    int profile = s.modeOpt[0] > 0 ? s.modeOpt[0] - 1 : 2;
    if (profile < 0 || profile > 9) profile = 2;
    const double rate = sampleRate;
    auto synth = std::make_unique<TxLoopSynth>(rate, 1500.0 + s.cfoHz, s.snrDb, 1.0, [rate, profile](int pass) -> std::unique_ptr<TxGenerator> {
    TxRequest r;
    r.modeId = "kaft";
    r.callsign = "ONAIR";
    r.opt["kaft.profile"] = profile;
    r.opt["kaft.width"] = 2;
    r.opt["kaft.repeat"] = 1;
    r.opt["kaft.session"] = 4242;
    (void)pass;
    std::string err;
    return makeKaftTxGenerator(r, rate, err);
    }, 0x4B414654ULL);
    if (!synth->ok()) return nullptr;
    return synth;
}

} // namespace dect2
