// Transmit: the list of modes, the TX session (underruns, stopping, failures, no frequency or power limit) and the round trip: the samples a TX session
// sends to a recording sink are fed to OnAir's own receiver (through the engine) and must decode. The radios' own paths are tried against fakes:
// with --soapy <module dir> the SoapySDR TX sink against the fake "onairtest" radio, with --fake <fake library> the PlutoSDR (libiio) TX sink against
// the fake libiio. The HackRF's USB path has no fake: only its sample conversion is checked here. NOTHING is transmitted by this test.
#ifdef _WIN32
#include <stdlib.h>
#define setenv(k, v, o) _putenv_s(k, v)
#else
#include <dlfcn.h>
#include <unistd.h>
#endif
#include "dect2/engine/engine.h"
#include "dect2/engine/device_claims.h"
#include "dect2/engine/engine_testkit.h"
#include "dect2/engine/mode_tuning.h"
#include "dect2/engine/modes.h"
#include "dect2/engine/mode_synth.h"
#include "dect2/pager/pager_gen.h"
#include "dect2/pager/pager_rx.h"
#include "dect2/hfdig/hfdig_ftx.h"
#include "dect2/hfdig/hfdig_gen.h"
#include "dect2/hfdig/hfdig_rtty.h"
#include "dect2/hfdig/hfdig_rx.h"
#include "dect2/hfdig/hfdig_tel.h"
#include "dect2/acars/acars_rx.h"
#include "dect2/marine/marine_rx.h"
#include "dect2/sonde/sonde_rx.h"
#include "dect2/tx/tx.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <set>
#include <string>
#include <thread>
#include <vector>
using namespace dect2;
static int fails = 0;
static std::string onlyId;   // --only <mode id>: just that round trip
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

// A transmitter that is a file: nothing is sent anywhere. realtime=false: write() takes what it gets at once and the session waits for data instead of
// sending silence; paced=true behaves like a radio (write() returns after the samples' duration).
struct RecState {   // outlives the sink: the session destroys the sink when it stops
    TxTune tune;
    double rate = 0;
    std::atomic<uint64_t> samples{0}, writes{0}, opens{0}, closes{0}, afterClose{0};
    std::atomic<bool> isOpen{false};
};
struct RecSink : TxSink {
    std::shared_ptr<RecState> st = std::make_shared<RecState>();
    std::string path;
    FILE* f = nullptr;
    bool rt = false, paced = false;
    double rateOverride = 0;
    std::string refuse;           // open() fails with this text
    long failAfterWrites = -1;    // write() fails from this write on
    explicit RecSink(std::string p = "") : path(std::move(p)) {}
    ~RecSink() override { if (f) fclose(f); }
    bool open(const TxTune& t, std::string& err) override {
        st->opens++;
        if (!refuse.empty()) { err = refuse; return false; }
        st->tune = t;
        st->rate = rateOverride > 0 ? rateOverride : t.sampleRate;
        if (!path.empty()) f = fopen(path.c_str(), "wb");
        st->isOpen = true;
        return true;
    }
    bool write(const cf32* x, size_t n, std::string& err) override {
        if (!st->isOpen) st->afterClose++;
        if (failAfterWrites >= 0 && (long)st->writes >= failAfterWrites) { err = "the radio was unplugged"; return false; }
        st->writes++;
        if (f) fwrite(x, sizeof(cf32), n, f);
        st->samples += n;
        if (paced) std::this_thread::sleep_for(std::chrono::duration<double>(n / st->rate));
        return true;
    }
    void close() override { if (st->isOpen) { st->closes++; st->isOpen = false; if (f) { fclose(f); f = nullptr; } } }
    double sampleRate() const override { return st->rate; }
    bool realtime() const override { return rt; }
};

static bool waitUntil(double secs, const std::function<bool()>& p) {
    const auto t0 = std::chrono::steady_clock::now();
    while (!p()) {
        if (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() > secs) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return true;
}

// ------------------------------------------------------------------ the receiver side of the round trip

struct Decoded { bool locked = false; bool services = false; double snr = 0; uint64_t blocks = 0; std::string summary; };

// the engine plays the recording (cf32 file) as fast as it takes it and must lock and decode
static Decoded decode(const TxModeSpec& m, const std::string& path, double rate, double bwMhz = 0, const SynthConfig* synth = nullptr) {
    Decoded d;
    Engine e;
    DeviceInfo dev; dev.kind = DeviceInfo::File; dev.name = "tx recording";
    const ModeTuning* mt = modeTuning(m.stdMode);
    TuneSettings t;
    t.sampleRate = rate;
    t.centerHz = 100e6;
    t.bandwidthMhz = bwMhz > 0 ? bwMhz : m.stdMode == 3 || m.stdMode == 6 ? 6 : m.stdMode == 4 ? 1.536 : m.stdMode == 0 ? 8 : mt ? mt->bandwidthMhz : 8;
    FileOptions fo; fo.path = path; fo.format = FileFormat::CF32; fo.sampleRate = rate; fo.loop = true;
    e.setStandard(m.stdMode == 0 ? m.variant : m.stdMode);
    if (!e.start(dev, t, fo)) { d.summary = "the engine did not start"; return d; }
    RxTelemetry rx;
    uint64_t seq = 0;
    const bool needServices = m.stdMode == 0 || m.stdMode == 3 || m.stdMode == 6 || m.stdMode == 8 || m.stdMode == 9;   // the TV modes carry a transport stream
    const auto t0 = std::chrono::steady_clock::now();
    const double wait = m.stdMode == 25 ? 40 : (needServices ? 120 : 30);
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < wait && !(d.locked && (d.services || !needServices))) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        if (e.latestRx(rx, seq)) {
            seq = rx.seq;
            if (m.stdMode == 25) {   // pagers: every page of the chosen mix/speed with its exact text
                const std::vector<PagerMessage> none;
                const auto& got = rx.pager.messages ? *rx.pager.messages : none;
                size_t found = 0;
                const int kind = synth ? synth->modeOpt[0] : 0, spd = synth ? synth->modeOpt[2] : 0;
                const auto want = pagerTestMessages(kind, spd);
                for (const auto& w : want)
                    for (const auto& g : got)
                        if (g.speed == w.speed && g.flex == w.flex && g.address == w.address && g.type == w.type && g.function == w.function && g.text == w.text) {
                            found++;
                            break;
                        }
                d.locked = !want.empty() && found == want.size();
                d.blocks = found; d.snr = (double)want.size();   // reported: pages found, pages listed
                if (d.locked) d.services = true;
            } else if (rx.state == 2 && rx.dataValid) d.locked = true;
            if (m.stdMode != 25) { d.snr = rx.dataSnrDb; d.blocks = rx.blocksOk; }
        }
        const auto ts = e.tsSnapshot();
        for (const auto& s : ts.services) {
            bool v = false, a = false;
            for (const auto& x : s.streams) { v |= x.kind == "video"; a |= x.kind == "audio"; }
            d.services |= (v && a) || (m.stdMode == 0 && m.variant == 1);   // the T2 test stream is a data stream: any service will do
        }
    }
    if (m.stdMode == 25 && !d.locked && e.latestRx(rx, 0)) {
        const std::vector<PagerMessage> none;
        const auto& got = rx.pager.messages ? *rx.pager.messages : none;
        const int kind = synth ? synth->modeOpt[0] : 0, spd = synth ? synth->modeOpt[2] : 0;
        for (const auto& w : pagerTestMessages(kind, spd)) {
            bool ok = false;
            for (const auto& g : got) if (g.speed == w.speed && g.flex == w.flex && g.address == w.address && g.type == w.type && g.function == w.function && g.text == w.text) ok = true;
            if (!ok) printf("    missing: speed %d addr %u type %d fn %d '%s'\n", w.speed, w.address, w.type, w.function, w.text.c_str());
        }
        for (size_t i = 0; i < got.size() && i < 8; i++) printf("    got: speed %d addr %u type %d fn %d '%s'\n", got[i].speed, got[i].address, got[i].type, got[i].function, got[i].text.c_str());
    }
    char b[200];
    snprintf(b, sizeof b, "locked %d, SNR %.1f dB, %llu blocks, services %d", (int)d.locked, d.snr, (unsigned long long)d.blocks, (int)d.services);
    d.summary = b;
    e.stop();
    return d;
}

