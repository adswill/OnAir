// Whole-span TV for the other transport-stream standards (docs/WHOLESPAN-TV-DESIGN.md 2.2-2.7): several channels of one standard side by side
// in one wide capture, each from that standard's own test generator with its own named service, plus noise. Every channel must get its own
// receiver, lock and show its service in the merged list. DVB-C is free frequency (the span analysis finds the carriers and their symbol rates,
// here two adjacent 8 MHz cable channels at different rates); ISDB-T and ATSC are on the 6 MHz raster, DTMB on the 8 MHz raster.
// Pick one case with an argument (dvbc, isdbt, atsc, dtmb); no argument runs them all.
#include "dect2/atsc/atsc_gen.h"
#include "dect2/dsp/exact_resampler.h"
#include "dect2/dtmb/dtmb_gen.h"
#include "dect2/dvbc/dvbc_rx.h"
#include "dect2/dvbt/dvbt_gen.h"
#include "dect2/engine/mode_synth.h"
#include "dect2/engine/spantv/spantv.h"
#include "dect2/isdbt/isdbt_gen.h"
#include "dect2/common/stable_random.h"
#include "dect2/t2/t2rx.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <random>
#include <thread>
using namespace dect2;
#include "../common/test_util.h"

// one channel: a generator that makes samples at the span rate already shifted to its offset (unit rms before `gain`)
struct Chan {
    virtual ~Chan() = default;
    virtual void pull(size_t n, std::vector<cf32>& out) = 0;   // adds to out
    double offsetHz = 0;
    std::string name;
};

struct SynthChan : Chan {   // a ModeSynth (DVB-C, DTMB) at the span rate, the offset as its carrier offset
    std::unique_ptr<ModeSynth> s;
    float gain = 1;
    std::vector<cf32> t;
    void pull(size_t n, std::vector<cf32>& out) override {
        t.resize(n);
        s->generate(t.data(), n);
        for (size_t i = 0; i < n; i++) out[i] += gain * t[i];
    }
};

struct AtscChan : Chan {
    std::unique_ptr<atsc::Generator> g;
    float gain = 1;
    std::vector<cf32> t;
    void pull(size_t n, std::vector<cf32>& out) override {
        t.clear();
        g->generate(n, t);
        for (size_t i = 0; i < n; i++) out[i] += gain * t[i];
    }
};

struct IsdbtChan : Chan {   // native rate frames, resampled to the span rate, then shifted
    std::unique_ptr<isdbt::Generator> g;
    ExactResampler rs;
    std::vector<cf32> ready;
    double rate = 0;
    uint64_t pos = 0;
    float gain = 1;
    void pull(size_t n, std::vector<cf32>& out) override {
        while (ready.size() < n) { std::vector<cf32> f; g->nextFrame(f); rs.process(f.data(), f.size(), ready); }
        const double w = 2 * M_PI * offsetHz / rate;
        for (size_t i = 0; i < n; i++) {
            const double ph = w * (double)(pos + i);
            out[i] += gain * ready[i] * cf32((float)std::cos(ph), (float)std::sin(ph));
        }
        ready.erase(ready.begin(), ready.begin() + (long)n);
        pos += n;
    }
};

struct Result { std::vector<TvChannelRow> rows; std::vector<MultiService> svc; int running = 0; };

