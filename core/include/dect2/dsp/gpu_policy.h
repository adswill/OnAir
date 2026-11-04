// Where a mode may use the GPU: one yes / no per GPU-capable decoder family, set from the app's "Use GPU where available" tick and the
// per-mode overrides. "No" means the CPU path of that decoder, which gives the same output (the GPU kernels are checked against it).
// Header only, no GPU code here: the decoders keep their own CPU / GPU / Auto choice (0 / 1 / 2); this only forces 0 when the tick is off.
#pragma once
#include <atomic>
#include <cstdio>
#include <string>

namespace dect2::gpupol {

enum Domain { T2 = 0, T = 1, DvbS = 2, Dtmb = 3, kDomains };   // DVB-T2 LDPC, DVB-T Viterbi, DVB-S / S2 (LDPC + Viterbi), DTMB LDPC

inline const char* domainName(int d) {
    static const char* n[] = {"DVB-T2", "DVB-T", "DVB-S/S2", "DTMB"};
    return d >= 0 && d < kDomains ? n[d] : "?";
}
inline const char* domainStage(int d) {
    static const char* n[] = {"LDPC", "Viterbi", "LDPC + Viterbi", "LDPC"};
    return d >= 0 && d < kDomains ? n[d] : "?";
}

// the compute mode a decoder gets: the user's choice (0 CPU, 1 GPU, 2 auto) when the GPU is allowed, else the CPU
inline int effectiveMode(int chosen, bool gpuAllowed) { return gpuAllowed ? chosen : 0; }

struct Policy {
    std::atomic<bool> allowed[kDomains];
    Policy() { for (auto& a : allowed) a = true; }
};

// One line for the diagnostics: which stage ran where. `onGpu` is what the decoder reported last (-1 = not running / no data).
inline std::string diagLine(int domain, bool allowed, bool gpuUsable, int chosen, int onGpu) {
    const char* where = onGpu < 0 ? "not running" : onGpu ? "GPU" : "CPU";
    const char* why = !allowed ? "GPU switched off" : !gpuUsable ? "no usable GPU" : chosen == 0 ? "compute set to CPU" : "";
    std::string s = std::string(domainName(domain)) + " " + domainStage(domain) + ": " + where;
    if (*why) s += std::string(" (") + why + ")";
    return s;
}

}   // namespace dect2::gpupol
