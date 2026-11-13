// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Mode S uplink (1030 MHz) telemetry, shared by the receiver and the UI.
// The first members are copied into RxTelemetry (history plot, top bar): keep them and their meaning.
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace dect2 {

// A ground interrogator: a Mode S station by its II / SI code, or the Mode A/C (or intermode) interrogations together
struct UplinkInterrogator {
    int kind = 0;                  // 0 a Mode S interrogator, 1 the Mode A/C interrogations, 2 the intermode all-calls (Mode S radars, no code)
    int ii = -1, si = -1;
    uint64_t count = 0;            // interrogations heard
    uint64_t allCalls = 0, rollCalls = 0, modeA = 0, modeC = 0, sidelobe = 0, intermode = 0;
    float ratePerSec = 0;          // over the last 10 s
    float periodSec = 0;           // antenna rotation from the main-beam bursts, 0 until two bursts
    int bursts = 0;
    float levelDb = -200;          // strongest main-beam level, dBFS
    double lastSec = 0, lastBurstSec = 0;
};

struct UplinkAircraft {
    uint32_t addr = 0;
    uint64_t count = 0;            // interrogations addressed to it
    uint64_t uf[32] = {};
    std::vector<std::string> registers;   // Comm-B registers asked for ("4,0"), first time order
    std::string interrogators;     // "II 3, SI 17"
    bool confirmed = false;        // addressed by more than one interrogation (a random bit error gives an address once)
    double firstSec = 0, lastSec = 0;
    float levelDb = -200;
};

struct UplinkTcas {
    double sec = 0;
    int uf = 0;
    uint32_t from = 0, to = 0;     // UF16: MID (the ACAS aircraft) and the address it coordinates with; UF0: from 0
    std::string text;
};

struct UplinkLogLine {
    double sec = 0;
    int uf = -1;                   // -1 Mode A/C or intermode
    uint32_t addr = 0;
    float levelDb = -200;
    std::string text;
};

constexpr size_t kUplinkLogKeep = 500;
constexpr size_t kUplinkTcasKeep = 200;

struct UplinkTelemetry {
    uint64_t seq = 0;
    int state = 0;                 // 0 nothing, 1 pulses, 2 interrogations decoded in the last 10 s
    double cfoHz = 0;              // mean of the Mode S messages
    float snrDb = 0;               // newest message, over the noise floor
    float levelDb = -200;
    bool dataValid = false;
    uint64_t blocksOk = 0;         // interrogations decoded (Mode A/C and S)
    uint64_t blocksBad = 0;        // Mode S that did not decode (no sync, bad UF11 parity, undefined format)
    // details
    double timeSec = 0, inputRate = 0;
    float noiseDb = -200;
    uint64_t pulses = 0;
    uint64_t modeA = 0, modeC = 0, modeAcSidelobe = 0, intermodeAcs = 0, intermodeAc = 0, modeS = 0, modeSSidelobe = 0;
    uint64_t uf[32] = {};
    std::vector<UplinkInterrogator> interrogators;
    std::vector<UplinkAircraft> aircraft;   // by last seen, newest first
    std::vector<UplinkTcas> tcas;           // oldest first
    std::vector<UplinkLogLine> log;         // oldest first
};

inline std::string uplinkSummary(const UplinkTelemetry& t) {
    char b[200];
    snprintf(b, sizeof b, "%llu Mode S, %llu Mode A/C, %zu interrogators, %zu aircraft", (unsigned long long)t.modeS,
             (unsigned long long)(t.modeA + t.modeC), t.interrogators.size(), t.aircraft.size());
    return b;
}

} // namespace dect2