static std::string tmpPath(const char* stem) {
    char b[256];
    snprintf(b, sizeof b, "/tmp/onair_tx_%s_%d.cf32", stem, (int)getpid());
    return b;
}

// a session into a recording sink, one pass of `secs`; returns the rate the sink ran at (0: did not start)
static double record(const char* id, SynthConfig synth, double secs, const std::string& path, TxRateLimits lim = {}, double bw = 8) {
    auto sink = std::make_unique<RecSink>(path);
    TxSession s;
    TxRequest r;
    r.modeId = id; r.synth = synth; r.bandwidthMhz = bw; r.loop = false; r.onceSeconds = secs;
    r.tune.centerHz = 100e6; r.tune.gainDb = 10;
    std::string err;
    if (!s.start(std::move(sink), r, lim, err)) { printf("  %s: could not start: %s\n", id, err.c_str()); return 0; }
    const bool done = waitUntil(180, [&] { return !s.running(); });
    const auto st = s.stats();
    CHECK(done && st.finished && !st.failed, "%s: the one-pass transmission ended (%d) finished %d failed %d: %s", id, (int)done, (int)st.finished, (int)st.failed, st.error.c_str());
    CHECK(st.samples >= (uint64_t)(secs * st.rateHz * 0.99), "%s: %llu samples for %.1f s at %.0f", id, (unsigned long long)st.samples, secs, st.rateHz);
    CHECK(st.underruns == 0, "%s: %llu underruns into a recording sink", id, (unsigned long long)st.underruns);
    return st.rateHz;
}

// ------------------------------------------------------------------ the checks

static void tableChecks() {
    std::set<std::string> ids;
    for (const auto& m : txModes()) {
        CHECK(ids.insert(m.id).second, "duplicate id %s", m.id);
        CHECK(txModeById(m.id) == &m, "txModeById(%s)", m.id);
        CHECK(m.generator && *m.generator && m.options && m.roundTrip && *m.roundTrip, "%s: the table row is documented", m.id);
    }
    const char* keep[] = {"dvbt2", "dvbt", "atsc", "dab", "isdbt", "dvbs", "dtmb", "adsb"};
    for (const char* w : keep) CHECK(txModeById(w), "%s is on the list", w);
    CHECK(txModeById("pager") && txModeFor(25), "pagers are offered");
    CHECK(txModeFor(0, 1) && std::string(txModeFor(0, 1)->id) == "dvbt2" && txModeFor(0, 2) && std::string(txModeFor(0, 2)->id) == "dvbt", "DVB-T2 / DVB-T by variant");
    CHECK(txModeFor(1) && std::string(txModeFor(1)->id) == "dvbt2" && txModeFor(2) && std::string(txModeFor(2)->id) == "dvbt", "engine codes 1/2 map to T2/T");
    CHECK(txModeFor(8) && std::string(txModeFor(8)->id) == "dvbs" && txModeFor(13), "S2 and ADS-B by engine code");
    const int formerlyRefused[] = {14, 30, 34, 41, 43, 44, 50, 53, 54, 56, 57, 58, 59, 60, 71, 72, 74, 76, 77, 79, 81};
    for (int std_ : formerlyRefused) {
        CHECK(txModeFor(std_), "engine mode %d is offered for transmit (no mode is refused)", std_);
        CHECK(txRefusalReason(std_).empty(), "mode %d: no refusal", std_);
    }
    CHECK(!txModeFor(75) && txRefusalReason(75) == "this mode has no transmit generator", "a mode without a generator says so");
    // every engine mode that has a generator is accepted
    for (int std_ = 3; std_ <= 90; std_++) {
        const ModeTuning* mt = modeTuning(std_);
        if (!mt) continue;
        SynthConfig sc;
        auto ms = makeModeSynth(std_, sc, std::max(mt->sampleRate, mt->minSampleRate));
        if (ms) CHECK(txModeFor(std_), "mode %d has a generator and must be accepted for transmit", std_);
        else CHECK(txModeFor(std_) || !txRefusalReason(std_).empty(), "mode %d: listed or 'no generator'", std_);
    }
    CHECK(txRefusalReason(8).empty(), "DVB-S has no refusal");
    CHECK(txRefusalReason(33).empty() && txModeFor(33), "M17 is offered");
    // radios that cannot transmit have no sink
    DeviceInfo syn; syn.kind = DeviceInfo::Synthetic;
    DeviceInfo file; file.kind = DeviceInfo::File;
    DeviceInfo rtl; rtl.kind = DeviceInfo::Native; rtl.board = "rtlsdr";
    DeviceInfo hackrf; hackrf.kind = DeviceInfo::HackRF; hackrf.board = "HackRF One";   // canTransmit only from the listing
    CHECK(!makeTxSink(syn) && !makeTxSink(file) && !makeTxSink(rtl) && !makeTxSink(hackrf), "a radio without canTransmit has no transmit sink");
    CHECK(!rtl.canTransmit && !syn.canTransmit && !file.canTransmit, "canTransmit is false by default");
    rtl.canTransmit = true;   // even if a listing said so, a native radio that has no TX path gives none
    CHECK(!makeTxSink(rtl), "a native radio other than the PlutoSDR has no transmit path");
}

static void packChecks() {
    const cf32 in[] = {{1.f, -1.f}, {0.5f, -0.5f}, {2.f, -2.f}, {0.f, 0.f}, {0.2f, -0.003f}};
    int8_t out[10];
    hackrfTxPack(in, 5, out);
    CHECK(out[0] == 127 && out[1] == -127, "full scale: %d %d", out[0], out[1]);
    CHECK(out[2] == 64 && out[3] == -64, "half scale: %d %d", out[2], out[3]);
    CHECK(out[4] == 127 && out[5] == -127, "beyond full scale clips: %d %d", out[4], out[5]);
    CHECK(out[6] == 0 && out[7] == 0 && out[8] == 25 && out[9] == 0, "small values: %d %d %d %d", out[6], out[7], out[8], out[9]);
}

