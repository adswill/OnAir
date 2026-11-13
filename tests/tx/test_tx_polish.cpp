// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Transmit fixes: the soft limiter and NaN guard before the DAC, the HackRF's int8 packing, retuning a running session, start / stop / read races,
// bounded memory over a long run, the carrier cross-fade, a missing audio file, the dry-run sink, and the radio range / availability texts.
// NOTHING is transmitted: every sink here is a counter.
#include "dect2/tx/tx.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <thread>
#include <vector>
using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

namespace {
struct Counter {
    std::atomic<uint64_t> samples{0}, closes{0}, retunes{0};
    std::atomic<float> maxMag{0};
    std::atomic<bool> nonFinite{false};
    TxTune last;
    std::mutex mu;
    std::vector<cf32> keep;   // the first samples, when asked for
    size_t keepN = 0;
};
struct CountSink : TxSink {
    std::shared_ptr<Counter> c = std::make_shared<Counter>();
    bool paced = true, canRetune = true;
    double rate = 0;
    bool isOpen = false;
    bool open(const TxTune& t, std::string&) override { rate = t.sampleRate; isOpen = true; std::lock_guard<std::mutex> l(c->mu); c->last = t; return true; }
    bool write(const cf32* x, size_t n, std::string&) override {
        float mx = c->maxMag;
        for (size_t i = 0; i < n; i++) {
            if (!std::isfinite(x[i].real()) || !std::isfinite(x[i].imag())) c->nonFinite = true;
            else mx = std::max(mx, std::abs(x[i]));
        }
        c->maxMag = mx;
        {
            std::lock_guard<std::mutex> l(c->mu);
            if (c->keep.size() < c->keepN) c->keep.insert(c->keep.end(), x, x + std::min(n, c->keepN - c->keep.size()));
        }
        c->samples += n;
        if (paced) std::this_thread::sleep_for(std::chrono::duration<double>((double)n / rate));
        return true;
    }
    bool retune(const TxTune& t, std::string& err) override {
        if (!canRetune) { err = "this radio cannot change its transmit settings while sending"; return false; }
        c->retunes++; std::lock_guard<std::mutex> l(c->mu); c->last = t; return true;
    }
    void close() override { if (isOpen) { isOpen = false; c->closes++; } }
    double sampleRate() const override { return rate; }
    bool realtime() const override { return paced; }
};
bool waitUntil(double secs, const std::function<bool()>& p) {
    const auto t0 = std::chrono::steady_clock::now();
    while (!p()) {
        if (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() > secs) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return true;
}
TxRequest fmReq() {
    TxRequest r;
    r.modeId = "fm";
    r.tune.centerHz = 100e6;
    return r;
}
}

static void limiterChecks() {
    std::vector<cf32> x = {{0.1f, -0.2f}, {0.6f, 0.6f}, {0.9f, 0.f}, {2.f, 0.f}, {3.f, 4.f}, {NAN, 0.f}, {0.f, INFINITY}, {-1.f, 0.f}, {0.f, 100.f}};
    const std::vector<cf32> orig = x;
    size_t lim = 0;
    const float pk = txSoftLimit(x.data(), x.size(), &lim);
    CHECK(x[0] == orig[0] && x[1] == orig[1], "samples below the knee are untouched (bit-identical)");
    CHECK(x[2] == orig[2], "exactly the knee passes");
    for (size_t i = 0; i < x.size(); i++) CHECK(std::isfinite(x[i].real()) && std::isfinite(x[i].imag()) && std::abs(x[i]) <= 1.0f, "sample %zu is finite and within full scale (%.4f)", i, std::abs(x[i]));
    CHECK(x[5] == cf32(0, 0) && x[6] == cf32(0, 0), "NaN and Inf become 0");
    CHECK(std::fabs(std::arg(x[4]) - std::arg(orig[4])) < 1e-5f, "the phase is kept");
    CHECK(std::abs(x[3]) > 0.9f && std::abs(x[3]) < std::abs(x[4]) + 1e-6f, "monotone: 2.0 -> %.4f, 5.0 -> %.4f", std::abs(x[3]), std::abs(x[4]));
    CHECK(lim == 4, "limited count %zu", lim);
    CHECK(std::fabs(pk - 100.f) < 1e-3f, "the largest input is reported: %.3f", pk);
    float prev = 0;   // the transfer curve never goes down
    for (float m = 0; m < 10; m += 0.01f) { cf32 v(m, 0); txSoftLimit(&v, 1); CHECK(v.real() + 1e-7f >= prev && v.real() <= 1.f, "monotone at %.2f", m); prev = v.real(); }
    const TxLevel l = txMeasure(orig.data(), 2);
    CHECK(std::fabs(l.peak - std::sqrt(0.72f)) < 1e-5f, "measured peak %.4f", l.peak);
    printf("  limiter ok\n");
}

static void packChecks() {
    cf32 in[5] = {{1.f, -1.f}, {2.f, -2.f}, {NAN, INFINITY}, {0.5f, -0.5f}, {0.f, 0.f}};
    int8_t out[10];
    hackrfTxPack(in, 5, out);
    CHECK(out[0] == 127 && out[1] == -127 && out[2] == 127 && out[3] == -127, "full scale and clipping: %d %d %d %d", out[0], out[1], out[2], out[3]);
    CHECK(out[4] == 0, "NaN packs to 0, got %d", out[4]);
    CHECK(out[6] == 64 && out[7] == -64 && out[8] == 0 && out[9] == 0, "half scale: %d %d", out[6], out[7]);
    printf("  int8 packing ok\n");
}

static void levelChecks() {
    {   // the carrier (amplitude 1.0) and an overdriven payload never reach the sink above full scale
        auto sink = std::make_unique<CountSink>();
        sink->paced = false;
        auto c = sink->c;
        TxSession s;
        TxRequest r = fmReq();
        r.carrierTune = true;
        std::string err;
        CHECK(s.start(std::move(sink), r, {}, err), "start: %s", err.c_str());
        CHECK(waitUntil(10, [&] { return c->samples > 500000; }), "samples flow");
        const TxSession::Stats st = s.stats();
        s.stop();
        CHECK(c->maxMag <= 1.0f && !c->nonFinite, "peak at the sink %.4f", (double)c->maxMag);
        CHECK(st.peak > 0.9f && st.peak <= 1.0f && st.rms > 0.9f, "the meters see the carrier: peak %.3f rms %.3f", st.peak, st.rms);
        CHECK(st.limitedPct > 90, "the carrier is in the limiter's knee: %.1f%%", st.limitedPct);
    }
    {   // a normal payload is not touched by the limiter
        auto sink = std::make_unique<CountSink>();
        sink->paced = false;
        auto c = sink->c;
        TxSession s;
        std::string err;
        CHECK(s.start(std::move(sink), fmReq(), {}, err), "start: %s", err.c_str());
        CHECK(waitUntil(10, [&] { return c->samples > 500000; }), "samples flow");
        const TxSession::Stats st = s.stats();
        s.stop();
        CHECK(st.limitedPct == 0 && st.peak < 0.9f, "fm payload: limited %.2f%% peak %.3f", st.limitedPct, st.peak);
    }
    printf("  levels ok\n");
}

static void retuneChecks() {
    TxSession s;
    std::string err;
    CHECK(!s.retune(TxTune(), err) && !err.empty(), "retune with nothing running is refused: %s", err.c_str());
    auto sink = std::make_unique<CountSink>();
    auto c = sink->c;
    CHECK(s.start(std::move(sink), fmReq(), {}, err), "start: %s", err.c_str());
    CHECK(waitUntil(10, [&] { return c->samples > 20000; }), "sending");
    TxTune t;
    t.centerHz = 433.92e6; t.gainDb = 12.5;
    CHECK(s.retune(t, err), "retune: %s", err.c_str());
    {
        std::lock_guard<std::mutex> l(c->mu);
        CHECK(c->last.centerHz == 433.92e6 && c->last.gainDb == 12.5 && c->retunes == 1, "the radio got the new frequency and gain");
    }
    CHECK(s.running(), "still sending after the retune");
    s.stop();
    CHECK(c->closes == 1, "closed once");
    // a radio that cannot retune says so and keeps going
    auto sink2 = std::make_unique<CountSink>();
    sink2->canRetune = false;
    CHECK(s.start(std::move(sink2), fmReq(), {}, err), "start 2");
    CHECK(waitUntil(10, [&] { return s.stats().samples > 20000; }), "sending 2");
    CHECK(!s.retune(t, err) && err.find("cannot change") != std::string::npos && s.running(), "no retune: '%s'", err.c_str());
    s.stop();
    printf("  retune ok\n");
}

static void raceChecks() {
    // one thread starts and stops, others read the stats, the log and the tap, and retune: nothing may crash or hang
    TxSession s;
    std::atomic<bool> done{false};
    std::vector<std::thread> readers;
    for (int k = 0; k < 3; k++)
        readers.emplace_back([&, k] {
            std::vector<cf32> iq; double rate = 0;
            TxTune t; t.centerHz = 90e6;
            while (!done) {
                (void)s.stats(); (void)s.log(); s.copyTap(iq, rate); (void)s.running();
                if (k == 0) { std::string e; s.retune(t, e); }
                if (k == 1) s.setCarrierTune(true), s.setCarrierTune(false);
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        });
    for (int i = 0; i < 12; i++) {
        auto sink = std::make_unique<CountSink>();
        sink->paced = i % 2 == 0;
        std::string err;
        TxRequest r = fmReq();
        r.loop = i % 3 != 0;
        r.onceSeconds = 0.2;
        if (s.start(std::move(sink), r, {}, err)) std::this_thread::sleep_for(std::chrono::milliseconds(20 + 15 * (i % 4)));
        s.stop();
    }
    done = true;
    for (auto& t : readers) t.join();
    CHECK(!s.running(), "stopped");
    printf("  start/stop/read races ok\n");
}

static void memoryChecks() {
    auto sink = std::make_unique<CountSink>();
    sink->paced = false;   // as fast as the generator goes: the queue must stay bounded
    auto c = sink->c;
    TxSession s;
    std::string err;
    CHECK(s.start(std::move(sink), fmReq(), {}, err), "start: %s", err.c_str());
    size_t maxQ = 0, maxTap = 0;
    const auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < 4) {
        const auto st = s.stats();
        maxQ = std::max(maxQ, st.queued);
        std::vector<cf32> iq; double rate;
        s.copyTap(iq, rate);
        maxTap = std::max(maxTap, iq.size());
        CHECK(st.queued <= st.queueMax, "queue %zu over its limit %zu", st.queued, st.queueMax);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    const double made = (double)c->samples;
    s.stop();
    CHECK(maxTap <= 1000000, "the tap stays at or below 1e6 samples (%zu)", maxTap);
    CHECK(s.log().size() <= 500, "the log is bounded");
    printf("  memory: %.1f Msamples sent, queue peak %zu blocks, tap %zu samples\n", made / 1e6, maxQ, maxTap);
}

static void carrierFadeChecks() {
    auto sink = std::make_unique<CountSink>();
    sink->paced = false;
    auto c = sink->c;
    c->keepN = 4000000;
    TxSession s;
    TxRequest r = fmReq();
    r.loop = true;
    std::string err;
    CHECK(s.start(std::move(sink), r, {}, err), "start: %s", err.c_str());
    CHECK(waitUntil(10, [&] { return c->samples > 300000; }), "sending");
    s.setCarrierTune(true);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    s.setCarrierTune(false);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    s.stop();
    std::vector<cf32> k;
    { std::lock_guard<std::mutex> l(c->mu); k = c->keep; }
    float maxStep = 0;
    for (size_t i = 1; i < k.size(); i++) maxStep = std::max(maxStep, std::abs(k[i] - k[i - 1]));
    CHECK(k.size() > 1000000, "kept %zu samples", k.size());
    CHECK(maxStep < 0.1f, "no step when the carrier switches: largest sample-to-sample change %.4f", maxStep);
    printf("  carrier cross-fade ok (largest step %.4f)\n", maxStep);
}

static void audioFileChecks() {
    auto sink = std::make_unique<CountSink>();
    sink->paced = false;
    auto c = sink->c;
    TxSession s;
    TxRequest r = fmReq();
    r.audioSrc = 1;
    r.file = "/nonexistent/none.wav";
    std::string err;
    CHECK(s.start(std::move(sink), r, {}, err), "a missing audio file does not stop the start: %s", err.c_str());
    bool said = false;
    for (const auto& l : s.log()) if (l.find("audio file not used") != std::string::npos) said = true;
    CHECK(said, "the log says the file was not used");
    CHECK(waitUntil(10, [&] { return c->samples > 100000; }), "a tone is sent instead");
    s.stop();
    printf("  audio file fallback ok\n");
}

static void dryRunChecks() {
    auto sink = makeNullTxSink();
    TxSession s;
    TxRequest r = fmReq();
    std::string err;
    CHECK(s.start(std::move(sink), r, {}, err), "dry run start: %s", err.c_str());
    CHECK(s.dryRun(), "the session knows it is a test");
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    const auto st = s.stats();
    CHECK(st.seconds > 0.8 && st.seconds < 2.2, "the dry run is paced like a radio: %.2f s of signal in 1.5 s", st.seconds);
    std::vector<cf32> iq; double rate = 0;
    s.copyTap(iq, rate);
    CHECK(iq.size() > 2048 && rate > 0, "the tap has IQ for the spectrum");
    CHECK(st.underruns == 0, "the start is pre-rolled: no gap of silence in the first seconds (%llu)", (unsigned long long)st.underruns);
    const auto t0 = std::chrono::steady_clock::now();
    s.stop();
    CHECK(std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < 0.5, "stop is quick");
    // a very low rate mode: stop must still be quick (the sender writes in pieces)
    printf("  dry run ok\n");
}

// capability classification and message text (no hardware)
static void usrpClassChecks() {
    auto usrp = [](const char* label, const char* nativeArgs, bool tx) {
        DeviceInfo d; d.kind = DeviceInfo::Native; d.board = "usrp"; d.serial = "000000001"; d.nativeArgs = nativeArgs;
        d.name = std::string("USRP B210 000000001 ") + label + " (native, experimental)";
        d.txName = "USRP B210 000000001 (native, experimental)"; d.canTransmit = tx;
        return d;
    };
    DeviceInfo rtl; rtl.kind = DeviceInfo::Native; rtl.board = "rtlsdr"; rtl.name = "RTL2838UHIDIR 10115420 (native, experimental)";
    std::vector<DeviceInfo> devs = {rtl, usrp("RF A RX2", "type=b200", true), usrp("RF A TX/RX", "type=b200#0#TX/RX", true), usrp("RF B RX2", "type=b200#1#RX2", true)};
    const std::vector<int> ch = txRadioChoices(devs);
    CHECK(ch.size() == 1 && ch[0] == 1, "a USRP with several RX inputs is offered once for TX");
    CHECK(txRadioLabel(devs[1]) == "USRP B210 000000001", "TX label: %s", txRadioLabel(devs[1]).c_str());
    const std::vector<int> ro = txReceiveOnlyRadios(devs);
    CHECK(ro.size() == 1 && ro[0] == 0, "only the RTL-SDR is hidden as receive-only");
    const std::string r = txUnavailableReason(rtl);
    CHECK(r == "RTL-SDR dongles can only receive", "rtl text: %s", r.c_str());
    for (auto& d : devs) d.canTransmit = false;   // a UHD build without TX functions
    const std::string u = txUnavailableReason(devs[1]);
    CHECK(u.find("USRP B210 000000001 cannot transmit in this build: ") == 0 && u.find("UHD") != std::string::npos, "usrp text: %s", u.c_str());
    size_t first = u.find("USRP B210"), second = u.find("USRP B210", first + 1);
    CHECK(second == std::string::npos, "the name is printed once: %s", u.c_str());
    CHECK(txUnavailableReason(devs[1]).find("(native") == std::string::npos, "no '(native, experimental)' in the sentence");
    DeviceInfo ah; ah.kind = DeviceInfo::Native; ah.board = "airspyhf"; ah.name = "Airspy HF+ Discovery (native, experimental)";
    CHECK(txUnavailableReason(ah).find("receive-only") != std::string::npos && txUnavailableReason(ah).find("Airspy HF+ Discovery") == 0, "airspyhf: %s", txUnavailableReason(ah).c_str());
    // the sink: a build/UHD without the library gives no sink
    DeviceInfo tx = usrp("RF A RX2", "type=b200", true);
    std::unique_ptr<TxSink> sink = makeTxSink(tx);
    CHECK(sink != nullptr, "a USRP that canTransmit has a sink");
    if (sink) { std::string e; TxTune t; t.centerHz = 433e6; t.sampleRate = 1e6; CHECK(!sink->open(t, e) && !e.empty(), "no UHD here: open says why: %s", e.c_str()); }
}

static void infoChecks() {
    DeviceInfo d;
    d.name = "TestSDR"; d.kind = DeviceInfo::Soapy; d.minFreqHz = 70e6; d.maxFreqHz = 6e9;
    CHECK(txRangeInfo(d, 433e6).empty(), "inside the range: no note");
    const std::string o = txRangeInfo(d, 20e6);
    CHECK(o.find("70.000 to 6000.000 MHz") != std::string::npos && o.find("sent anyway") != std::string::npos, "outside: info, not a block: %s", o.c_str());
    d.minFreqHz = d.maxFreqHz = 0;
    CHECK(txRangeInfo(d, 1e12).empty(), "unknown range: no note");
    d.canTransmit = true;
    CHECK(txUnavailableReason(d).empty(), "can transmit");
    d.canTransmit = false;
    CHECK(txUnavailableReason(d).find("receive-only") != std::string::npos, "rx only: %s", txUnavailableReason(d).c_str());
    d.kind = DeviceInfo::Audio;
    CHECK(txUnavailableReason(d).find("sound card") != std::string::npos, "sound card");
    // occupied bandwidth of two tones at +-100 kHz at 1 Msps
    const int n = 2048;
    std::vector<cf32> x(n);
    for (int i = 0; i < n; i++) x[(size_t)i] = std::polar(0.5f, 2.f * 3.14159265f * 0.1f * (float)i) + std::polar(0.5f, -2.f * 3.14159265f * 0.1f * (float)i);
    std::vector<float> db;
    txSpectrumDb(x.data(), n, n, db);
    const double bw = txOccupiedBandwidthHz(db, 1e6);
    CHECK(bw > 150e3 && bw < 300e3, "two tones at +-100 kHz occupy about 200 kHz: %.0f Hz", bw);
    printf("  info texts ok (occupied bandwidth %.0f Hz)\n", bw);
}

// the HF beacon and Mode S uplink generators put no noise floor into what a transmitter sends
static void cleanSignalChecks() {
    for (const char* id : {"hfbeacon", "uplink"}) {
        const TxModeSpec* m = txModeById(id);
        CHECK(m, "mode %s", id);
        if (!m) continue;
        TxRequest r;
        r.modeId = id;
        std::string err;
        const double rate = txRequestRate(*m, r, TxRateLimits{});
        auto g = makeTxGenerator(*m, r, rate, err);
        CHECK(g, "%s generator: %s", id, err.c_str());
        if (!g) continue;
        std::vector<cf32> x(1 << 16);
        g->generate(x.data(), x.size());
        std::vector<float> db;
        txSpectrumDb(x.data() + x.size() - 4096, 4096, 4096, db);
        std::vector<float> sorted = db;
        std::sort(sorted.begin(), sorted.end());
        const float median = sorted[sorted.size() / 2], top = sorted.back();
        CHECK(top - median > 50 || top < -150, "%s: a clean signal has no noise floor (peak %.1f dB, median bin %.1f dB)", id, top, median);
    }
    printf("  clean generators ok\n");
}

int main() {
    printf("tx polish:\n");
    cleanSignalChecks();
    limiterChecks();
    packChecks();
    levelChecks();
    retuneChecks();
    raceChecks();
    memoryChecks();
    carrierFadeChecks();
    audioFileChecks();
    dryRunChecks();
    infoChecks();
    usrpClassChecks();
    printf(fails ? "tx polish tests FAILED (%d)\n" : "tx polish tests passed\n", fails);
    return fails ? 1 : 0;
}
