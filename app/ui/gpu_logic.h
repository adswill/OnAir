// "Use GPU where available": the global tick, the per-mode overrides and why a mode has no GPU tick. No ImGui, no App (the test checks it).
// Stored as two settings: gpuGlobal (flag, default on) and gpuModes (text "DVB=0;DTMB=1": the mode-list name = 1 forced on, 0 forced off;
// a mode that is not there follows the global tick). The default changes nothing: every decoder keeps its CPU / GPU / Auto choice and the
// tick only forces the CPU path when it is off.
#pragma once
#include "dect2/dsp/gpu_policy.h"
#include <cstring>
#include <initializer_list>
#include <string>

namespace gpulogic {

struct Row {
    const char* mode;    // the name of the row in the mode list
    int domains[2];      // dect2::gpupol::Domain values it drives, -1 = none
};
// the mode-list rows that have a GPU stage today. DVB drives DVB-T2 (LDPC) and DVB-T (Viterbi, experimental: only when GPU is picked).
inline const Row* gpuRows(int& n) {
    static const Row r[] = {{"DVB", {dect2::gpupol::T2, dect2::gpupol::T}}, {"DVB-S/S2", {dect2::gpupol::DvbS, -1}}, {"DTMB", {dect2::gpupol::Dtmb, -1}}};
    n = 3;
    return r;
}
inline const Row* rowOf(const char* mode) {
    int n; const Row* r = gpuRows(n);
    for (int i = 0; i < n; i++) if (!strcmp(r[i].mode, mode)) return &r[i];
    return nullptr;
}

// the stored override of one mode: 1 on, 0 off, -1 none
inline int overrideOf(const std::string& list, const char* mode) {
    size_t p = 0;
    while (p < list.size()) {
        size_t e = list.find(';', p); if (e == std::string::npos) e = list.size();
        const std::string item = list.substr(p, e - p);
        const size_t q = item.rfind('=');
        if (q != std::string::npos && item.compare(0, q, mode) == 0 && q + 1 < item.size()) return item[q + 1] == '1' ? 1 : item[q + 1] == '0' ? 0 : -1;
        p = e + 1;
    }
    return -1;
}
inline std::string setOverride(const std::string& list, const char* mode, int v) {
    std::string out;
    size_t p = 0;
    while (p < list.size()) {
        size_t e = list.find(';', p); if (e == std::string::npos) e = list.size();
        const std::string item = list.substr(p, e - p);
        const size_t q = item.rfind('=');
        if (!item.empty() && !(q != std::string::npos && item.compare(0, q, mode) == 0)) out += (out.empty() ? "" : ";") + item;
        p = e + 1;
    }
    if (v >= 0) out += std::string(out.empty() ? "" : ";") + mode + (v ? "=1" : "=0");
    return out;
}
// may this mode use the GPU?
inline bool modeAllowed(bool global, const std::string& list, const char* mode) {
    const int o = overrideOf(list, mode);
    return o >= 0 ? o == 1 : global;
}

// why a mode has no GPU tick (one line); nullptr when it has one
inline const char* noGpuReason(const char* mode) {
    if (rowOf(mode)) return nullptr;
    static const std::initializer_list<const char*> fecCpu = {"ATSC 3.0", "ATSC", "ISDB-T", "ISDB-S (BS)", "DVB-C", "DAB / DAB+", "DRM", "HD Radio", "CDR", "FT8", "FT4", "FT2",
        "JS8Call", "WSPR", "Wi-Fi HaLow (802.11ah)", "TETRA", "DMR", "P25", "Cell towers", "5G NR cells", "NB-IoT cells", "Meteor-M LRPT", "GOES HRIT", "HRPT / AHRPT"};
    for (const char* n : fecCpu) if (!strcmp(n, mode)) return "Its error correction has no GPU kernel yet; it runs on the CPU.";
    return "Nothing in this mode is heavy enough for the GPU; it runs on the CPU.";
}

}   // namespace gpulogic