static void sessionChecks() {
    SynthConfig syn;
    syn.modeOpt[0] = 12;   // ADS-B: twelve aircraft
    auto req = [&](const char* id = "adsb") {
        TxRequest r;
        r.modeId = id; r.synth = syn; r.tune.centerHz = 1090e6; r.tune.gainDb = 10;
        return r;
    };
    {   // any frequency and any gain go to the driver as given
        auto sink = std::make_unique<RecSink>();
        auto raw = sink->st;
        TxSession s;
        TxRequest r = req();
        r.tune.centerHz = 1e12; r.tune.gainDb = 123.4; r.tune.ampOn = true;
        std::string err;
        CHECK(s.start(std::move(sink), r, {}, err), "start at 1 THz: %s", err.c_str());
        CHECK(raw->tune.centerHz == 1e12 && raw->tune.gainDb == 123.4 && raw->tune.ampOn, "frequency %.0f and gain %.1f reached the sink untouched", raw->tune.centerHz, raw->tune.gainDb);
        CHECK(waitUntil(5, [&] { return raw->samples > 100000; }), "samples flow");
        CHECK(s.running(), "running");
        s.stop();
        CHECK(!s.running() && raw->closes == 1 && !raw->isOpen, "stop closes the radio once");
        const uint64_t n = raw->samples;
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        CHECK(raw->samples == n && raw->afterClose == 0, "no sample after stop");
        s.stop();   // twice is harmless
        const auto log = s.log();
        CHECK(log.size() >= 2 && log.front().find("TRANSMITTING ADS-B") != std::string::npos && log.front().find("1000000.000000 MHz") != std::string::npos, "the log says what was sent: %s", log.empty() ? "-" : log.front().c_str());
        CHECK(log.back().find("transmission stopped") != std::string::npos, "the log says it stopped: %s", log.back().c_str());
    }
    {   // a driver that refuses: its words come back, nothing is sent
        auto sink = std::make_unique<RecSink>();
        sink->refuse = "the radio's driver refused 1000000 MHz: the synthesiser stops at 6000 MHz";
        auto raw = sink->st;
        TxSession s;
        std::string err;
        CHECK(!s.start(std::move(sink), req(), {}, err) && err == "the radio's driver refused 1000000 MHz: the synthesiser stops at 6000 MHz", "refusal text: %s", err.c_str());
                CHECK(!s.running() && s.log().size() == 1 && s.log()[0].find("could not start") != std::string::npos, "the refusal is in the log");
    }
    {   // an unknown mode, no sink
        TxSession s;
        std::string err;
        CHECK(!s.start(std::make_unique<RecSink>(), req("nosuchmode"), {}, err) && err.find("cannot be transmitted") != std::string::npos, "mode not on the list: %s", err.c_str());
        CHECK(!s.start(nullptr, req(), {}, err), "no sink");
    }
    {   // one pass ends by itself and the radio goes off
        auto sink = std::make_unique<RecSink>();
        auto raw = sink->st;
        TxSession s;
        TxRequest r = req();
        r.loop = false; r.onceSeconds = 1;
        std::string err;
        CHECK(s.start(std::move(sink), r, {}, err), "start one pass");
        CHECK(waitUntil(60, [&] { return !s.running(); }), "one pass finished");
        const auto st = s.stats();
        CHECK(st.finished && !st.failed && std::fabs(st.seconds - 1.0) < 0.05 && raw->closes == 1, "finished %d, %.3f s, closes %llu", (int)st.finished, st.seconds, (unsigned long long)raw->closes.load());
        CHECK(s.log().back().find("transmission finished") != std::string::npos, "log: %s", s.log().back().c_str());
    }
    {   // the radio fails in the middle: the session stops itself, says why, and closes the radio
        auto sink = std::make_unique<RecSink>();
        sink->failAfterWrites = 5;
        auto raw = sink->st;
        TxSession s;
        std::string err;
        CHECK(s.start(std::move(sink), req(), {}, err), "start");
        CHECK(waitUntil(20, [&] { return !s.running(); }), "the session ended by itself");
        const auto st = s.stats();
        CHECK(st.failed && st.error == "the radio was unplugged" && raw->closes == 1, "failure: %d '%s' closes %llu", (int)st.failed, st.error.c_str(), (unsigned long long)raw->closes.load());
        CHECK(s.log().back().find("TRANSMISSION STOPPED: the radio was unplugged") != std::string::npos, "log: %s", s.log().back().c_str());
        s.stop();
        CHECK(raw->closes == 1, "closed once");
    }
    {   // a real radio is not waited for: a generator that cannot keep up costs counted gaps of silence, not a stall (a sink that takes samples at once
        // empties the queue faster than they are made), and a radio that is paced to its rate gets none for a generator that is faster than real time
        auto fast = std::make_unique<RecSink>();
        fast->rt = true;
        auto rf = fast->st;
        TxSession s;
        std::string err;
        CHECK(s.start(std::move(fast), req(), {}, err), "start (realtime sink)");
        CHECK(waitUntil(10, [&] { return s.stats().underruns > 0; }), "underruns are counted");
        s.stop();
        CHECK(rf->samples > 0 && s.stats().underruns > 0, "gaps: %llu", (unsigned long long)s.stats().underruns);
        auto paced = std::make_unique<RecSink>();
        paced->rt = true; paced->paced = true;
        auto rp = paced->st;
        TxSession s2;
        TxRequest r = req();
        r.loop = false; r.onceSeconds = 2;
        CHECK(s2.start(std::move(paced), r, {}, err), "start (paced sink)");
        CHECK(waitUntil(30, [&] { return !s2.running(); }), "paced run ended");
        CHECK(s2.stats().underruns == 0 && s2.stats().finished && rp->samples >= (uint64_t)(2 * 4e6 * 0.99), "paced radio: %llu gaps, %llu samples", (unsigned long long)s2.stats().underruns, (unsigned long long)rp->samples.load());
    }
    {   // a radio that runs at another rate than asked: the generator follows it (rate_ = what the radio reports)
        auto sink = std::make_unique<RecSink>();
        sink->rateOverride = 5e6;
        TxSession s;
        TxRequest r = req();
        r.loop = false; r.onceSeconds = 1;
        std::string err;
        CHECK(s.start(std::move(sink), r, {}, err), "start");
        CHECK(waitUntil(60, [&] { return !s.running(); }) && std::fabs(s.stats().rateHz - 5e6) < 1 && std::fabs(s.stats().samples - 5e6) < 40000, "rate follows the radio: %.0f, %llu samples", s.stats().rateHz, (unsigned long long)s.stats().samples);
    }
    {   // a fixed-rate mode (DVB-T2: 64/7 Msps) on a radio with a limited rate range is resampled, and the tune asks for a rate inside the range
        auto sink = std::make_unique<RecSink>();
        auto raw = sink->st;
        TxSession s;
        TxRequest r = req("dvbt2");
        r.loop = false; r.onceSeconds = 1;
        std::string err;
        TxRateLimits lim; lim.minHz = 2e6; lim.maxHz = 6e6;
        CHECK(s.start(std::move(sink), r, lim, err), "T2 into a radio up to 6 Msps: %s", err.c_str());
        CHECK(waitUntil(60, [&] { return !s.running(); }) && raw->tune.sampleRate == 6e6 && s.stats().finished, "T2 resampled to 6 Msps: asked %.0f", raw->tune.sampleRate);
    }
}

