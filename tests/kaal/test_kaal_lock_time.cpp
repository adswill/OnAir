// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// KAAL time to lock: stream time from the first sample to lock and to the first audio sample, for CFO, start offset and SNR.
// Prints the table; checks the bound (stream time, not wall time) at SNR >= 10 dB and offsets up to +-1 kHz, reports the worst case.
#include "dect2/kaal/kaal_rx.h"
#include "dect2/kaal/kaal_sweepid.h"
#include "kaal_rx_gen.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
using namespace dect2;
#include "../common/test_util.h"

static const double kLockBoundS = 1.3;   // first try at 1.25 s (was 3.0 s); 1.5 cleaned sweep periods are the least the estimates run on

struct T { double lock = -1, audio = -1; uint64_t good = 0, bad = 0; };

// lock = the time of the last "acquired" (an early lock that the receiver rejected again does not count), audio = the first audio after it.
static T measure(const std::vector<std::complex<float>>& sig, size_t from) {
    kaal::VoiceRx rx;
    rx.setFastAcquire(true);
    rx.configure(48000.0);
    T t;
    double now = 0;
    const bool verbose = getenv("KLOG") != nullptr;
    rx.setLogCallback([&](const std::string& s) {
        if (verbose) printf("  log %.3f: %s\n", now, s.c_str());
        if (s.rfind("KAAL: acquired", 0) == 0) { t.lock = now; t.audio = -1; }
    });
    std::vector<int16_t> buf(4096);
    for (size_t i = from; i < sig.size(); i += 480) {
        now = (double)(std::min(sig.size(), i + 480) - from) / 48000.0;
        rx.feed(&sig[i], std::min<size_t>(480, sig.size() - i));
        if (t.lock >= 0 && t.audio < 0 && rx.popAudio(buf.data(), buf.size()) > 0) t.audio = now;
        else if (t.lock < 0) while (rx.popAudio(buf.data(), buf.size()) > 0) {}
    }
    const auto s = rx.stats();
    t.good = s.framesGood; t.bad = s.framesBad;
    return t;
}

int main() {
    const std::vector<int16_t> pcm = kaalrx::testPcm(16000 * 6);
    const int kb = 12;
    const std::vector<uint8_t> bits = kaalrx::frameBits(pcm, kb);
    double pw = 0;
    const std::vector<kaalrx::cd> tx = kaalrx::transmit(bits, kb, "", pw);
    double worst = 0;
    printf("%8s %8s %8s %9s %9s %6s %6s\n", "snr dB", "cfo Hz", "skip smp", "lock s", "audio s", "good", "bad");
    for (double snr : {40.0, 10.0, 6.0, 4.0, 2.0, 0.0})
        for (double cfo : {0.0, 300.0, -1000.0, 1000.0})
            for (size_t skip : {(size_t)0, (size_t)7777}) {
                const T t = measure(kaalrx::channelSnr(tx, pw, snr, cfo, 3), skip);
                printf("%8.0f %8.0f %8zu %9.3f %9.3f %6llu %6llu\n", snr, cfo, skip, t.lock, t.audio, (unsigned long long)t.good, (unsigned long long)t.bad);
                if (snr >= 10.0) {
                    worst = std::max(worst, std::max(t.lock, t.audio));
                    CHECK(t.lock > 0 && t.audio > 0, "no lock or audio at snr %.0f cfo %.0f", snr, cfo);
                }
            }
    {   // false-lock guard: noise only (and a sweep with no data) must give no good word and no audio
        std::vector<kaalrx::cd> z(tx.size(), kaalrx::cd(0, 0));
        for (const bool withSweep : {false, true}) {
            std::vector<kaalrx::cd> src(z);
            if (withSweep) for (size_t i = 0; i < src.size(); i++) src[i] = kaal::sweepSampleAt(0, (int)(i % (size_t)kaal::kSwL)) * 0.3;
            const auto sig = kaalrx::channelSnr(src, 1.0, withSweep ? 10.0 : 0.0, 0.0, 9);
            kaal::VoiceRx rx;
            rx.configure(48000.0);
            std::vector<int16_t> buf(4096);
            size_t audio = 0;
            for (size_t i = 0; i < sig.size(); i += 480) {
                rx.feed(&sig[i], std::min<size_t>(480, sig.size() - i));
                audio += rx.popAudio(buf.data(), buf.size());
            }
            printf("no data (%s): good %llu bad %llu audio %zu\n", withSweep ? "sweep only" : "noise only", (unsigned long long)rx.stats().framesGood,
                   (unsigned long long)rx.stats().framesBad, audio);
            CHECK(rx.stats().framesGood == 0, "false lock: %llu good words", (unsigned long long)rx.stats().framesGood);
        }
    }
    printf("worst lock/audio time at SNR >= 10 dB: %.3f s\n", worst);
    CHECK(worst < kLockBoundS, "time to audio %.3f s", worst);
    return testSummary("kaal_lock_time");
}
