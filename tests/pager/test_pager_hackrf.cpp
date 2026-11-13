// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Pagers on HF through a fake HackRF (report: HackRF at 2 Msps, "Auto (2 Msps)", tuned to 6.1625 MHz, the radio at 6.2125 MHz; the POCSAG level
// stayed at -200 dBFS with Whole span off and nothing decoded with it on). Unlike pager_hf (the synthetic source) this goes through the radio path
// of the engine: a DeviceInfo::HackRF, realtimeHardware, the automatic offset tuning, the rate and centre the engine asks the driver for. The fake
// radio delivers the POCSAG 512 test signal where a real antenna would put it: at the frequency the user tuned, seen from the centre the radio was
// set to, at the rate it was set to. Simulated signal only: no real HackRF and no real HF recording.
#include "dect2/engine/engine.h"
#include "dect2/engine/engine_testkit.h"
#include "dect2/engine/mode_synth.h"
#include "dect2/engine/rate_choice.h"
#include "dect2/pager/pager_gen.h"
#include "dect2/pager/pager_rx.h"
#include "dect2/source/source.h"
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
using namespace dect2;
using namespace dect2::enginetest;
#include "../common/test_util.h"

namespace {
std::atomic<double> gUserHz{0};   // where the pager transmitter is
std::mutex gMu;
double gAskedCentre = 0, gAskedRate = 0;

class FakeHackrf : public IqSource {
public:
    bool start(const TuneSettings& s, IqRing& ring, std::string&) override {
        stop();
        rate_ = s.sampleRate > 0 ? s.sampleRate : 2e6;
        centre_ = s.centerHz;
        { std::lock_guard<std::mutex> lk(gMu); gAskedCentre = centre_; gAskedRate = rate_; }
        run_ = true;
        th_ = std::thread([this, &ring] { loop(ring); });
        return true;
    }
    void stop() override {
        run_ = false;
        if (th_.joinable() && th_.get_id() != std::this_thread::get_id()) th_.join();
    }
    bool retune(const TuneSettings& s, std::string&) override {
        centre_ = s.centerHz;
        std::lock_guard<std::mutex> lk(gMu); gAskedCentre = s.centerHz;
        return true;
    }
    double sampleRate() const override { return rate_; }
    bool realtimeHardware() const override { return true; }
    ~FakeHackrf() override { stop(); }

private:
    void loop(IqRing& ring) {
        // the pager generator puts its carrier at cfoHz - tuneOffsetHz (it expects the radio tuneOffsetHz above the channel); the mixer moves it
        // to wherever the user frequency really is seen from this radio's centre
        SynthConfig sc;
        sc.mode = 25; sc.snrDb = 25; sc.cfoHz = 300;
        sc.modeOpt[2] = kPocsag512 + 1;
        auto g = makeModeSynth(25, sc, rate_);
        std::vector<cf32> buf(16384);
        double ph = 0;
        const auto t0 = std::chrono::steady_clock::now();
        uint64_t sent = 0;
        while (run_) {
            if (ring.available() > ring.capacity() / 8) { std::this_thread::sleep_for(std::chrono::milliseconds(1)); continue; }
            // at most 4x real time: a receiver that keeps up never has a backlog the engine would cut (catchUp)
            const double ahead = sent / (rate_ * 4.0) - std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            if (ahead > 0.05) { std::this_thread::sleep_for(std::chrono::milliseconds(2)); continue; }
            g->generate(buf.data(), buf.size());
            const double shift = gUserHz.load() - centre_.load() + pagerTuning().tuneOffsetHz;   // 0 when the radio sits tuneOffsetHz above the user
            const double w = 2 * M_PI * shift / rate_;
            for (cf32& v : buf) { v *= cf32((float)std::cos(ph), (float)std::sin(ph)); ph = std::fmod(ph + w, 2 * M_PI); }
            ring.write(buf.data(), buf.size());
            sent += buf.size();
        }
    }
    double rate_ = 0;
    std::atomic<double> centre_{0};
    std::atomic<bool> run_{false};
    std::thread th_;
};

// what the app leaves in TuneSettings for the mode with the rate on Auto (applyBandwidth): the mode's own rate, filter and width
TuneSettings autoTune(double hz) {
    const ModeTuning mt = pagerTuning();
    TuneSettings t;
    t.centerHz = hz;
    t.bandwidthMhz = mt.bandwidthMhz; t.sampleRate = mt.sampleRate; t.basebandFilterHz = mt.basebandHz;
    return t;
}

void run(double hz, bool wide, bool autoOffset) {
    gUserHz = hz;
    Engine e;
    DeviceInfo dev; dev.kind = DeviceInfo::HackRF; dev.board = "HackRF Pro"; dev.name = "HackRF Pro (fake)"; dev.minRateHz = 2e6; dev.maxRateHz = 20e6;
    e.setAutoOffset(autoOffset);
    e.setStandard(25);
    e.setWideVoice(wide);
    FileOptions fo;
    const TuneSettings t = autoTune(hz);
    CHECK(e.start(dev, t, fo), "%.4f MHz: engine start", hz / 1e6);
    double centre, rate;
    { std::lock_guard<std::mutex> lk(gMu); centre = gAskedCentre; rate = gAskedRate; }
    const auto want = pagerTestMessages(0, kPocsag512 + 1);
    CHECK(!want.empty(), "no POCSAG 512 test messages");
    RxTelemetry rx; uint64_t seq = 0;
    auto found = [&] {
        int n = 0;
        if (!rx.pager.messages) return 0;
        for (const auto& w : want) for (const auto& m : *rx.pager.messages) if (m.address == w.address && m.text == w.text) { n++; break; }
        return n;
    };
    waitFor(120, [&] { if (e.latestRx(rx, seq)) seq = rx.seq; return found() == (int)want.size() && signalSecs(e) > 2; });
    e.latestRx(rx, 0);
    const double rxRate = e.sampleRate();
    e.stop();
    printf("HackRF pager %.4f MHz whole span %d auto offset %d: radio at %.4f MHz %.3f Msps, receiver %.3f Msps, level %.1f dBFS, %.3f Msps in, %d of %zu pages\n",
           hz / 1e6, wide, autoOffset, centre / 1e6, rate / 1e6, rxRate / 1e6, rx.pager.levelDb, rx.pager.inputRate / 1e6, found(), want.size());
    CHECK(rate >= pagerTuning().minSampleRate, "%.4f MHz: the radio was asked for %.3f Msps", hz / 1e6, rate / 1e6);
    if (!wide) CHECK(rx.pager.levelDb > -150, "%.4f MHz whole span off: level %.1f dBFS (no samples reached the pager receiver)", hz / 1e6, rx.pager.levelDb);
    CHECK(found() == (int)want.size(), "%.4f MHz whole span %d auto offset %d: %d of %zu pages", hz / 1e6, wide, autoOffset, found(), want.size());
}
}  // namespace

int main() {
    setTestSourceFactory([](const DeviceInfo&) { return std::unique_ptr<IqSource>(new FakeHackrf()); });
    for (double hz : {6.1625e6, 466.075e6})
        for (bool wide : {false, true}) run(hz, wide, true);
    run(6.1625e6, false, false);
    return testSummary("pager_hackrf");
}