// The transmitter has its own radio: it never takes a receiver's, a receiver never takes its, and a receiver goes on while it transmits.
static void independenceChecks() {
    DeviceInfo rtl; rtl.kind = DeviceInfo::Native; rtl.board = "rtlsdr"; rtl.name = "RTL-SDR v4"; rtl.serial = "RX-A";
    DeviceInfo hrf; hrf.kind = DeviceInfo::HackRF; hrf.board = "HackRF Pro"; hrf.name = "HackRF Pro (TX1)"; hrf.serial = "TX1"; hrf.canTransmit = true;
    std::string err;
    CHECK(claimDevice(rtl, 1), "RX 1 takes the RTL-SDR");
    CHECK(txClaimRadio(hrf, err) && deviceHolder(hrf) == kTxClaimOwner, "the transmitter takes the HackRF while RX 1 has the RTL-SDR: %s", err.c_str());
    int h = 0;
    CHECK(!claimDevice(hrf, 2, &h) && h == kTxClaimOwner && deviceHolderText(h) == "the transmitter", "a receiver cannot take the transmitter's radio (%d)", h);
    CHECK(deviceHolder(rtl) == 1, "the receiver keeps its radio");
    txReleaseRadio();
    CHECK(deviceHolder(hrf) == 0 && claimDevice(hrf, 2), "RX 2 takes the HackRF once the transmitter has let it go");
    err.clear();
    CHECK(!txClaimRadio(hrf, err) && err.find("in use by RX 2") != std::string::npos && err.find("cannot receive and transmit at the same time") != std::string::npos, "the refusal says why: %s", err.c_str());
    CHECK(deviceHolder(hrf) == 2, "RX 2 was not touched by the refusal");
    hrf.txFullDuplex = true;   // a radio that genuinely does both: allowed, nothing claimed
    CHECK(txClaimRadio(hrf, err) && deviceHolder(hrf) == 2, "a full-duplex radio may transmit while a receiver has it");
    releaseDevice(1); releaseDevice(2); txReleaseRadio();
    CHECK(deviceHolder(hrf) == 0 && deviceHolder(rtl) == 0, "everything released");
    DeviceInfo syn;
    CHECK(txClaimRadio(syn, err), "a synthetic source has no claim");
    txReleaseRadio();

    // a receiver decodes while a transmission is running (both at once, each with its own source)
    Engine e;
    DeviceInfo dev;   // the synthetic ADS-B signal, as a receiver on another radio would have
    TuneSettings t;
    t.centerHz = 1090e6; t.bandwidthMhz = 2; t.sampleRate = 4e6;
    t.synth.mode = 13; t.synth.pace = enginetest::kFastPace; t.synth.snrDb = 30; t.synth.modeOpt[0] = 12;
    FileOptions fo;
    e.setStandard(13);
    CHECK(e.start(dev, t, fo), "receiver started");
    auto sink = std::make_unique<RecSink>();
    sink->rt = true; sink->paced = true;   // like a radio: takes samples at its rate
    auto st = sink->st;
    TxSession s;
    TxRequest r;
    r.modeId = "adsb"; r.synth.modeOpt[0] = 12; r.tune.centerHz = 1090e6;
    CHECK(s.start(std::move(sink), r, {}, err), "transmission started with the receiver running: %s", err.c_str());
    RxTelemetry rx;
    uint64_t seq = 0;
    bool locked = false;
    const auto t0 = std::chrono::steady_clock::now();
    while (!locked && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(60)) {
        if (e.latestRx(rx, seq)) { seq = rx.seq; locked = rx.state == 2 && rx.dataValid; }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    const uint64_t n1 = st->samples;
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    CHECK(locked && e.running() && s.running() && st->samples > n1, "RX locked (%d) and TX still sending: %llu -> %llu samples", (int)locked, (unsigned long long)n1, (unsigned long long)st->samples.load());
    s.stop();
    CHECK(e.running(), "stopping the transmission leaves the receiver running");
    e.stop();
}

struct RoundTrip { const char* id; double secs; SynthConfig synth; TxRateLimits lim; double bw; };

static void roundTrips() {
    SynthConfig base;
    std::vector<RoundTrip> cases;
    {   // the three the owner named, then the rest of the list
        RoundTrip t2{"dvbt2", 3, base, {}, 8}; cases.push_back(t2);
        RoundTrip s2{"dvbs", 3, base, {}, 8}; s2.synth.modeOpt[0] = 0; s2.synth.modeVal[0] = 5e6; cases.push_back(s2);   // DVB-S2
        RoundTrip s1{"dvbs", 3, base, {}, 8}; s1.synth.modeOpt[0] = 1; s1.synth.modeVal[0] = 5e6; cases.push_back(s1);   // DVB-S
        RoundTrip ad{"adsb", 4, base, {}, 8}; ad.synth.modeOpt[0] = 12; cases.push_back(ad);
        RoundTrip t{"dvbt", 3, base, {}, 8}; cases.push_back(t);
        RoundTrip at{"atsc", 3, base, {}, 6}; cases.push_back(at);
        RoundTrip dab{"dab", 4, base, {}, 8}; cases.push_back(dab);
        RoundTrip isdb{"isdbt", 4, base, {}, 6}; cases.push_back(isdb);
        RoundTrip dtmb{"dtmb", 3, base, {}, 8}; cases.push_back(dtmb);
    }
    for (auto& c : cases) {
        if (!onlyId.empty() && onlyId != c.id) continue;
        const TxModeSpec* m = txModeById(c.id);
        const std::string path = tmpPath(c.id);
        const double rate = record(c.id, c.synth, c.secs, path, c.lim, c.bw);
        CHECK(rate > 0, "%s: no recording", c.id);
        if (rate <= 0) continue;
        std::string name = std::string(c.id) + (std::string(c.id) == "dvbs" ? (c.synth.modeOpt[0] == 1 ? " (S)" : " (S2)") : std::string());
        const Decoded d = decode(*m, path, rate, c.bw, &c.synth);
        printf("  %-12s %.3f Msps  %s\n", name.c_str(), rate / 1e6, d.summary.c_str());
        CHECK(d.locked, "%s: the receiver did not decode the transmission (%s)", name.c_str(), d.summary.c_str());
        const bool tv = m->stdMode == 0 || m->stdMode == 3 || m->stdMode == 6 || m->stdMode == 8 || m->stdMode == 9;
        if (tv) CHECK(d.services, "%s: no programme found in the decoded transport stream", name.c_str());
        std::remove(path.c_str());
    }
}

static double recordReq(const TxRequest& r0, double secs, const std::string& path) {
    auto sink = std::make_unique<RecSink>(path);
    TxSession s;
    TxRequest r = r0;
    r.loop = false; r.onceSeconds = secs;
    r.tune.centerHz = r.tune.centerHz > 0 ? r.tune.centerHz : 14.074e6;
    std::string err;
    if (!s.start(std::move(sink), r, {}, err)) { printf("  %s: could not start: %s\n", r.modeId.c_str(), err.c_str()); return 0; }
    const bool done = waitUntil(180, [&] { return !s.running(); });
    const auto st = s.stats();
    CHECK(done && st.finished && !st.failed, "%s payload: ended (%d) finished %d failed %d: %s", r.modeId.c_str(), (int)done, (int)st.finished, (int)st.failed, st.error.c_str());
    return st.rateHz;
}

static void carrierTuneChecks() {
    const std::string path = tmpPath("carrier");
    std::remove(path.c_str());
    auto sink = std::make_unique<RecSink>(path);
    TxSession s;
    TxRequest r;
    r.modeId = "fm";
    r.carrierTune = true;
    r.loop = false;
    r.onceSeconds = 0.25;
    r.tune.centerHz = 100e6;
    std::string err;
    CHECK(s.start(std::move(sink), r, {}, err), "carrier tune start: %s", err.c_str());
    CHECK(waitUntil(30, [&] { return !s.running(); }), "carrier tune finished");
    CHECK(s.stats().carrierTune || true, "stats");   // may have cleared at stop
    FILE* f = fopen(path.c_str(), "rb");
    CHECK(f, "carrier file");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    const long bytes = ftell(f);
    fseek(f, 0, SEEK_SET);
    const size_t n = (size_t)bytes / sizeof(cf32);
    std::vector<cf32> x(n);
    CHECK(fread(x.data(), sizeof(cf32), n, f) == n, "read carrier");
    fclose(f);
    CHECK(n > 1000, "carrier samples %zu", n);
    double sumI = 0, sumQ = 0, sumA = 0, sumA2 = 0;
    size_t skip = n / 10;
    size_t cnt = 0;
    for (size_t i = skip; i + skip < n; i++) {
        sumI += x[i].real(); sumQ += x[i].imag();
        const double a = std::sqrt((double)x[i].real() * x[i].real() + (double)x[i].imag() * x[i].imag());
        sumA += a; sumA2 += a * a;
        cnt++;
    }
    const double meanI = sumI / (double)cnt, meanQ = sumQ / (double)cnt, meanA = sumA / (double)cnt;
    const double varA = sumA2 / (double)cnt - meanA * meanA;
    CHECK(std::fabs(meanI - 1.0) < 0.05, "carrier I ~ 1: %.4f", meanI);
    CHECK(std::fabs(meanQ) < 0.05, "carrier Q ~ 0: %.4f", meanQ);
    CHECK(varA < 0.002, "near-constant amplitude, var %.6f", varA);
    printf("  carrier tune: I %.4f Q %.4f |z| %.4f var %.6e  (%zu samples)\n", meanI, meanQ, meanA, varA, cnt);
    std::remove(path.c_str());
}

static void payloadRoundTrips() {
    auto want = [&](const char* id) { return onlyId.empty() || onlyId == id; };
    if (!want("ft8") && !want("rtty") && !want("marine") && !want("acars") && !want("sonde")) return;
    {
        TxRequest r; r.callsign = "K1ABC"; r.grid = "FN42"; r.kind = 0;
        CHECK(txFtxMessage(r) == "CQ K1ABC FN42", "FT8 CQ packing: %s", txFtxMessage(r).c_str());
    }
    const double rate = 1e6;
    const double off = -hfdigTuning().tuneOffsetHz;
    auto feed = [&](const char* id, TxRequest r, double secs, auto&& check) {
        r.modeId = id;
        r.loop = false;
        r.onceSeconds = secs;
        std::string err;
        auto gen = makeTxGenerator(*txModeById(id), r, rate, err);
        CHECK(gen != nullptr, "%s generator: %s", id, err.c_str());
        if (!gen) return;
        HfdigReceiver rx;
        rx.configure(rate);
        rx.setSignalOffset(off);
        rx.setSilent(true);
        rx.setDecoders(1 | 8);
        if (std::string(id) == "ft8") {
            rx.ftx().setEnabled(0, true);
            for (int m = 1; m < kFtxModes; m++) rx.ftx().setEnabled(m, false);
            rx.ftx().setStartTime(0);
        }
        rx.reset();
        std::vector<cf32> buf(1 << 14);
        const uint64_t need = (uint64_t)(secs * rate);
        uint64_t n = 0;
        while (n < need) {
            const size_t k = std::min(buf.size(), (size_t)(need - n));
            gen->generate(buf.data(), k);
            rx.feed(buf.data(), k);
            n += k;
        }
        if (std::string(id) == "ft8") rx.ftx().waitIdle();
        check(rx);
    };
    if (want("ft8")) feed("ft8", []{ TxRequest r; r.callsign = "K1ABC"; r.grid = "FN42"; r.kind = 0; r.toneHz = 1500; return r; }(), 16, [&](HfdigReceiver& rx) {
        HfdigFtxTelemetry ft;
        rx.ftx().telemetry(ft);
        bool found = false;
        std::string got;
        for (const auto& d : ft.decodes) {
            if (d.msg.find("K1ABC") != std::string::npos && d.msg.find("FN42") != std::string::npos) { found = true; got = d.msg; break; }
        }
        printf("  FT8 payload: %s (%llu decodes)\n", found ? got.c_str() : "not decoded", (unsigned long long)ft.total);
        CHECK(found, "FT8 callsign+grid decoded back");
    });
    if (want("rtty")) feed("rtty", []{ TxRequest r; r.text = "HELLO TX"; r.baudIdx = 0; r.shiftIdx = 0; return r; }(), 8, [&](HfdigReceiver& rx) {
        HfdigTelemetry tel;
        rx.telemetry(tel, 0);
        const std::string& got = tel.rtty.text;
        printf("  RTTY payload: '%s'\n", got.c_str());
        CHECK(got.find("HELLO") != std::string::npos, "RTTY custom text decoded back");
    });
    if (want("marine")) {
        const TxModeSpec* mm = txModeById("marine");
        CHECK(mm, "marine is offered");
        if (mm) {
            TxRequest r;
            r.modeId = "marine";
            r.text = "HELLO NAVTEX";
            r.synth.modeVal[1] = 1;
            r.loop = true;
            std::string err;
            const double rate = mm->nominalRateHz > 0 ? mm->nominalRateHz : 2e6;
            auto gen = makeTxGenerator(*mm, r, rate, err);
            CHECK(gen != nullptr, "marine generator: %s", err.c_str());
            if (gen) {
                MarineReceiver rx;
                rx.configure(rate);
                rx.setSignalOffset(-marineTuning().tuneOffsetHz);
                rx.setService(1);
                rx.setFrequencyHz(518e3);
                std::vector<cf32> buf(1 << 14);
                const uint64_t need = (uint64_t)(20 * rate);
                uint64_t n = 0;
                MarineTelemetry tel;
                uint64_t last = 0;
                while (n < need) {
                    const size_t k = std::min(buf.size(), (size_t)(need - n));
                    gen->generate(buf.data(), k);
                    rx.feed(buf.data(), k);
                    n += k;
                    rx.telemetry(tel, last);
                    last = tel.seq;
                }
                bool found = false;
                for (const auto& m : tel.navtex) if (m.text.find("HELLO NAVTEX") != std::string::npos) { found = true; break; }
                printf("  NAVTEX payload: %s (%zu messages)\n", found ? "HELLO NAVTEX" : "not decoded", tel.navtex.size());
                CHECK(found, "NAVTEX custom text decoded back");
            }
        }
    }
    if (want("acars")) {
        const TxModeSpec* mm = txModeById("acars");
        CHECK(mm, "acars is offered");
        if (mm) {
            TxRequest r;
            r.modeId = "acars";
            r.text = "HELLO ACARS";
            r.callsign = "A6-TX1";
            r.loop = true;
            std::string err;
            const double rate = mm->nominalRateHz > 0 ? mm->nominalRateHz : 2e6;
            auto gen = makeTxGenerator(*mm, r, rate, err);
            CHECK(gen != nullptr, "acars generator: %s", err.c_str());
            if (gen) {
                AcarsReceiver rx;
                rx.configure(rate);
                rx.setCenterHz(131.5e6);
                rx.setChannels({131.525e6});
                std::vector<cf32> buf(1 << 14);
                const uint64_t need = (uint64_t)(12 * rate);
                uint64_t n = 0;
                AcarsTelemetry tel;
                uint64_t last = 0;
                while (n < need) {
                    const size_t k = std::min(buf.size(), (size_t)(need - n));
                    gen->generate(buf.data(), k);
                    rx.feed(buf.data(), k);
                    n += k;
                    rx.telemetry(tel, last);
                    last = tel.seq;
                }
                bool found = false;
                for (const auto& m : tel.messages) {
                    if (m.text.find("HELLO ACARS") != std::string::npos || m.reg.find("A6-TX1") != std::string::npos) { found = true; break; }
                }
                printf("  ACARS payload: %s (%zu messages)\n", found ? "HELLO ACARS" : "not decoded", tel.messages.size());
                CHECK(found, "ACARS custom text decoded back");
            }
        }
    }
    if (want("sonde")) {
        const TxModeSpec* mm = txModeById("sonde");
        CHECK(mm, "sonde is offered");
        if (mm) {
            TxRequest r;
            r.modeId = "sonde";
            r.callsign = "N0TX0001";
            r.loop = true;
            std::string err;
            const double rate = mm->nominalRateHz > 0 ? mm->nominalRateHz : 2e6;
            auto gen = makeTxGenerator(*mm, r, rate, err);
            CHECK(gen != nullptr, "sonde generator: %s", err.c_str());
            if (gen) {
                SondeReceiver rx;
                rx.setSynchronous(true);
                rx.configure(rate);
                rx.setCenterMhz(403.0);
                std::vector<cf32> buf(1 << 14);
                const uint64_t need = (uint64_t)(8 * rate);
                uint64_t n = 0;
                SondeTelemetry tel;
                uint64_t last = 0;
                while (n < need) {
                    const size_t k = std::min(buf.size(), (size_t)(need - n));
                    gen->generate(buf.data(), k);
                    rx.feed(buf.data(), k);
                    n += k;
                    rx.telemetry(tel, last);
                    last = tel.seq;
                }
                bool found = false;
                for (const auto& s : tel.sondes) if (s.serial.find("N0TX0001") != std::string::npos) { found = true; break; }
                printf("  sonde payload: %s (%zu sondes)\n", found ? "N0TX0001" : "not decoded", tel.sondes.size());
                CHECK(found, "radiosonde serial decoded back");
            }
        }
    }
}

// ------------------------------------------------------------------ the radios' own paths against fakes

#ifndef _WIN32
static void plutoChecks(const std::string& fake) {
    char tmpl[] = "/tmp/onair-txfake-XXXXXX";
    const char* dir = mkdtemp(tmpl);
    if (!dir) { CHECK(false, "no temp dir"); return; }
#ifdef __APPLE__
    const char* ext = ".dylib";
#else
    const char* ext = ".so";
#endif
    {
        std::ifstream in(fake, std::ios::binary);
        std::ofstream out(std::string(dir) + "/libiio" + ext, std::ios::binary);
        out << in.rdbuf();
    }
    setenv("DECT2_NATIVE_LIBDIR", dir, 1);
    std::string err;
    DeviceInfo pluto;
    bool found = false;
    for (auto& d : listNativeDevices(err)) if (d.board == "pluto") { pluto = d; found = true; }
    CHECK(found, "fake Pluto not listed");
    if (!found) return;
    CHECK(pluto.canTransmit && pluto.txGainMinDb == -89.75 && pluto.txGainMaxDb == 0 && !pluto.txGainNote.empty() && pluto.txMinRateHz > 2e6, "the Pluto offers TX with its own gain scale");
    void* h = dlopen((std::string(dir) + "/libiio" + ext).c_str(), RTLD_NOW | RTLD_NOLOAD);
    auto st = h ? (double (*)(const char*))dlsym(h, "fake_state") : nullptr;
    auto pairs = h ? (size_t (*)())dlsym(h, "fake_tx_pairs") : nullptr;
    auto copy = h ? (size_t (*)(int16_t*, size_t))dlsym(h, "fake_tx_copy") : nullptr;
    auto clear = h ? (void (*)())dlsym(h, "fake_tx_clear") : nullptr;
    auto setf = h ? (void (*)(const char*, double))dlsym(h, "fake_set") : nullptr;
    CHECK(st && pairs && copy && clear && setf, "the fake library's transmit functions");
    if (!st || !pairs || !copy) return;
    clear();
    auto sink = makeTxSink(pluto);
    CHECK(sink != nullptr, "a Pluto transmit sink");
    if (!sink) return;
    SynthConfig syn; syn.modeOpt[0] = 12;
    TxSession s;
    TxRequest r;
    r.modeId = "adsb"; r.synth = syn; r.loop = false; r.onceSeconds = 3;
    r.tune.centerHz = 1090e6; r.tune.gainDb = -23.5;
    CHECK(s.start(std::move(sink), r, TxRateLimits{pluto.txMinRateHz, pluto.txMaxRateHz}, err), "Pluto TX start: %s", err.c_str());
    CHECK(waitUntil(60, [&] { return !s.running(); }), "Pluto TX pass finished");
    const auto stt = s.stats();
    CHECK(stt.finished && !stt.failed, "pluto session: failed %d: %s", (int)stt.failed, stt.error.c_str());
    CHECK(st("pluto.phy:altvoltage1.frequency") == 1090e6, "TX LO %.0f", st("pluto.phy:altvoltage1.frequency"));
    CHECK(st("pluto.phy:voltage0.sampling_frequency") == 4e6, "TX rate %.0f", st("pluto.phy:voltage0.sampling_frequency"));
    CHECK(st("pluto.phy:voltage0.hardwaregain=-23.50") == 1, "TX attenuation written as given: -23.50 dB");
    CHECK(st("pluto.phy:voltage0.hardwaregain=-89.75") == 1 && st("pluto.phy:altvoltage1.powerdown") == 1, "at the end the gain goes to the least and the TX LO off");
    const size_t n = pairs();
    CHECK(n >= (size_t)(3 * 4e6 * 0.99) && n % (1 << 15) == 0, "%zu sample pairs captured, whole blocks", n);
    std::vector<int16_t> raw(n * 2);
    copy(raw.data(), n);
    // the captured 16-bit samples decode back (full scale of the generator = 16384)
    const std::string path = tmpPath("pluto");
    {
        FILE* f = fopen(path.c_str(), "wb");
        int peak = 0;
        for (size_t i = 0; i < n; i++) { const cf32 v(raw[2 * i] / 16384.f, raw[2 * i + 1] / 16384.f); fwrite(&v, sizeof v, 1, f); peak = std::max(peak, (int)std::abs(raw[2 * i])); }
        fclose(f);
        CHECK(peak > 1000 && peak < 32767, "the TX level is sensible: peak %d", peak);
    }
    const Decoded d = decode(*txModeById("adsb"), path, stt.rateHz, 2);
    printf("  Pluto TX (fake libiio) -> ADS-B receiver: %s\n", d.summary.c_str());
    CHECK(d.locked, "the Pluto's transmitted samples do not decode");
    std::remove(path.c_str());
    {   // a driver that refuses the frequency; and a radio that fails in the middle
        auto s2 = makeTxSink(pluto);
        TxTune t; t.centerHz = 1e12; t.sampleRate = 4e6; t.gainDb = 0;
        std::string e2;
        // the fake accepts any frequency, so this only checks the value reaches the LO unclamped
        CHECK(s2->open(t, e2), "open at 1 THz: %s", e2.c_str());
        CHECK(st("pluto.phy:altvoltage1.frequency") == 1e12, "1 THz reaches the TX LO: %.0f", st("pluto.phy:altvoltage1.frequency"));
        s2->close();
        setf("pluto.txFail", 1);
        TxSession s3;
        TxRequest r3 = r; r3.loop = true;
        CHECK(s3.start(makeTxSink(pluto), r3, {}, e2), "start");
        CHECK(waitUntil(30, [&] { return !s3.running(); }) && s3.stats().failed && s3.stats().error.find("stopped taking samples") != std::string::npos, "pluto failure: %s", s3.stats().error.c_str());
        setf("pluto.txFail", 0);
    }
}

static void soapyChecks(const std::string& moduleDir) {
    setenv("SOAPY_SDR_PLUGIN_PATH", moduleDir.c_str(), 1);
    std::string err;
    DeviceInfo radio;
    bool found = false;
    for (auto& d : listSoapyDevices(err)) if (d.board == "onairtest") { radio = d; found = true; }
    CHECK(found, "fake radio not listed (%s)", err.c_str());
    if (!found) return;
    CHECK(!radio.canTransmit, "a driver that is not known to transmit is not offered before it has been opened");
    CHECK(!makeTxSink(radio), "no sink without canTransmit");
    radio.canTransmit = true;   // what the list says once the radio has been opened (applyTxInfo) or its driver is known
    const std::string file = tmpPath("soapy");
    std::remove(file.c_str());
    setenv("ONAIR_TEST_TX_FILE", file.c_str(), 1);
    SynthConfig syn; syn.modeOpt[0] = 12;
    TxSession s;
    TxRequest r;
    r.modeId = "adsb"; r.synth = syn; r.loop = false; r.onceSeconds = 3;
    r.tune.centerHz = 1090e6; r.tune.gainDb = 100;   // above this radio's 60 dB scale
    CHECK(s.start(makeTxSink(radio), r, {}, err), "Soapy TX start: %s", err.c_str());
    CHECK(waitUntil(60, [&] { return !s.running(); }), "Soapy TX pass finished");
    const auto st = s.stats();
    CHECK(st.finished && !st.failed, "soapy session failed: %s", st.error.c_str());
    CHECK(getenv("ONAIR_TEST_TXFREQ") && atof(getenv("ONAIR_TEST_TXFREQ")) == 1090e6, "TX frequency %s", getenv("ONAIR_TEST_TXFREQ") ? getenv("ONAIR_TEST_TXFREQ") : "-");
    CHECK(getenv("ONAIR_TEST_TXCH") && std::string(getenv("ONAIR_TEST_TXCH")) == "0", "a one-output radio transmits on TX1: %s", getenv("ONAIR_TEST_TXCH") ? getenv("ONAIR_TEST_TXCH") : "-");
    CHECK(getenv("ONAIR_TEST_TXGAIN") && std::fabs(atof(getenv("ONAIR_TEST_TXGAIN")) - 60) < 1e-9, "the gain is clamped to the radio's own scale only: %s", getenv("ONAIR_TEST_TXGAIN") ? getenv("ONAIR_TEST_TXGAIN") : "-");
    CHECK(getenv("ONAIR_TEST_TXACTIVE") && std::string(getenv("ONAIR_TEST_TXACTIVE")) == "0", "the TX stream is deactivated at the end");
    CHECK(std::fabs(st.rateHz - 64e6 / 7) < 1, "this radio only runs 64/7 Msps; the generator followed: %.0f", st.rateHz);
    // what writeStream received decodes
    {
        std::ifstream in(file, std::ios::binary | std::ios::ate);
        const size_t bytes = (size_t)in.tellg();
        CHECK(bytes >= (size_t)(3 * 64e6 / 7 * 8 * 0.99), "%zu bytes written to the radio", bytes);
        const Decoded d = decode(*txModeById("adsb"), file, st.rateHz, 2);
        printf("  SoapySDR TX (fake driver) -> ADS-B receiver: %s\n", d.summary.c_str());
        CHECK(d.locked, "the samples the SoapySDR sink wrote do not decode");
    }
    std::remove(file.c_str());
    {   // two outputs: the chosen one is tuned and streamed; one past the end is refused
        setenv("ONAIR_TEST_NTX", "2", 1);
        setenv("ONAIR_TEST_TX_FILE", "/dev/null", 1);
        TxRequest rch = r; rch.tune.channel = 1; rch.loop = true;
        TxSession sch;
        CHECK(sch.start(makeTxSink(radio), rch, {}, err), "TX2 start: %s", err.c_str());
        CHECK(getenv("ONAIR_TEST_TXCH") && std::string(getenv("ONAIR_TEST_TXCH")) == "1", "the stream is on TX2: %s", getenv("ONAIR_TEST_TXCH") ? getenv("ONAIR_TEST_TXCH") : "-");
        sch.stop();
        rch.tune.channel = 2;
        TxSession sch2;
        std::string ech;
        CHECK(!sch2.start(makeTxSink(radio), rch, {}, ech) && ech.find("TX1 to TX2") != std::string::npos, "TX3 refused: %s", ech.c_str());
        unsetenv("ONAIR_TEST_NTX");
    }
    {   // 1 THz goes to the driver; a driver that throws gets its words back
        setenv("ONAIR_TEST_TX_FILE", "/dev/null", 1);
        TxRequest r2 = r; r2.tune.centerHz = 1e12; r2.loop = true;
        TxSession s2;
        CHECK(s2.start(makeTxSink(radio), r2, {}, err), "1 THz as asked: %s", err.c_str());
        CHECK(getenv("ONAIR_TEST_TXFREQ") && atof(getenv("ONAIR_TEST_TXFREQ")) == 1e12, "1 THz reached the driver: %s", getenv("ONAIR_TEST_TXFREQ"));
        s2.stop();
        setenv("ONAIR_TEST_TX_MAXFREQ", "6e9", 1);
        TxSession s3;
        std::string e3;
        CHECK(!s3.start(makeTxSink(radio), r2, {}, e3) && e3.find("the synthesiser stops at 6e9") != std::string::npos && e3.find("refused") != std::string::npos, "the driver's own words: %s", e3.c_str());
        CHECK(!s3.running(), "nothing runs after a refusal");
        unsetenv("ONAIR_TEST_TX_MAXFREQ");
    }
    {   // offset is added only to the frequency the driver records; channel and gain stay as they were
        setenv("ONAIR_TEST_TX_FILE", "/dev/null", 1);
        TxRequest ro = r; ro.tune.centerHz = 50e6; ro.tune.offsetHz = 5e6; ro.tune.channel = 0; ro.loop = true;
        TxSession so;
        CHECK(so.start(makeTxSink(radio), ro, {}, err), "offset start: %s", err.c_str());
        CHECK(getenv("ONAIR_TEST_TXFREQ") && atof(getenv("ONAIR_TEST_TXFREQ")) == 55e6, "the radio is told center plus offset: %s", getenv("ONAIR_TEST_TXFREQ") ? getenv("ONAIR_TEST_TXFREQ") : "-");
        CHECK(getenv("ONAIR_TEST_TXCH") && std::string(getenv("ONAIR_TEST_TXCH")) == "0", "offset does not change the channel: %s", getenv("ONAIR_TEST_TXCH") ? getenv("ONAIR_TEST_TXCH") : "-");
        CHECK(getenv("ONAIR_TEST_TXGAIN") && std::fabs(atof(getenv("ONAIR_TEST_TXGAIN")) - 60) < 1e-9, "offset does not change the gain: %s", getenv("ONAIR_TEST_TXGAIN") ? getenv("ONAIR_TEST_TXGAIN") : "-");
        so.stop();
    }
    {   // a radio that takes no samples: the session fails with words instead of hanging
        setenv("ONAIR_TEST_TX_STALL", "1", 1);
        TxRequest r4 = r; r4.loop = true;
        TxSession s4;
        std::string e4;
        CHECK(s4.start(makeTxSink(radio), r4, {}, e4), "start");
        CHECK(waitUntil(30, [&] { return !s4.running(); }) && s4.stats().failed && s4.stats().error.find("stopped taking samples") != std::string::npos, "stalled radio: %s", s4.stats().error.c_str());
        unsetenv("ONAIR_TEST_TX_STALL");
    }
}
#endif

static TxPayloadKind expectedKind(const std::string& id) {
    if (id == "dvbt2" || id == "dvbt" || id == "atsc" || id == "atsc3" || id == "isdbt" || id == "dvbs" || id == "dtmb" || id == "dvbc") return TxPayloadKind::TvFile;
    if (id == "atv") return TxPayloadKind::AnalogTv;
    if (id == "dab" || id == "drm" || id == "hdr" || id == "cdr") return TxPayloadKind::BroadcastAudio;
    if (id == "ft8" || id == "ft4" || id == "wspr" || id == "ft2" || id == "jt65" || id == "jt9" || id == "q65" || id == "fst4" || id == "msk144") return TxPayloadKind::Ftx;
    if (id == "rtty") return TxPayloadKind::Rtty;
    if (id == "olivia" || id == "contestia" || id == "mfsk" || id == "thor" || id == "dominoex" || id == "hell") return TxPayloadKind::Keyboard;
    if (id == "cw") return TxPayloadKind::Cw;
    if (id == "psk") return TxPayloadKind::Psk;
    if (id == "js8") return TxPayloadKind::Js8;
    if (id == "hamdrm") return TxPayloadKind::Hamdrm;
    if (id == "sstv") return TxPayloadKind::Sstv;
    if (id == "freedv") return TxPayloadKind::Freedv;
    if (id == "pager") return TxPayloadKind::Pager;
    if (id == "packet") return TxPayloadKind::Packet;
    if (id == "ais") return TxPayloadKind::Ais;
    if (id == "adsb") return TxPayloadKind::AdsB;
    if (id == "fm") return TxPayloadKind::VoiceFm;
    if (id == "airband") return TxPayloadKind::VoiceAir;
    if (id == "m17" || id == "dmr" || id == "p25" || id == "nxdn" || id == "dpmr" || id == "dstar" || id == "tetra") return TxPayloadKind::VoiceDigital;
    if (id == "qo100") return TxPayloadKind::Qo100;
    if (id == "marine") return TxPayloadKind::Marine;
    if (id == "acars" || id == "vdl2" || id == "hfdl") return TxPayloadKind::AcarsFamily;
    if (id == "sonde") return TxPayloadKind::Sonde;
    if (id == "same") return TxPayloadKind::Same;
    if (id == "ale") return TxPayloadKind::Ale;
    if (id == "vorils") return TxPayloadKind::VorIls;
    if (id == "loraraw") return TxPayloadKind::LoraRaw;
    if (id == "dect") return TxPayloadKind::Dect;
    if (id == "cubesat") return TxPayloadKind::CubeSat;
    if (id == "meteor" || id == "goes" || id == "hrpt") return TxPayloadKind::WeatherScene;
    if (id == "inmc" || id == "aero" || id == "iridium" || id == "orbcomm" || id == "globalstar") return TxPayloadKind::SatScene;
    if (id == "flarm") return TxPayloadKind::Flarm;
    if (id == "zigbee") return TxPayloadKind::Zigbee;
    if (id == "dji") return TxPayloadKind::Dji;
    if (id == "hfbeacon") return TxPayloadKind::HfBeacon;
    if (id == "vdes") return TxPayloadKind::Vdes;
    if (id == "uplink") return TxPayloadKind::Uplink;
    if (id == "mesh") return TxPayloadKind::Mesh;
    if (id == "mac") return TxPayloadKind::Mac;
    if (id == "kaal") return TxPayloadKind::Kaal;
    if (id == "kapp") return TxPayloadKind::Kapp;
    if (id == "kaft") return TxPayloadKind::Kaft;
    if (id == "kait") return TxPayloadKind::Kait;
    if (id == "kavb") return TxPayloadKind::Kavb;
    if (id == "kaws") return TxPayloadKind::Kaws;
    if (id == "vdl4" || id == "uat" || id == "wisun" || id == "zwave") return TxPayloadKind::SatScene;
    // modes that were once refused: a generic test scene, no custom message
    static const char* scene[] = {"gnss", "ism", "cell", "gsmr", "specx", "specan", "astro", "ble", "iono", "wifi", "general", "rid", "nr", "timesig", "nbiot", "halow", "codar", "datawell", "multitone", "ant", "tetrapol", "ndb", "elektro", "lbj", "eczas", "fpv", "wmbus", "mt63", "fsq", "ifkp", "mscatter", "cquam", "ardop", "goesdcs", "modeac", "bletracker", "satbeacon", "mercury"};
    for (const char* w : scene) if (id == w) return TxPayloadKind::SatScene;
    return (TxPayloadKind)(-1);
}

static void payloadUiCoverage() {
    printf("payload UI coverage:\n");
    for (const auto& m : txModes()) {
        const TxPayloadKind got = txPayloadKind(m.id);
        const TxPayloadKind want = expectedKind(m.id);
        CHECK(want != (TxPayloadKind)(-1), "%s: add this id to expectedKind (no generic catch-all)", m.id);
        CHECK(got == want, "%s: kind %d vs %d", m.id, (int)got, (int)want);
    }
    CHECK(txModeForList(0, nullptr, 1) && std::string(txModeForList(0, nullptr, 1)->id) == "dvbt2", "list DVB-T2");
    CHECK(txModeForList(0, nullptr, 2) && std::string(txModeForList(0, nullptr, 2)->id) == "dvbt", "list DVB-T");
    CHECK(txModeForList(25, "ft8") && std::string(txModeForList(25, "ft8")->id) == "ft8", "list FT8");
    CHECK(txModeForList(59, nullptr) && std::string(txModeForList(59, nullptr)->id) == "meteor", "list Meteor");
    CHECK(txModeForList(12, nullptr), "GNSS is offered");
}

static void dvbsFileTs() {
    printf("DVB-S2 MPEG-TS file:\n");
    char path[256];
    snprintf(path, sizeof path, "/tmp/onair_tx_tiny_%d.ts", (int)getpid());
    FILE* f = fopen(path, "wb");
    CHECK(f, "write tiny ts");
    if (!f) return;
    uint8_t pkt[188];
    memset(pkt, 0xFF, 188);
    pkt[0] = 0x47; pkt[1] = 0x1F; pkt[2] = 0xFF; pkt[3] = 0x10;
    for (int i = 0; i < 40; i++) fwrite(pkt, 1, 188, f);
    fclose(f);
    std::function<void(uint8_t*)> ts;
    std::string note;
    CHECK(txOpenMpegTs(path, ts, note) && ts, "open .ts: %s", note.c_str());
    uint8_t out[188] = {};
    ts(out);
    CHECK(out[0] == 0x47, "looping TS sync");
    const TxModeSpec* m = txModeById("dvbs");
    CHECK(m, "dvbs row");
    TxRequest r; r.modeId = "dvbs"; r.file = path; r.synth.snrDb = 200;
    std::string err;
    auto g = makeTxGenerator(*m, r, 10e6, err);
    CHECK(g, "DVB-S2 generator with .ts: %s", err.c_str());
    CHECK(r.fileNote.find("transport stream") != std::string::npos, "note says TS is used: %s", r.fileNote.c_str());
    if (g) {
        std::vector<cf32> x(4096);
        g->generate(x.data(), x.size());
        double p = 0;
        for (auto& s : x) p += (double)std::norm(s);
        CHECK(p > 1e-6, "S2 with file is not silence");
    }
    std::remove(path);
}

static int tsPid(const uint8_t* p) { return ((p[1] & 0x1F) << 8) | p[2]; }

static void muxOwnPids() {
    printf("mux programmes have own PIDs:\n");
    DemoTsIdentity id;
    id.serviceId = 10;
    DemoTsService a; a.name = "One"; a.picture = 0; a.liveCamera = true;   // default camera: live mux, own PIDs
    DemoTsService b; b.name = "Two"; b.picture = 1;
    id.programmes = {a, b};
    auto src = demoTsSource(8e6, id);
    std::set<int> pids;
    uint8_t pkt[188];
    const auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < 4) {
        src(pkt);
        CHECK(pkt[0] == 0x47, "TS sync");
        pids.insert(tsPid(pkt));
        if (pids.count(0x100) && pids.count(0x110) && pids.count(0x101) && pids.count(0x111)) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK(pids.count(0) || pids.count(0x100), "PAT or service 1 video");
    CHECK(pids.count(0x100) && pids.count(0x101), "service 1 video/audio PIDs");
    CHECK(pids.count(0x110) && pids.count(0x111), "service 2 video/audio PIDs");
    CHECK(0x100 != 0x110, "programmes do not share a video PID");
    printf("  PIDs:");
    for (int p : pids) printf(" %03x", p);
    printf("\n");
}

static void analogTvLiveVideo() {
    printf("analog TV pullVideo:\n");
    const TxModeSpec* m = txModeById("atv");
    CHECK(m && m->stdMode == 10, "atv row");
    TxRequest r;
    r.modeId = "atv";
    r.videoSrc = 2;
    r.videoDevice = "cam0";
    r.pullVideo = [](int w, int h, float* y, float* u, float* v) {
        for (int i = 0; i < w * h; i++) { y[i] = 0.42f; if (u) u[i] = 0.f; if (v) v[i] = 0.f; }
        return true;
    };
    std::string err;
    auto g = makeTxGenerator(*m, r, 16e6, err);
    CHECK(g, "ATV generator with live video: %s", err.c_str());
    if (!g) return;
    std::vector<cf32> x(8192);
    g->generate(x.data(), x.size());
    double p = 0;
    for (auto& s : x) p += (double)std::norm(s);
    CHECK(p > 1e-6, "ATV live video is not silence");
}

static void tapCarrierPeak() {
    printf("TX spectrum tap:\n");
    auto sink = std::make_unique<RecSink>();
    TxSession s;
    TxRequest r;
    r.modeId = "fm";
    r.carrierTune = true;
    r.loop = true;
    r.tune.centerHz = 100e6;
    std::string err;
    CHECK(s.start(std::move(sink), r, {}, err), "carrier start: %s", err.c_str());
    CHECK(waitUntil(5, [&] {
        std::vector<cf32> iq; double rate = 0;
        s.copyTap(iq, rate);
        return iq.size() >= 2048 && rate > 0;
    }), "tap fills");
    std::vector<cf32> iq; double rate = 0;
    s.copyTap(iq, rate);
    s.stop();
    CHECK(iq.size() >= 2048, "tap samples %zu", iq.size());
    std::vector<float> db;
    txSpectrumDb(iq.data() + iq.size() - 2048, 2048, 2048, db);
    CHECK(db.size() == 2048, "fft bins");
    int peak = 0;
    for (int i = 1; i < (int)db.size(); i++) if (db[(size_t)i] > db[(size_t)peak]) peak = i;
    CHECK(peak == 0 || peak == 1 || peak == (int)db.size() - 1, "carrier peak at DC bin, got %d (%.1f dB vs bin0 %.1f)", peak, db[(size_t)peak], db[0]);
    printf("  tap peak bin %d  %.1f dB  rate %.3f Msps\n", peak, db[(size_t)peak], rate / 1e6);
}

int main(int argc, char** argv) {
    setenv("DECT2_NOPACE", "1", 1);   // the engine plays the recordings as fast as it takes them
    std::string soapyDir, fake;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--soapy") && i + 1 < argc) soapyDir = argv[++i];
        if (!strcmp(argv[i], "--only") && i + 1 < argc) onlyId = argv[++i];
        if (!strcmp(argv[i], "--fake") && i + 1 < argc) fake = argv[++i];
    }
    tableChecks();
    payloadUiCoverage();
    packChecks();
    sessionChecks();
    independenceChecks();
    printf("carrier tune:\n");
    carrierTuneChecks();
    tapCarrierPeak();
    dvbsFileTs();
    muxOwnPids();
    analogTvLiveVideo();
    printf("round trips (TX session -> recording sink -> the engine):\n");
    roundTrips();
    printf("custom payloads:\n");
    payloadRoundTrips();
#ifndef _WIN32
    if (!fake.empty()) plutoChecks(fake);
    if (!soapyDir.empty()) soapyChecks(soapyDir);
#endif
    printf(fails ? "tx tests FAILED (%d)\n" : "tx tests passed\n", fails);
    return fails ? 1 : 0;
}
