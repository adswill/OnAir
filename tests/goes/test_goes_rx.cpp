// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// GOES HRIT end to end: the synthetic signal (goes_gen.h: files -> packets -> VCDUs -> Reed-Solomon -> randomiser -> convolutional code
// -> BPSK with a root raised cosine, carrier offset, symbol clock offset and noise) into the receiver, at the RTL-SDR rate and at a
// HackRF rate; every product must come back byte for byte (Rice and plain images, the JPEG, the admin and EMWIN texts) and every LRIT
// file must equal the one sent. Noise alone gives nothing.
#include "dect2/goes/goes_gen.h"
#include "dect2/common/stable_random.h"
#include "dect2/goes/goes_rx.h"
#include "dect2/goes/goes_tel.h"
#include "dect2/hfdig/hfdig_hamdrm.h"
#include <chrono>
#include <cstdio>
#include <random>
using namespace dect2;
#include "../common/test_util.h"

static std::shared_ptr<goes::Product> find(const GoesTelemetry& t, int kind) {
    for (const auto& p : t.products) if (p->kind == kind && p->complete) return p;
    return nullptr;
}

static void runCase(const char* name, double rate, double snr, double cfo, double ppm, double seconds, bool invG2 = true, float dcSpike = 0) {
    GoesGenConfig g;
    g.rate = rate; g.snrDb = snr; g.cfoHz = cfo; g.sroPpm = ppm; g.fillFrames = 12; g.seed = 5; g.invertG2 = invG2;
    GoesGenerator gen(g);
    GoesReceiver rx;
    rx.configure(rate);
    std::vector<std::vector<uint8_t>> files;
    rx.setFileCallback([&](const std::vector<uint8_t>& f, int, int) { files.push_back(f); });
    std::vector<cf32> buf(65536);
    GoesTelemetry t;
    uint64_t seq = 0;
    double cpu = 0;
    for (double done = 0; done < seconds * rate; done += (double)buf.size()) {
        gen.generate(buf.data(), buf.size());
        if (dcSpike > 0) for (auto& v : buf) v += cf32(dcSpike, 0.3f * dcSpike);   // the radio's DC spike
        const auto t0 = std::chrono::steady_clock::now();
        rx.feed(buf.data(), buf.size());
        cpu += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        if (rx.telemetry(t, seq)) seq = t.seq;
    }
    printf("  %s: %s; cfo %.0f Hz (sent %.0f), sym %.1f, Es/N0 %.1f dB, lock %.2f, frames %llu ok %llu bad, RS %.3f%%, %llu files, %llu CRC, G2 %s, receiver %.2f s CPU for %.1f s\n",
           name, goesSummary(t).c_str(), t.cfoHz, cfo, t.symbolRate, t.snrDb, t.lockMetric, (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad,
           t.rsByteErrors * 100, (unsigned long long)t.files, (unsigned long long)t.crcErrors, t.invertedG2 ? "inverted" : "plain", cpu, seconds);
    CHECK(t.state == 3 && t.carrierLock, "%s: state %d", name, t.state);
    CHECK(std::fabs(t.cfoHz - cfo) < 300, "%s: carrier %.0f", name, t.cfoHz);
    CHECK(t.invertedG2 == invG2, "%s: G2", name);
    // every file of every round byte exact
    const std::vector<GoesTestFile> tf = goesTestFiles();
    CHECK(files.size() >= tf.size(), "%s: %zu files", name, files.size());
    size_t exact = 0;
    for (const auto& f : files) { bool m = false; for (const auto& x : tf) m |= f == x.file; exact += m; }
    CHECK(exact == files.size(), "%s: %zu of %zu files byte exact", name, exact, files.size());
    int fw, fh, mw, mh;
    const std::vector<uint8_t> fd = goesTestFullDisk(fw, fh), meso = goesTestMeso(mw, mh);
    auto p = find(t, goes::KFullDisk);
    CHECK(p && p->pix == fd && !p->errors, "%s: full disk", name);
    auto m = find(t, goes::KMeso);
    CHECK(m && m->pix == meso && !m->errors, "%s: mesoscale", name);
    auto j = find(t, goes::KImage);
    CHECK(j && j->data == hamdrmTestPicture(), "%s: JPEG", name);
    auto a = find(t, goes::KAdmin);
    CHECK(a && a->text == goesTestAdminText(), "%s: admin", name);
    auto e = find(t, goes::KEmwin);
    CHECK(e && e->text == goesTestEmwinText(), "%s: EMWIN", name);
    CHECK(t.crcErrors == 0 && t.fileErrors == 0, "%s: %llu CRC errors, %llu file errors", name, (unsigned long long)t.crcErrors, (unsigned long long)t.fileErrors);
}

int main() {
    runCase("2.4 Msps, Es/N0 8 dB, +35 kHz, +30 ppm", 2.4e6, 8, 35e3, 30, 1.5);
    runCase("2.4 Msps, Es/N0 4 dB, -12 kHz", 2.4e6, 4, -12e3, -20, 1.5);
    runCase("10 Msps, Es/N0 6 dB, +120 kHz", 10e6, 6, 120e3, 10, 1.2);
    runCase("2.4 Msps, Es/N0 5 dB, G2 not inverted, -60 kHz", 2.4e6, 5, -60e3, 0, 1.5, false);
    runCase("2.4 Msps, Es/N0 30 dB, on the dial (the app's test signal)", 2.4e6, 30, 0, 0, 1.5);
    runCase("2.4 Msps, Es/N0 6 dB, +25 kHz, a strong DC spike", 2.4e6, 6, 25e3, 0, 2.0, true, 0.15f);
    // noise alone
    {
        GoesReceiver rx;
        rx.configure(2.4e6);
        std::mt19937 rng(1);
        dect2::normal_distribution<float> n(0.f, 0.2f);
        std::vector<cf32> buf(65536);
        GoesTelemetry t;
        uint64_t seq = 0;
        for (int k = 0; k < 40; k++) {
            for (auto& v : buf) v = cf32{n(rng), n(rng)};
            rx.feed(buf.data(), buf.size());
            if (rx.telemetry(t, seq)) seq = t.seq;
        }
        CHECK(t.blocksOk == 0 && t.products.empty() && t.state < 2, "noise: state %d, %llu frames", t.state, (unsigned long long)t.blocksOk);
    }
    printf(fails ? "goes rx: %d FAILED\n" : "goes rx: all passed\n", fails);
    return fails ? 1 : 0;
}
