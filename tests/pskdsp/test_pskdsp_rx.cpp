// PSK demodulator receiver and mode wiring: the numbers and the registry, the engine's idle snapshot, and the whole receiver on generated
// signals (blind analysis in the background, then the streaming chain): rate, modulation, bits behind the sync word, settings, and a retune.
#include "dect2/engine/engine.h"
#include "dect2/pskdsp/pskdsp_rx.h"
#include "dect2/pskdsp/pskdsp.h"
#include "../common/test_util.h"
#include <chrono>
#include <set>
#include <thread>
using namespace dect2;
using pskd::Bits;

// feed the capture again and again (a loop of the same frames) until the receiver has followed the signal and found enough sync words
static bool follow(PskdReceiver& rx, const std::vector<cf32>& cap, PskdTelemetry& t, uint64_t wantSyncs) {
    uint64_t seq = 0;
    for (int pass = 0; pass < 60; pass++) {
        for (size_t i = 0; i < cap.size(); i += 4800) rx.feed(cap.data() + i, std::min<size_t>(4800, cap.size() - i));
        rx.telemetry(t, 0);
        (void)seq;
        if (t.syncs >= wantSyncs && t.state == 2) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
    }
    rx.telemetry(t, 0);
    return false;
}

static int matched(const PskdTelemetry& t, const Bits& sync, const pskd::GenTruth& tr, int payloadBits) {
    std::set<Bits> truth(tr.payloads.begin(), tr.payloads.end());
    int ok = 0;
    for (size_t at : pskd::findSync(t.recent, sync, 1)) {
        if (at + (size_t)payloadBits > t.recent.size()) continue;
        if (truth.count(Bits(t.recent.begin() + (long)at, t.recent.begin() + (long)at + payloadBits))) ok++;
    }
    return ok;
}

int main() {
    // numbers
    CHECK(pskd::kFamily == 202 && pskd::kStandard == 204 && pskd::kActiveStandard == 203 && pskd::kPollSlot == 163, "mode numbers of docs/MODE-NUMBERS.md");
    CHECK(pskd::kPollSlot < Engine::kPollSeqSlots, "poll slot inside the table");
    const ModeTuning tu = pskdTuning();
    CHECK(tu.stdMode == pskd::kStandard && modeTuningById("pskdemod") == modeTuning(pskd::kStandard), "registry resolves pskdemod by id and standard");
    {
        auto engine = std::make_unique<Engine>();
        engine->setStandard(pskd::kStandard);
        RxTelemetry rx;
        CHECK(engine->latestRx(rx, 0) && rx.standard == pskd::kActiveStandard, "idle snapshot on the PSK demodulator active standard (got %d)", rx.standard);
    }

    const Bits sync = pskd::parseBitsOrHex("1ACFFC1D");
    // QPSK 1200 baud, the carrier 2 kHz from the dial, differential NRZ-S coded and G3RUH scrambled
    {
        pskd::GenSpec g;
        g.order = pskd::Order::Qpsk; g.fs = 48000; g.baud = 1200; g.offsetHz = 2000; g.esn0Db = 22; g.sync = sync; g.payloadBits = 240; g.frames = 60;
        g.diff = pskd::Diff::NrzS; g.scr = pskd::Scrambler::G3ruh;
        pskd::GenTruth tr;
        const std::vector<cf32> cap = pskd::generate(g, &tr);
        PskdReceiver rx;
        rx.configure(48000);
        CHECK(rx.ready(), "ready at 48 ksps");
        rx.setBits(pskd::Diff::NrzS, pskd::Scrambler::G3ruh, "1ACFFC1D", false, true, 0, false);
        PskdTelemetry t;
        const bool ok = follow(rx, cap, t, 6);
        CHECK(ok, "followed the signal (state %d, syncs %llu, status %s)", t.state, (unsigned long long)t.syncs, t.status.c_str());
        CHECK(t.haveAnalysis && t.rateFound && t.order == (int)pskd::Order::Qpsk && std::fabs(t.baud - 1200) < 2, "analysis: %s, %.2f baud", t.status.c_str(), t.baud);
        CHECK(std::fabs(t.carrierHz - 2000) < 100, "carrier %.1f Hz from the dial", t.carrierHz);
        CHECK(!t.constellation.empty() && !t.spectrumDb.empty() && !t.rows.empty() && t.bits > 1000, "displays: %zu constellation values, %zu spectrum bins, %zu rows, %llu bits",
              t.constellation.size(), t.spectrumDb.size(), t.rows.size(), (unsigned long long)t.bits);
        const int m = matched(t, sync, tr, g.payloadBits);
        CHECK(m >= 3, "%d payloads recovered behind the sync word", m);
        CHECK(!t.frames.empty(), "frames listed after the sync word");
        CHECK(pskdSummary(t).find("QPSK") != std::string::npos, "summary: %s", pskdSummary(t).c_str());
        // a retune forgets the signal and the settings stay
        rx.reset();
        rx.telemetry(t, 0);
        CHECK(!t.haveAnalysis && t.baud == 0, "reset forgets the analysis");
        rx.clearResults();
    }
    // the settings: a forced rate and modulation, the search width, a too low sample rate
    {
        pskd::GenSpec g;
        g.order = pskd::Order::Bpsk; g.fs = 48000; g.baud = 600; g.offsetHz = -3000; g.esn0Db = 20; g.sync = sync; g.payloadBits = 240; g.frames = 40;
        pskd::GenTruth tr;
        const std::vector<cf32> cap = pskd::generate(g, &tr);
        PskdReceiver rx;
        rx.configure(48000);
        rx.setOrder(pskd::Order::Bpsk);
        rx.setBaud(600);
        rx.setAlpha(0.35);
        rx.setBits(pskd::Diff::None, pskd::Scrambler::None, "1ACFFC1D", false, true, 0, false);
        PskdTelemetry t;
        const bool ok = follow(rx, cap, t, 6);
        CHECK(ok && t.order == (int)pskd::Order::Bpsk && t.baud == 600, "forced BPSK 600 baud followed (state %d, baud %.2f)", t.state, t.baud);
        CHECK(matched(t, sync, tr, g.payloadBits) >= 3, "BPSK payloads recovered");
        PskdReceiver low;
        low.configure(4000);
        CHECK(!low.ready(), "4 ksps is below the minimum");
    }
    return testSummary("test_pskdsp_rx");
}
