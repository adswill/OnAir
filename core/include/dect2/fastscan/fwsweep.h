// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// HackRF firmware sweep for the fast scanner (receive only). libhackrf's public sweep API (hackrf_init_sweep / hackrf_start_rx_sweep) makes the
// firmware retune by itself and tag every 16 KiB block with 0x7F 0x7F and the tuned frequency, so the host pays no USB round trip per hop. The
// scanner then treats each block as one hop capture (FastScanner::runBlocks). Falls back to the ordinary retune loop when the firmware or the
// host library does not support it. The packet parser is pure code (tests/fastscan/test_fastscan.cpp feeds it synthetic packets).
#pragma once
#include "dect2/fastscan/scanner.h"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace dect2 {
namespace fastscan {

constexpr size_t kSweepBlockBytes = 16384;   // libhackrf BYTES_PER_BLOCK: 10 bytes of header (0x7F 0x7F + uint64 LE Hz), padded to 16, then int8 IQ

// Splits a buffer of sweep blocks. The header frequency is the low edge of the tuning; the radio is centred offsetHz above it.
// cb(centerHz, samples, n) is called per valid block. Blocks without the 0x7F7F marker are skipped. Returns the number of valid blocks.
size_t parseSweepBuffer(const uint8_t* buf, size_t len, double offsetHz, const std::function<void(double, const cf32*, size_t)>& cb);

// The plan for a firmware sweep: the radio runs at 20 Msps, hops are 15 MHz wide and start on a whole MHz; step = hop width, offset = 7.5 MHz.
struct FwSweepPlan {
    FastPlanConfig plan;             // bands rounded out to the hop grid, 20 Msps profile
    uint16_t loMhz = 0, hiMhz = 0;   // the range handed to hackrf_init_sweep
    double stepHz = 15e6, offsetHz = 7.5e6;
};
FwSweepPlan planFirmwareSweep(const FastPlanConfig& wanted, const RadioProfile& r);

class HackrfFwSweep : public BlockSource {
public:
    ~HackrfFwSweep() override { close(); }
    // Opens the first HackRF (or the serial) through libhackrf, applies the gains and starts the sweep. false and err when the radio is missing, busy
    // or its firmware lacks sweep support (the caller then uses the retune loop).
    bool open(const FwSweepPlan& p, double lnaDb, double vgaDb, bool amp, const std::string& serial, std::string& err);
    void close();
    double sampleRate() const override { return 20e6; }
    bool next(double& centerHz, std::vector<cf32>& out, const std::atomic<bool>* cancel, std::string& err) override;
    uint64_t dropped() const { return dropped_.load(); }
private:
    static int cb(void* transfer);
    void push(double c, const cf32* s, size_t n);
    void* dev_ = nullptr;
    double offsetHz_ = 7.5e6;
    struct Blk { double c; std::vector<cf32> s; };
    std::mutex m_;
    std::condition_variable cv_;
    std::deque<Blk> q_;
    std::atomic<uint64_t> dropped_{0};
    bool open_ = false;
};

}  // namespace fastscan
}  // namespace dect2
