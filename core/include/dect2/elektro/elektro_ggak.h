// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Elektro-L / Arktika-M GGAK (heliogeophysical) L-band downlink: PCM/PM, 5 ksym/s, 1693 MHz.
// 224-byte CADU, CCSDS ASM 0x1ACFFC1D, no convolutional code and no Reed-Solomon (the frame is sent twice).
// Format from the public GGAK-Worky notes (SatDump / amateur reverse engineering). Receive only.
#pragma once
#include "dect2/common/ring.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace dect2 {

constexpr int kGgakFrameBytes = 224;
constexpr int kGgakFrameBits = kGgakFrameBytes * 8;
constexpr uint32_t kGgakAsm = 0x1ACFFC1Du;
constexpr double kGgakSymbolRate = 5000.0;
constexpr double kGgakMhz = 1693.0;

uint16_t ggakCrc16(const uint8_t* d, size_t n);   // CRC-16/IBM 0x8005, init 0xFFFF, as tried on the captured CADUs
void ggakPutCrc(uint8_t frame[kGgakFrameBytes]);
bool ggakCrcOk(const uint8_t frame[kGgakFrameBytes]);

struct GgakFrame {
    uint8_t raw[kGgakFrameBytes]{};
    int marker = 0, sub = 0;
    uint64_t timestamp = 0;
    bool crcOk = false;
    bool duplicate = false;          // the second copy of FEC-ski
    float bx = 0, by = 0, bz = 0;    // first int16 triple after the header, nT-sized if it looks like FM-VE (unverified)
    bool magOk = false;
    double timeSec = 0;
};

struct GgakTelemetry {
    uint64_t seq = 0;
    int state = 0;                   // 0 searching, 1 carrier, 2 frames
    double cfoHz = 0;
    float snrDb = 0, levelDb = -200, lockMetric = 0;
    bool dataValid = false, carrierLock = false, frameLock = false;
    uint64_t blocksOk = 0, blocksBad = 0;
    double timeSec = 0, inputRate = 0, symbolRate = 0;
    std::vector<cf32> constellation;
    std::vector<float> snrHist, bxHist, byHist, bzHist;
    std::vector<GgakFrame> frames;   // newest first
    std::vector<std::string> log;
    int lastMarker = -1, lastSub = -1;
    uint64_t lastTimestamp = 0;
    float bx = 0, by = 0, bz = 0, bmag = 0;
    bool magOk = false;
};

void ggakParse(GgakFrame& f);
void ggakBuildTestFrame(uint8_t out[kGgakFrameBytes], int marker, int sub, uint64_t timestamp, int16_t bx, int16_t by, int16_t bz);

class GgakReceiver {
public:
    GgakReceiver();
    ~GgakReceiver();
    void configure(double inputRateHz);
    void setSignalOffset(double hz);
    bool ready() const;
    void reset();
    void feed(const cf32* x, size_t n);
    bool telemetry(GgakTelemetry& out, uint64_t lastSeq);
    void clearResults();
    std::string saveResults() const;
    bool loadResults(const std::string& blob);
    void setLogCallback(std::function<void(const std::string&)> cb);

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

struct GgakGenConfig {
    double rate = 240e3;
    double symbolRate = kGgakSymbolRate;
    double snrDb = 20;
    double cfoHz = 0;
    double sroPpm = 0;
    double rolloff = 0.5;
    double amplitude = 0.2;
    uint32_t seed = 1;
};

class GgakGenerator {
public:
    explicit GgakGenerator(const GgakGenConfig& c);
    ~GgakGenerator();
    void generate(cf32* out, size_t n);
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

} // namespace dect2
