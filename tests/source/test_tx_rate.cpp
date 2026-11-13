// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Generic TX Fs control: automatic selection, manual validation, hardware readback,
// and a complete short session into an in-memory sink. No radio is opened.
#include "dect2/tx/tx.h"
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <limits>
#include <memory>
#include <iostream>
#include <thread>
using namespace dect2;
static int failures = 0;
#define CHECK(c, message) do { if (!(c)) { std::cerr << "FAIL: " << message << '\n'; ++failures; } } while (0)
struct State {
    std::atomic<int> opens{0}, closes{0};
    std::atomic<uint64_t> samples{0};
    std::atomic<bool> opened{false};
    double requested = 0, actual = 0;
};
class MemorySink final : public TxSink {
    std::shared_ptr<State> s_;
    double actual_;
public:
    explicit MemorySink(std::shared_ptr<State> s, double actual = 0) : s_(std::move(s)), actual_(actual) {}
    bool open(const TxTune& t, std::string&) override {
        ++s_->opens; s_->requested = t.sampleRate;
        s_->actual = actual_ > 0 ? actual_ : t.sampleRate; s_->opened = true; return true;
    }
    bool write(const cf32*, size_t n, std::string&) override { s_->samples += n; return true; }
    void close() override { if (s_->opened.exchange(false)) ++s_->closes; }
    double sampleRate() const override { return s_->actual; }
    bool realtime() const override { return false; }
};
static TxRequest request(double rate = 0) {
    TxRequest r; r.modeId = "dab"; r.tune.sampleRate = rate; r.tune.centerHz = 220e6;
    r.loop = false; r.onceSeconds = 0.1; return r;
}
static void rejected(TxRequest r, TxRateLimits lim, const char* text) {
    auto s = std::make_shared<State>(); TxSession tx; std::string err;
    CHECK(!tx.start(std::make_unique<MemorySink>(s), r, lim, err), "invalid TX Fs accepted");
    CHECK(err.find(text) != std::string::npos, "error '" << err << "' lacks '" << text << "'");
    CHECK(s->opens == 0 && s->samples == 0 && !tx.running(), "validation must precede hardware open and samples");
}
static void transmitted(double requested, double readback, double expectedRequest, double expectedRate) {
    auto s = std::make_shared<State>(); TxSession tx; std::string err;
    CHECK(tx.start(std::make_unique<MemorySink>(s, readback), request(requested), {2.1e6, 61.44e6}, err), "start: " << err);
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (tx.running() && std::chrono::steady_clock::now() < until) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    CHECK(!tx.running() && tx.stats().finished && !tx.stats().failed, "short TX session finishes");
    tx.stop();
    CHECK(s->requested == expectedRequest && tx.stats().rateHz == expectedRate, "requested " << s->requested << ", actual " << tx.stats().rateHz);
    CHECK(s->samples > 0 && s->closes == 1, "session sends samples and closes once");
}
int main() {
    const auto* dab = txModeById("dab");
    if (!dab) return 2;
    CHECK(txRequestRate(*dab, request(), {2.1e6, 61.44e6}) == 2.1e6, "automatic rate respects radio minimum");
    CHECK(txRequestRate(*dab, request(4e6), {2.1e6, 3e6}) == 4e6, "manual request is never silently clamped");
    rejected(request(-1), {}, "finite positive");
    rejected(request(std::numeric_limits<double>::quiet_NaN()), {}, "finite positive");
    rejected(request(std::numeric_limits<double>::infinity()), {}, "finite positive");
    rejected(request(2e6), {2.1e6, 61.44e6}, "outside the radio's range");
    rejected(request(8e6), {2.1e6, 4e6}, "outside the radio's range");
    rejected(request(1.9e6), {}, "needs at least");
    TxRequest tv = request(4e6); tv.modeId = "dvbt2"; tv.bandwidthMhz = 8;
    rejected(tv, {}, "needs at least");
    TxRequest isdb = request(6e6); isdb.modeId = "isdbt"; isdb.synth.isdbtBwMhz = 8;
    rejected(isdb, {}, "needs at least");
    transmitted(0, 0, 2.1e6, 2.1e6);
    transmitted(4e6, 0, 4e6, 4e6);
    transmitted(0, 4e6, 2.1e6, 4e6); // automatic may adopt a running RX clock
    transmitted(4e6, 4e6 + 10, 4e6, 4e6 + 10); // normal hardware rounding
    {
        auto s = std::make_shared<State>(); TxSession tx; std::string err;
        CHECK(!tx.start(std::make_unique<MemorySink>(s, 8e6), request(4e6), {2.1e6, 61.44e6}, err), "manual rate may not silently adopt a different clock");
        CHECK(err.find("not the requested TX rate") != std::string::npos && s->closes == 1 && s->samples == 0,
              "different readback closes without sending: " << err);
    }
    printf("TX rate: %s (%d failures)\n", failures ? "FAILED" : "passed", failures);
    return failures ? 1 : 0;
}
