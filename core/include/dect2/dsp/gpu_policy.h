// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Where a mode may use the GPU: one yes / no per GPU-capable decoder family, set from the app's "Use GPU where available" tick and the
// per-mode overrides. "No" means the CPU path of that decoder, which gives the same output (the GPU kernels are checked against it).
// Header only, no GPU code here: the decoders keep their own CPU / GPU / Auto choice (0 / 1 / 2); this only forces 0 when the tick is off.
#pragma once
#include <atomic>
#include <cstdio>
#include <string>

namespace dect2::gpupol {

enum Domain { T2 = 0, T = 1, DvbS = 2, Dtmb = 3, Atsc3 = 4, IsdbT = 5, Dsp = 6, kDomains };   // DVB-T2 LDPC, DVB-T Viterbi, DVB-S / S2 (LDPC + Viterbi), DTMB LDPC, ATSC 3.0 LDPC, ISDB-T Viterbi, generic FFT

inline const char* domainName(int d) {
    static const char* n[] = {"DVB-T2", "DVB-T", "DVB-S/S2", "DTMB", "ATSC 3.0", "ISDB-T", "Spectrum"};
    return d >= 0 && d < kDomains ? n[d] : "?";
}
inline const char* domainStage(int d) {
    static const char* n[] = {"LDPC", "Viterbi", "LDPC + Viterbi", "LDPC", "LDPC", "Viterbi", "FFT"};
    return d >= 0 && d < kDomains ? n[d] : "?";
}

// the compute mode a decoder gets: the user's choice (0 CPU, 1 GPU, 2 auto) when the GPU is allowed, else the CPU
inline int effectiveMode(int chosen, bool gpuAllowed) { return gpuAllowed ? chosen : 0; }

struct Policy {
    std::atomic<bool> allowed[kDomains];
    Policy() { for (auto& a : allowed) a = true; }
};

// Process-wide copy of the engine's choice, for decoders that have no engine object to hand it to (ATSC 3.0, ISDB-T, the spectrum FFT): the engine
// writes it (Engine::applyCompute), those decoders read it. `onGpu(d)`: where the last block of that decoder ran (-1 no data, 0 CPU, 1 GPU).
struct Live {
    std::atomic<bool> allowed[kDomains];
    std::atomic<int> compute{2};
    std::atomic<int> onGpu[kDomains];
    Live() { for (auto& a : allowed) a = true; for (auto& o : onGpu) o = -1; }
};
inline Live& live() { static Live l; return l; }
// the compute mode a decoder of `domain` should use now: 0 CPU, 1 GPU, 2 automatic
inline int liveMode(int domain) { return effectiveMode(live().compute.load(), domain >= 0 && domain < kDomains && live().allowed[domain].load()); }
inline void reportGpu(int domain, bool onGpu) { if (domain >= 0 && domain < kDomains) live().onGpu[domain] = onGpu ? 1 : 0; }

// One line for the diagnostics: which stage ran where. `onGpu` is what the decoder reported last (-1 = not running / no data).
inline std::string diagLine(int domain, bool allowed, bool gpuUsable, int chosen, int onGpu) {
    const char* where = onGpu < 0 ? "not running" : onGpu ? "GPU" : "CPU";
    const char* why = !allowed ? "GPU switched off" : !gpuUsable ? "no usable GPU" : chosen == 0 ? "compute set to CPU" : "";
    std::string s = std::string(domainName(domain)) + " " + domainStage(domain) + ": " + where;
    if (*why) s += std::string(" (") + why + ")";
    return s;
}

}   // namespace dect2::gpupol
