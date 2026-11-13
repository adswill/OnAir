// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// HF military / government waveform identifier in the engine: the radio's upper-sideband audio (12 kHz) goes through milhf_rx.h.
// Engine standard 147 (family 145, "HF waveform ID"). IDENTIFICATION ONLY: nothing is decoded, no message content is read,
// no encryption is touched. The candidates are estimates from measured physical-layer features.
#pragma once
#include "dect2/engine/mode_tuning.h"
#include "dect2/milhf/milhf_rx.h"
#include <complex>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace dect2 {

constexpr int kMilhfRows = 80;     // spectrogram snippet: rows of 0.2 s
constexpr int kMilhfCols = 153;    // 0 - 3.6 kHz

struct MilhfTelemetry {
    uint64_t seq = 0;
    int state = 0;                  // 0 no input, 1 collecting audio, 2 analysed
    double cfoHz = 0;               // not measured (the common fields every mode reports)
    bool dataValid = false;         // a signal was found (never means decoded: nothing is decoded)
    float snrDb = 0;
    uint64_t blocksOk = 0, blocksBad = 0;   // analyses done / not used
    bool analysed = false;
    milhf::Result res;              // features and ranked candidates (estimates)
    float levelDb = -200;           // audio level, dBFS
    double audioRate = 0;           // the audio rate the analysis ran at
    double audioSecs = 0;           // seconds of audio held
    int rows = 0;                   // filled spectrogram rows (oldest first)
    std::vector<uint8_t> wf;        // rows x kMilhfCols, 0..255 (dB scaled)
    double inputRate = 0;
    double centerHz = 0;
};

class MilhfReceiver {
public:
    MilhfReceiver();
    ~MilhfReceiver();

    void configure(double inputRateHz);
    void setSignalOffset(double hz);     // where the dial sits relative to 0 Hz in the input (about -tuneOffsetHz)
    void setCenterHz(double hz);
    bool ready() const;
    void reset();
    void feed(const std::complex<float>* x, size_t n);
    bool telemetry(MilhfTelemetry& out, uint64_t lastSeq);
    void setLogCallback(std::function<void(const std::string&)> cb);
    // The last (up to) 20 s of audio as a 16-bit mono WAV, for a public signal-identification wiki. Empty audio fails with err set.
    bool saveClip(const std::string& path, std::string& err);

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

ModeTuning milhfTuning();

// One line for the top bar and the receiver tab
std::string milhfSummary(const MilhfTelemetry& t);

}  // namespace dect2
