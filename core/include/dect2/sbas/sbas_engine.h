// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// SBAS L1 receiver in the engine: the GNSS receiver (acquisition, tracking, message layer) restricted to the SBAS PRNs 120..158 with no GPS satellite needed,
// and the content decoder of sbas_msg.h behind it. Engine standard 152 (family 150, "SBAS"). Receive only: the corrections are shown, not applied.
// Not covered: the QZSS L1S (SLAS) and L6 signals, which have their own codes and message formats and need the IS-QZSS description; the QZSS L1 C/A
// satellites are in the GNSS mode.
#pragma once
#include "dect2/engine/mode_tuning.h"
#include "dect2/sbas/sbas_msg.h"
#include "dect2/common/ring.h"
#include "dect2/source/source.h"
#include "dect2/engine/mode_synth.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace dect2 {

struct SbasSat {
    int prn = 0;
    int state = 0;               // GnssChState: 0 pull-in .. 3 frame sync
    float cn0 = 0;
    double dopplerHz = 0;
    float lockSecs = 0;
    uint32_t messages = 0;       // with a good CRC
    uint32_t framesBad = 0;
    int lastType = -1;
    std::vector<float> cn0Hist;
};

struct SbasTelemetry {
    uint64_t seq = 0;
    int state = 0;               // 0 searching, 1 satellites tracked, 2 messages decoded
    double cfoHz = 0;
    bool dataValid = false;      // at least one PRN mask and one fast correction held
    float snrDb = 0;             // best C/N0
    uint64_t blocksOk = 0, blocksBad = 0;
    double inputRate = 0, centerMhz = 0, signalSecs = 0;
    float levelDbfs = -99;
    bool searching = false;
    int searchPrn = 0;
    float searchProgress = 0;
    uint32_t searchRounds = 0;
    std::vector<SbasSat> sats;                 // tracked, strongest first
    std::vector<sbas::GeoState> geos;          // what each satellite has sent, by PRN
    std::vector<sbas::LogEntry> log;           // the last 100 messages, oldest first
    uint64_t typeCount[64] = {};
    uint64_t total = 0;
    std::string status;
};

class SbasReceiver {
public:
    SbasReceiver();
    ~SbasReceiver();
    void configure(double inputRateHz);
    void setSignalOffset(double hz);          // not used: the band is received at its centre
    void setCenterHz(double hz);              // the radio's centre (0: a file, assumed 1575.42 MHz)
    bool ready() const;
    void reset();
    void feed(const cf32* x, size_t n);
    bool telemetry(SbasTelemetry& out, uint64_t lastSeq);
    void setLogCallback(std::function<void(const std::string&)> cb);
    void clearMessages();                     // forget the decoded state (the satellites stay tracked)
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

ModeTuning sbasTuning();

// One line for the top bar and the receiver tab
std::string sbasSummary(const SbasTelemetry& t);

// The service a PRN belongs to by its usual assignment ("EGNOS", "WAAS", "MSAS", "GAGAN", "SDCM", "BDSBAS", "KASS"), "" when not one of the well known ones.
// Informal: assignments change; the satellite's own messages are the truth.
const char* sbasServiceOfPrn(int prn);

// Test signal: the GNSS test signal with only the three SBAS satellites of its sky (PRN 123, 127, 128) and the message content of sbas::synthData.
// SynthConfig options as for GNSS (gnss_gen.h); modeOpt[0] is set to SBAS only here.
std::unique_ptr<ModeSynth> makeSbasSynth(const SynthConfig& cfg, double sampleRate);

}  // namespace dect2
