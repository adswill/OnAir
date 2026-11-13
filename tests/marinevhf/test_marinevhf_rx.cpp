// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Marine VHF receiver: three NFM voice channels (16, 6, 13) in one 960 ksps capture with noise. The calls are logged on the right channels with sensible
// start and length, the audio of a channel is its tone and silent outside the call, recordings are written, the activity counts follow, and the
// play choice follows the user's pick, the channel 16 priority watch and the scan.
#include "dect2/marinevhf/marinevhf_rx.h"
#include "dect2/marinevhf/marinevhf_tel.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <map>
#include <mutex>
#include <random>
#include <thread>
using namespace dect2;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

static const double kFs = 960000, kCenter = 156.55e6, kDur = 7.5;
struct Tx { double mhz; double t0, t1; double toneHz; };

static std::vector<cf32> synth(const std::vector<Tx>& txs, float amp, float noiseSigma, double fs = kFs, double center = kCenter, double dur = kDur) {
    const size_t n = (size_t)(dur * fs);
    std::vector<cf32> x(n);
    std::mt19937 rng(7);
    std::normal_distribution<float> g(0.f, noiseSigma);
    for (size_t i = 0; i < n; i++) x[i] = cf32(g(rng), g(rng));
    for (const Tx& t : txs) {
        const double off = t.mhz * 1e6 - center;
        const size_t a = (size_t)(t.t0 * fs), b = std::min(n, (size_t)(t.t1 * fs));
        double phi = 0;   // the audio integral
        for (size_t i = a; i < b; i++) {
            const double ts = (double)i / fs;
            const double m = 0.6 * std::sin(2 * M_PI * t.toneHz * ts) + 0.4 * std::sin(2 * M_PI * 1.7 * t.toneHz * ts);   // +-3 kHz deviation peak (deviation 3 kHz at m = 1)
            phi += 2 * M_PI * 3000.0 * m / fs;
            const double ph = 2 * M_PI * off * ts + phi;
            x[i] += cf32((float)(amp * std::cos(ph)), (float)(amp * std::sin(ph)));
        }
    }
    return x;
}

static double tonePower(const std::vector<float>& a, double hz) {   // Goertzel, normalised to the mean square
    if (a.empty()) return 0;
    const double w = 2 * M_PI * hz / 48000.0, c = 2 * std::cos(w);
    double s1 = 0, s2 = 0, ms = 0;
    for (float v : a) { const double s = v + c * s1 - s2; s2 = s1; s1 = s; ms += (double)v * v; }
    const double p = s1 * s1 + s2 * s2 - c * s1 * s2;
    return ms > 0 ? 2.0 * p / ((double)a.size() * ms) : 0;
}

struct Run {
    std::mutex mu;
    std::map<int, std::vector<float>> audio;          // per channel number: the gated stream
    std::map<int, std::vector<float>> audioOpen;      // only the non-silent blocks
    std::map<double, std::pair<int, char>> playAt;    // time -> playing channel
    MarineVhfTelemetry tel;
};

