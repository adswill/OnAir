// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// KAPP test signal for the engine's synthetic source (see kapp_gen.h).
#include "dect2/kapp/kapp_gen.h"
#include "dect2/kapp/kapp_txgen.h"
#include "dect2/engine/synth_loop.h"

namespace dect2 {

std::unique_ptr<ModeSynth> makeKappSynth(const SynthConfig& s, double sampleRate) {
    if (sampleRate < 125000 - 1) return nullptr;
    int profile = s.modeOpt[0] > 0 ? s.modeOpt[0] - 1 : 2;
    if (profile < 0 || profile > 8) profile = 2;
    const double rate = sampleRate;
    auto synth = std::make_unique<TxLoopSynth>(rate, 1000.0 + s.cfoHz, s.snrDb, 1.0, [rate, profile](int pass) -> std::unique_ptr<TxGenerator> {
    TxRequest r;
    r.modeId = "kapp";
    r.callsign = "ONAIR";
    static const char* kText[3] = {"OnAir KAPP test signal: message one. Selective-repeat packets, sent by the real transmitter.",
                                   "OnAir KAPP test signal: message two. A receiver that joins late gets the next one.",
                                   "OnAir KAPP test signal: message three, then the first again."};
    r.text = kText[pass % 3];
    r.opt["kapp.profile"] = profile;
    r.opt["kapp.repeat"] = 1;
    std::string err;
    return makeKappTxGenerator(r, rate, err);
    }, 0x4B415050ULL);
    if (!synth->ok()) return nullptr;
    return synth;
}

} // namespace dect2