static Result run(const char* title, int stdMode, double widthMhz, double rate, std::vector<std::unique_ptr<Chan>>& ch, double secs, double noiseRms, int maxCh = 3) {
    printf("%s\n", title);
    SpanTv tv;
    SpanTvOptions o; o.stdMode = stdMode; o.spanRate = rate; o.widthMhz = widthMhz; o.autoGrid = true; o.maxChannels = maxCh; o.blocking = true;
    CHECK(tv.configure(o), "configure");
    std::mt19937 rng(9);
    dect2::normal_distribution<float> nd(0.f, (float)(noiseRms / std::sqrt(2.0)));
    std::vector<cf32> blk;
    RxTelemetry t;
    const auto t0 = std::chrono::steady_clock::now();
    for (size_t done = 0; done < (size_t)(rate * secs); done += 1 << 16) {
        blk.assign(1 << 16, cf32(0, 0));
        for (auto& c : ch) c->pull(blk.size(), blk);
        for (auto& v : blk) { v += cf32{nd(rng), nd(rng)}; v *= 0.12f; }
        tv.feed(blk.data(), blk.size());
        tv.poll(t);
    }
    tv.flush();
    Result r;
    for (int i = 0; i < 200; i++) { tv.poll(t); r.svc = tv.services(); if (r.svc.size() >= ch.size()) break; std::this_thread::sleep_for(std::chrono::milliseconds(20)); }
    r.rows = tv.channels();
    r.running = (int)tv.runningSlots().size();
    for (const auto& row : r.rows)
        printf("  signal %d %+7.2f MHz bw %.2f MHz%s%s%s state %d snr %.1f dB, %d services, CPU %.0f %% %s\n", row.slot, row.offsetHz / 1e6, row.bwHz / 1e6, row.occupied ? " occupied" : " empty",
               row.running ? " running" : "", row.locked ? " locked" : "", row.state, row.snrDb, row.services, row.cpuPct, row.params.c_str());
    for (const auto& s : r.svc) printf("  service %d \"%s\" on signal %d (%+.2f MHz)\n", s.svc.id, s.svc.name.c_str(), s.slot, s.offsetHz / 1e6);
    printf("  %d receivers, %.1f s of signal in %.1f s\n", r.running, secs, std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
    tv.reset();
    return r;
}

static void expectAll(const char* what, const Result& r, const std::vector<std::unique_ptr<Chan>>& ch, double tol) {
    CHECK(r.running == (int)ch.size(), "%s: %d receivers, want %zu", what, r.running, ch.size());
    for (const auto& c : ch) {
        bool locked = false, svc = false;
        for (const auto& row : r.rows) if (std::fabs(row.offsetHz - c->offsetHz) < tol) locked = row.locked;
        for (const auto& s : r.svc) if (s.svc.name == c->name && std::fabs(s.offsetHz - c->offsetHz) < tol) svc = true;
        CHECK(locked, "%s: %+.2f MHz not locked", what, c->offsetHz / 1e6);
        CHECK(svc, "%s: service \"%s\" missing", what, c->name.c_str());
    }
}

static bool want(int argc, char** argv, const char* k) { return argc < 2 || !strcmp(argv[1], k); }

int main(int argc, char** argv) {
    if (want(argc, argv, "dvbc")) {   // two adjacent 8 MHz cable channels at 20 Msps: 6.952 MBd 64-QAM and 6.875 MBd 256-QAM
        const double rate = 20e6;
        std::vector<std::unique_ptr<Chan>> ch;
        const double rs[2] = {6.952e6, 6.875e6}, off[2] = {-4e6, 4e6};
        const int qam[2] = {0, 4};
        const char* names[2] = {"Cable One", "Cable Two"};
        for (int i = 0; i < 2; i++) {
            auto c = std::make_unique<SynthChan>();
            SynthConfig sc; sc.snrDb = 200; sc.cfoHz = off[i]; sc.modeOpt[0] = qam[i]; sc.modeVal[0] = rs[i];
            c->s = makeDvbcSynth(sc, rate, dvbt::testTsSourceNamed(i + 1, 401 + i, names[i]));
            c->offsetHz = off[i]; c->name = names[i];
            ch.push_back(std::move(c));
        }
        Result r = run("DVB-C: two adjacent cable channels at 20 Msps (free frequency)", 42, 8, rate, ch, 3.0, 0.02);
        expectAll("DVB-C", r, ch, 0.5e6);
        for (const auto& row : r.rows) if (row.running) CHECK(std::fabs(row.rsHz - (row.offsetHz < 0 ? rs[0] : rs[1])) < 0.01 * rs[0], "DVB-C symbol rate %.4f MBd", row.rsHz / 1e6);
    }
    if (want(argc, argv, "isdbt")) {   // three 6 MHz ISDB-T channels at 20 Msps (raster on a channel)
        const double rate = 20e6;
        std::vector<std::unique_ptr<Chan>> ch;
        const char* names[3] = {"Isdb Uno", "Isdb Dos", "Isdb Tres"};
        for (int i = 0; i < 3; i++) {
            auto c = std::make_unique<IsdbtChan>();
            isdbt::Params p; p.mode = 3; p.guard = isdbt::kGi8; p.layer[0].segments = 13; p.layer[0].mod = isdbt::k16Qam; p.layer[0].rate = isdbt::kR23; p.layer[0].ti = 1;
            c->g.reset(new isdbt::Generator(p, isdbt::singleLayerSource(0, dvbt::testTsSourceNamed(i + 1, 501 + i, names[i])), i + 1));
            c->rs.configure(isdbt::kSampleRate, rate);
            c->rate = rate; c->offsetHz = (i - 1) * 6e6; c->name = names[i];
            ch.push_back(std::move(c));
        }
        Result r = run("ISDB-T: three 6 MHz channels at 20 Msps (raster)", 6, 6, rate, ch, 3.0, 0.03);
        expectAll("ISDB-T", r, ch, 1e6);
    }
    if (want(argc, argv, "atsc")) {   // three 6 MHz ATSC channels at 20 Msps
        const double rate = 20e6;
        std::vector<std::unique_ptr<Chan>> ch;
        const char* names[3] = {"Atsc One", "Atsc Two", "Atsc Three"};
        for (int i = 0; i < 3; i++) {
            auto c = std::make_unique<AtscChan>();
            atsc::ChannelConfig cc; cc.snrDb = 99; cc.cfoHz = (i - 1) * 6e6;
            c->g.reset(new atsc::Generator(dvbt::testTsSourceNamed(i + 1, 601 + i, names[i]), cc, rate, i + 1));
            c->offsetHz = (i - 1) * 6e6; c->name = names[i];
            ch.push_back(std::move(c));
        }
        Result r = run("ATSC: three 6 MHz channels at 20 Msps (raster)", 3, 6, rate, ch, 3.0, 0.02);
        expectAll("ATSC", r, ch, 1e6);
    }
    if (want(argc, argv, "dtmb")) {   // two 8 MHz DTMB channels at 20 Msps (the capture between them)
        const double rate = 20e6;
        std::vector<std::unique_ptr<Chan>> ch;
        const char* names[2] = {"Dtmb One", "Dtmb Two"};
        for (int i = 0; i < 2; i++) {
            auto c = std::make_unique<SynthChan>();
            SynthConfig sc; sc.snrDb = 200; sc.cfoHz = i ? 4e6 : -4e6;
            c->s = makeDtmbSynth(sc, rate, dvbt::testTsSourceNamed(i + 1, 701 + i, names[i]));
            c->offsetHz = sc.cfoHz; c->name = names[i];
            if (!c->s) { CHECK(false, "no DTMB generator"); break; }
            ch.push_back(std::move(c));
        }
        Result r = run("DTMB: two 8 MHz channels at 20 Msps (raster)", 9, 8, rate, ch, 3.0, 0.02);
        expectAll("DTMB", r, ch, 1e6);
    }
    return testSummary("spantv_tv");
}
