// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// SITOR telemetry. RxTelemetry::sitor, family 119, active standard 120, engine standard 121. SITOR-B (FEC) and SITOR-A (ARQ) are read
// side by side from the same FSK; the summary names the one that is on the air.
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>

namespace dect2 {

struct SitorTelemetry {
    uint64_t seq = 0;
    int state = 0;                 // 0 no FSK found, 1 FSK found, 2 locked on a SITOR-B (FEC) stream
    double cfoHz = 0;              // offset of the FSK centre from the dial
    float snrDb = 0;
    bool dataValid = false;
    uint64_t blocksOk = 0;         // code words with four marks while locked
    uint64_t blocksBad = 0;        // code words that were not valid
    double shiftHz = 0;            // measured tone spacing
    double baud = 0;               // measured symbol rate
    bool inverted = false;         // the stream was found with the tones swapped
    uint64_t chars = 0;
    std::string text;              // the last part of the text, newest at the end
    // SITOR-A / AMTOR (ARQ) monitor (sitor_arq.h)
    bool arqLocked = false;
    uint64_t arqBlocks = 0, arqNew = 0, arqRepeats = 0, arqBad = 0, arqCs = 0, arqChars = 0;
    int arqCsOffset = -1;          // bits from the end of a block to the control signal, -1 not found
    std::string arqText;           // the ARQ text without repeats, newest at the end
    std::string selcall;           // the selective call read from the call blocks
    std::string arqLog;            // link events, newest at the end
};

inline std::string sitorSummary(const SitorTelemetry& t) {
    char b[160];
    if (t.state == 0) return "SITOR: no FSK signal";
    if (t.arqLocked && t.state != 2) {
        snprintf(b, sizeof b, "SITOR-A (ARQ) locked, %llu blocks, %llu repeats", static_cast<unsigned long long>(t.arqNew), static_cast<unsigned long long>(t.arqRepeats));
        return b;
    }
    if (t.state == 1) { snprintf(b, sizeof b, "SITOR: FSK %.0f Hz shift, not locked", t.shiftHz); return b; }
    snprintf(b, sizeof b, "SITOR-B locked, %llu characters", static_cast<unsigned long long>(t.chars));
    return b;
}

} // namespace dect2
