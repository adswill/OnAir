// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/railway/railway_modem.h"
#include "dect2/railway/railway_gen.h"
#include "dect2/railway/railway_rx.h"
#include "../common/test_util.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cmath>
using namespace dect2;
using namespace dect2::railway;
static std::vector<float> audio(const std::vector<uint8_t>& bits, double baud = 1200) {
    std::vector<float> samples; double phase = 0;
    const size_t n = size_t(std::ceil(bits.size() * 48000 / baud));
    for (size_t i = 0; i < n; i++) {
        const size_t at = std::min(bits.size() - 1, size_t(i * baud / 48000));
        phase += 2 * 3.14159265358979323846 * (bits[at] ? 1200 : 1800) / 48000;
        samples.push_back(float(std::sin(phase) + 0.8)); // discriminator frequency offset becomes DC
    }
    samples.resize(samples.size() + 4800, 0); return samples;
}
int main() {
    for (auto protocol : {Protocol::Klw, Protocol::Tdcs}) for (bool inverted : {false, true}) {
        const auto expected = demoFrame(protocol); const auto samples = audio(airBits(expected, inverted));
        for (size_t chunk : {size_t(1), size_t(37), size_t(4096), samples.size()}) {
            AudioDecoder decoder; std::vector<Message> found;
            for (size_t i = 0; i < samples.size(); i += chunk) decoder.feed(samples.data() + i, std::min(chunk, samples.size() - i), found);
            CHECK(found.size() == 1 && found[0].raw == expected, "audio protocol %d inverted %d chunk %zu: %zu frames", int(protocol), inverted, chunk, found.size());
        }
        // The same wire frame is a new reception after the short phase-dedup window.
        AudioDecoder repeated; std::vector<Message> found;
        repeated.feed(samples.data(), samples.size(), found); repeated.feed(samples.data(), samples.size(), found);
        CHECK(found.size() == 2, "real repeats survive phase deduplication");
    }
    for (double skew : {0.997, 1.003}) {
        const auto samples = audio(airBits(demoFrame(Protocol::Tdcs)), 1200 * skew);
        AudioDecoder decoder; std::vector<Message> found;
        for (size_t i = 0; i < samples.size(); i += 997) decoder.feed(samples.data() + i, std::min(size_t(997), samples.size() - i), found);
        CHECK(found.size() == 1 && found[0].train == "T112", "symbol clock drift %.3f", skew);
    }
    for (auto protocol : {Protocol::Klw, Protocol::Tdcs}) for (double rate : {240000.0, 960000.0}) for (int variant = 0; variant < 5; variant++) {
        const double offsets[] = {0, 1800, -2200, 8000, -8000};
        SynthConfig cfg; cfg.snrDb = variant >= 3 ? 15 : 25; cfg.cfoHz = offsets[variant]; cfg.modeOpt[1] = variant == 2;
        auto synth = makeRailwaySynth(cfg, rate, protocol); RailwayReceiver rx(protocol); rx.configure(rate); rx.setSignalOffset(-50000);
        std::vector<cf32> samples(size_t(rate * 2.2)); synth->generate(samples.data(), samples.size());
        const size_t chunk = variant == 1 ? 37 : 32768;
        for (size_t i = 0; i < samples.size(); i += chunk) rx.feed(samples.data() + i, std::min(chunk, samples.size() - i));
        RailwayTelemetry t; rx.telemetry(t, 0);
        CHECK(t.messages.size() >= 2 && t.messages[0].raw == demoFrame(protocol), "IQ protocol %d rate %.0f variant %d: %zu frames", int(protocol), rate, variant, t.messages.size());
        const auto saved = rx.saveResults(); RailwayReceiver restored(protocol);
        CHECK(restored.loadResults(saved), "saved frame CRC round trip");
        RailwayTelemetry loaded; restored.telemetry(loaded, 0);
        CHECK(loaded.messages.size() == t.messages.size(), "results rows");
        RailwayReceiver other(protocol == Protocol::Klw ? Protocol::Tdcs : Protocol::Klw);
        CHECK(!other.loadResults(saved), "reject another protocol's results");
        rx.clearMessages(); rx.telemetry(t, 0); CHECK(t.messages.empty() && !t.dataValid, "clear");
        rx.reset(); rx.telemetry(t, 0); CHECK(t.blocksOk == 0, "reset counters");
    }
    // 4.1 sensitivity: SNR in 25 kHz (the generator's definition), 960 kHz, carrier offsets 0, +2 kHz and -6 kHz, two noise seeds; a trial passes
    // with 2 frames in 3.2 s (3 whole TDCS bursts; the first starts with the file, before the AFC has 200 ms of noise to set its floor on). Before 4.1 (the +-14 kHz path alone) about 7 dB here (12 dB in the QA sweep's 7.7 kHz).
    for (auto protocol : {Protocol::Klw, Protocol::Tdcs}) for (double cfo : {0.0, 2000.0, -6000.0}) {
        const double snrs[] = {2, 3, 4, 5, 6, 8, 10, 12};
        double thr = 99;
        for (int k = 7; k >= 0; k--) {
            int ok = 0;
            for (int seed = 1; seed <= 2; seed++) {
                SynthConfig cfg; cfg.snrDb = snrs[k]; cfg.cfoHz = cfo; cfg.modeOpt[3] = seed + 10;
                auto synth = makeRailwaySynth(cfg, 960000, protocol); RailwayReceiver rx(protocol); rx.configure(960000); rx.setSignalOffset(-50000);
                std::vector<cf32> samples(size_t(960000 * 3.2)); synth->generate(samples.data(), samples.size());
                for (size_t i = 0; i < samples.size(); i += 32768) rx.feed(samples.data() + i, std::min(size_t(32768), samples.size() - i));
                RailwayTelemetry t; rx.telemetry(t, 0);
                bool good = t.messages.size() >= 2;
                for (const auto& m : t.messages) good = good && m.raw == demoFrame(protocol);
                ok += good;
            }
            printf("  sens protocol %d cfo %+5.0f snr %4.1f dB: %d of 2\n", int(protocol), cfo, snrs[k], ok);
            if (ok == 2) thr = snrs[k]; else break;
        }
        printf("  railway threshold (25 kHz SNR) protocol %d cfo %+.0f: %.0f dB\n", int(protocol), cfo, thr);
        CHECK(thr <= 6, "protocol %d cfo %+.0f: needs %.0f dB in 25 kHz (4.1 limit 6)", int(protocol), cfo, thr);
    }
    // IQ noise alone (two seeds, 10 s each, with the CPU time): no frame
    for (auto protocol : {Protocol::Klw, Protocol::Tdcs}) {
        size_t frames = 0; double cpu = 0;
        for (int seed = 1; seed <= 2; seed++) {
            SynthConfig cfg; cfg.snrDb = -40; cfg.modeOpt[3] = seed + 20;
            auto synth = makeRailwaySynth(cfg, 960000, protocol); RailwayReceiver rx(protocol); rx.configure(960000); rx.setSignalOffset(-50000);
            std::vector<cf32> samples(32768);
            for (size_t done = 0; done < size_t(960000 * 10); done += samples.size()) {
                synth->generate(samples.data(), samples.size());
                const auto t0 = std::chrono::steady_clock::now();
                rx.feed(samples.data(), samples.size());
                cpu += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            }
            RailwayTelemetry t; rx.telemetry(t, 0); frames += t.messages.size();
        }
        printf("  noise only protocol %d: %zu frames in 2 x 10 s, %.3f s CPU per s of 960 kHz input\n", int(protocol), frames, cpu / 20);
        CHECK(frames == 0, "IQ noise protocol %d: %zu frames", int(protocol), frames);
    }
    // Noise must not produce a decoded application frame.
    AudioDecoder decoder; std::vector<float> noise(48000 * 2); unsigned seed = 77;
    for (auto& s : noise) { seed = seed * 1664525u + 1013904223u; s = float(int(seed >> 16) - 32768) / 32768; }
    std::vector<Message> found; decoder.feed(noise.data(), noise.size(), found); CHECK(found.empty(), "audio noise");
    return testSummary("test_railway_rx");
}