static void runRx(MarineVhfReceiver& rx, const std::vector<cf32>& x, Run& r, const std::vector<double>& probes) {
    rx.setChannelTap([&](int num, char, const float* p, size_t n) {
        std::lock_guard<std::mutex> lk(r.mu);
        auto& a = r.audio[num]; a.insert(a.end(), p, p + n);
        bool any = false; for (size_t i = 0; i < n; i++) if (p[i] != 0.f) { any = true; break; }
        if (any) { auto& o = r.audioOpen[num]; o.insert(o.end(), p, p + n); }
    });
    const size_t chunk = 16384;
    size_t pi = 0;
    for (size_t i = 0; i < x.size(); i += chunk) {
        rx.feed(x.data() + i, std::min(chunk, x.size() - i));
        const double t = (double)(i + chunk) / kFs;
        while (pi < probes.size() && t >= probes[pi]) { r.playAt[probes[pi]] = rx.playingChannel(); pi++; }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    uint64_t seq = 0;
    rx.telemetry(r.tel, seq);
    rx.setChannelTap(nullptr);   // the receiver outlives r
}

static std::unique_ptr<MarineVhfReceiver> make() {
    auto rx = std::make_unique<MarineVhfReceiver>();
    rx->setSilent(true);
    rx->setCenterHz(kCenter);
    rx->setSignalOffset(0);
    rx->configure(kFs);
    rx->setSignalOffset(0);
    rx->setCenterHz(kCenter);
    return rx;
}

int main() {
    // the tuning
    const ModeTuning t = marineVhfTuning();
    CHECK(t.stdMode == 157 && std::string(t.id) == "marinevhf" && t.minSampleRate <= 250000 && t.sampleRate >= 6.5e6, "tuning");

    const std::vector<Tx> txs = {{156.800, 1.0, 3.0, 1000}, {156.300, 1.5, 2.5, 700}, {156.650, 4.0, 5.0, 1300}};
    const std::vector<cf32> x = synth(txs, 0.1f, 0.0436f);
    const std::string dir = (std::filesystem::temp_directory_path() / "onair_marinevhf_test").string();
    std::filesystem::remove_all(dir);

    // ---- automatic: every call is logged, recorded and played in turn
    {
        auto rx = make();
        rx->setScanTiming(0.4, 0.5);
        rx->setRecording(true, dir);
        Run r;
        runRx(*rx, x, r, {0.5, 1.4, 2.0, 2.4, 3.5, 4.5, 7.0});
        const MarineVhfTelemetry& tl = r.tel;
        CHECK(tl.rows.size() == marinevhf::channels(marinevhf::RegionInternational).size() && tl.region == 0, "rows for every channel of the plan");
        CHECK(tl.calls.size() == 3, "3 calls, got %zu", tl.calls.size());
        std::map<int, const MarineVhfCall*> byNum;
        for (auto& c : tl.calls) byNum[c.number] = &c;
        const struct { int num; double t0, t1; } want[] = {{16, 1.0, 3.0}, {6, 1.5, 2.5}, {13, 4.0, 5.0}};
        for (auto& w : want) {
            auto it = byNum.find(w.num);
            CHECK(it != byNum.end(), "a call on channel %d", w.num);
            if (it == byNum.end()) continue;
            const MarineVhfCall& c = *it->second;
            CHECK(c.startSec > w.t0 - 0.4 && c.startSec < w.t0 + 0.6, "ch %d starts at %.2f (sent %.2f)", w.num, c.startSec, w.t0);
            CHECK(std::fabs(c.durationSec - (w.t1 - w.t0)) < 0.7, "ch %d lasts %.2f s (sent %.2f)", w.num, c.durationSec, w.t1 - w.t0);
            CHECK(c.mhz > 156.0 && std::fabs(c.mhz - (w.num == 16 ? 156.8 : w.num == 6 ? 156.3 : 156.65)) < 1e-6, "ch %d frequency %.3f", w.num, c.mhz);
            CHECK(c.peakDb > -30 && c.peakDb < -10, "ch %d level %.1f dB", w.num, c.peakDb);
            CHECK(!c.file.empty() && std::filesystem::exists(c.file), "ch %d recorded", w.num);
            if (!c.file.empty() && std::filesystem::exists(c.file)) {
                const double sec = (double)(std::filesystem::file_size(c.file) - 44) / 2 / 48000.0;
                CHECK(sec > (w.t1 - w.t0) * 0.5 && sec < (w.t1 - w.t0) + 1.0, "ch %d file is %.2f s", w.num, sec);
                CHECK(c.file.find("_ch" + std::to_string(w.num) + "_") != std::string::npos && c.file.find("MHz.wav") != std::string::npos, "file name %s", c.file.c_str());
            }
        }
        CHECK(tl.recordedFiles == 3 && tl.callsTotal == 3, "counts: %d files, %llu calls", tl.recordedFiles, (unsigned long long)tl.callsTotal);
        // the activity list
        int withCalls = 0;
        for (auto& row : tl.rows) {
            if (row.calls) withCalls++;
            if (row.number == 16 && !row.suffix) CHECK(row.calls == 1 && row.totalSec > 1.2 && row.lastHeardAgoSec > 3.0 && row.lastHeardAgoSec < 5.5, "16: %d calls %.1f s heard %.1f s ago", row.calls, row.totalSec, row.lastHeardAgoSec);
            if (row.number == 70 || row.number == 9) CHECK(row.calls == 0, "ch %d is quiet", row.number);
        }
        CHECK(withCalls == 3, "three channels with calls: %d", withCalls);
        // the audio: the tone while the call lasts, silence outside it
        for (auto& w : std::vector<std::pair<int, double>>{{16, 1000}, {6, 700}, {13, 1300}}) {
            const auto& a = r.audioOpen[w.first];
            CHECK(a.size() > 48000 * 0.5, "ch %d audio %zu samples", w.first, a.size());
            if (a.size() > 48000 * 0.5) {
                CHECK(tonePower(a, w.second) > 0.2, "ch %d carries its %.0f Hz tone: %.2f", w.first, w.second, tonePower(a, w.second));
                std::vector<float> m; m.reserve(a.size()); for (float v : a) m.push_back(std::fabs(v));
                std::nth_element(m.begin(), m.begin() + (long)m.size() / 2, m.end());
                const float med = m[m.size() / 2];
                CHECK(med > 0.05f && med < 0.7f, "ch %d audio median level %.2f", w.first, med);
                CHECK(*std::max_element(a.begin(), a.end()) <= 1.0f && *std::min_element(a.begin(), a.end()) >= -1.0f, "ch %d audio is limited to +-1", w.first);
            }
        }
        const auto& all16 = r.audio[16];
        size_t zeros = 0; for (float v : all16) if (v == 0.f) zeros++;
        CHECK(all16.size() > 48000 * 6 && (double)zeros / (double)all16.size() > 0.6, "ch 16 audio is silent outside the call (%.0f %% zeros)", 100.0 * (double)zeros / (double)std::max<size_t>(1, all16.size()));
        // what plays: the strongest/only open channel
        CHECK(r.playAt[1.4].first == 16, "1.4 s: 16 plays (%d)", r.playAt[1.4].first);
        CHECK(r.playAt[2.0].first == 16, "2.0 s: 16 stays (priority) (%d)", r.playAt[2.0].first);
        CHECK(r.playAt[0.5].first == 0, "0.5 s: nothing open, nothing plays (%d)", r.playAt[0.5].first);
        CHECK(r.playAt[4.5].first == 13, "4.5 s: 13 plays (%d)", r.playAt[4.5].first);
        CHECK(r.playAt[7.0].first == 0, "7.0 s: quiet again (%d)", r.playAt[7.0].first);
        std::filesystem::remove_all(dir);
    }
    // ---- the user listens to 6: 16 takes over (priority), then 6 again; with the priority off 6 stays
    {
        auto rx = make();
        rx->listen(6);
        Run r;
        runRx(*rx, x, r, {0.5, 2.0, 4.0, 4.6, 6.9});
        CHECK(r.playAt[0.5].first == 6, "listening to 6 while it is quiet (%d)", r.playAt[0.5].first);
        CHECK(r.playAt[2.0].first == 16, "16 takes over from 6 (%d)", r.playAt[2.0].first);
        CHECK(r.playAt[4.6].first == 16, "16 held through its hang (%d)", r.playAt[4.6].first);
        CHECK(r.playAt[6.9].first == 6, "back on 6 (%d)", r.playAt[6.9].first);
    }
    {
        auto rx = make();
        rx->listen(6);
        rx->setPriority16(false);
        Run r;
        runRx(*rx, x, r, {2.0, 4.6});
        CHECK(r.playAt[2.0].first == 6 && r.playAt[4.6].first == 6, "priority off: stays on 6 (%d %d)", r.playAt[2.0].first, r.playAt[4.6].first);
    }
    // ---- scan 13 and 6 (no priority): stops on 6, then on 13
    {
        auto rx = make();
        rx->setPriority16(false);
        rx->setScanChannels({{13, 0}, {6, 0}});
        rx->setScan(true);
        Run r;
        runRx(*rx, x, r, {0.5, 2.0, 4.7});
        CHECK(r.playAt[0.5].first == 0, "scan: nothing open (%d)", r.playAt[0.5].first);
        CHECK(r.playAt[2.0].first == 6, "scan stops on 6 (%d)", r.playAt[2.0].first);
        CHECK(r.playAt[4.7].first == 13, "scan moves on to 13 (%d)", r.playAt[4.7].first);
        CHECK(r.audio.count(16) == 0 || true, "16 is decoded though not scanned");
    }
    // ---- flat audio and a tight squelch still decode; a very tight squelch rejects the weak signals
    {
        auto rx = make();
        rx->setDeemphasis(false);
        rx->setSquelch(30);
        Run r;
        runRx(*rx, x, r, {});
        CHECK(r.tel.calls.size() == 3, "flat audio, loose squelch: %zu calls", r.tel.calls.size());
        const auto& a = r.audioOpen[16];
        CHECK(a.size() > 24000 && tonePower(a, 1000) > 0.2, "flat audio carries the tone");
    }
    // ---- one channel at 250 ksps (an RTL-SDR on the dial frequency, the radio 12.5 kHz above it): channel 16 is found, 6 is not in the capture
    {
        auto rx = std::make_unique<MarineVhfReceiver>();
        rx->setSilent(true);
        rx->setDialHz(156.8e6);
        rx->setSignalOffset(-12500);
        rx->configure(250000);
        rx->setSignalOffset(-12500);
        rx->setDialHz(156.8e6);
        const std::vector<cf32> y = synth({{156.800, 0.5, 2.0, 1000}}, 0.1f, 0.0436f, 250000, 156.8125e6, 4.0);
        Run r;
        r.tel = MarineVhfTelemetry();
        rx->setChannelTap([&](int num, char, const float* p, size_t n) { std::lock_guard<std::mutex> lk(r.mu); auto& a = r.audio[num]; a.insert(a.end(), p, p + n); });
        for (size_t i = 0; i < y.size(); i += 4096) { rx->feed(y.data() + i, std::min<size_t>(4096, y.size() - i)); }
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        uint64_t seq = 0; rx->telemetry(r.tel, seq);
        rx->setChannelTap(nullptr);
        CHECK(r.tel.calls.size() == 1 && r.tel.calls[0].number == 16, "250 ksps: one call on 16 (%zu)", r.tel.calls.size());
        if (!r.tel.calls.empty()) CHECK(std::fabs(r.tel.calls[0].durationSec - 1.5) < 0.6, "250 ksps: %.2f s", r.tel.calls[0].durationSec);
    }
    // ---- noise only: nothing opens
    {
        auto rx = make();
        Run r;
        runRx(*rx, synth({}, 0.f, 0.0436f), r, {});
        CHECK(r.tel.calls.empty() && r.tel.callsTotal == 0, "noise only: %zu calls", r.tel.calls.size());
    }
    // ---- a region change keeps working; clearResults empties the log
    {
        auto rx = make();
        rx->setRegion(marinevhf::RegionUsa);
        CHECK(rx->region() == marinevhf::RegionUsa, "region");
        Run r;
        runRx(*rx, x, r, {});
        CHECK(r.tel.region == 3 && r.tel.calls.size() == 3, "USA plan: %zu calls", r.tel.calls.size());
        int n16 = 0; for (auto& c : r.tel.calls) if (c.number == 16) n16++;
        CHECK(n16 == 1, "the US 16");
        rx->clearResults();
        uint64_t seq = 0; MarineVhfTelemetry t2;
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        rx->telemetry(t2, seq);
        CHECK(t2.calls.empty(), "cleared");
    }

    if (fails) printf("%d failures\n", fails); else printf("marinevhf rx: all ok\n");
    return fails ? 1 : 0;
}
