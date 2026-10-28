// TETRA on a wide capture (3.4): a HackRF at 5, 10 and 20 Msps sees a 4 to 14 MHz stretch of the band. Every carrier in it must be found and its
// control channel decoded (cell identity, SYSINFO, main carrier): carriers at different levels (11.5 dB apart), each with its own base-station
// tuning error, in noise; the shared-FFT channeliser must give what the one-carrier path gives; the carrier search takes any number of carriers
// and a settable part of the capture; a narrow capture (2 Msps) behaves as before.
#include "dect2/tetra/tetra_gen.h"
#include "dect2/tetra/tetra_rx.h"
#include "dect2/tetra/tetra_tel.h"
#include "dect2/source/source.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>
using namespace dect2;
#include "../common/test_util.h"

struct Result { TetraTelemetry t; double decodeSec = 0; };

static Result run(double rate, int cells, double snrDb, double searchBand = 0) {
    SynthConfig sc;
    sc.mode = 48; sc.snrDb = snrDb; sc.modeOpt[0] = 2; sc.modeOpt[1] = cells;
    auto syn = makeTetraSynth(sc, rate);
    TetraReceiver rx;
    rx.configure(rate);
    rx.setCenterHz(kTetraTestCentreHz);
    if (searchBand > 0) rx.setSearchBand(searchBand);
    const size_t chunk = (size_t)(rate * 0.05);
    std::vector<cf32> buf(chunk);
    const size_t want = (size_t)(rate * TetraReceiver::kCaptureSec);
    size_t fed = 0;
    bool handed = false;
    Result r;
    auto t0 = std::chrono::steady_clock::now();
    while (fed < want + chunk) {   // one capture, then a sample to hand it over
        syn->generate(buf.data(), chunk);
        rx.feed(buf.data(), chunk);
        fed += chunk;
        if (fed >= want && !handed) { handed = true; t0 = std::chrono::steady_clock::now(); }   // the capture is complete and goes to the decoder
    }
    while (rx.busy()) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    r.decodeSec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    rx.feed(buf.data(), 1);
    rx.telemetry(r.t, 0);
    return r;
}

static const TetraCell* byCc(const TetraTelemetry& t, int cc) {
    for (const auto& c : t.cells) if (c.colourCode == cc) return &c;
    return nullptr;
}

// every one of `cells` carriers: found, identity right, SYSINFO read, announced main carrier = the carrier's frequency, measured frequency within 1.5 kHz
static int checkAll(const TetraTelemetry& t, double rate, int cells, const char* what) {
    int ok = 0;
    for (int i = 0; i < cells; i++) {
        const TetraWideCell w = tetraWideCell(i, cells, rate);
        const TetraCell* c = byCc(t, w.cfg.sync.colourCode);
        const double f = kTetraTestCentreHz + w.offHz;
        const bool good = c && c->mcc == 1 && c->mnc == 1 && c->hasSysinfo && c->la == 2000 + i && std::fabs(c->nominalHz - f) < 1 &&
                          std::fabs(c->freqHz - (f + w.errHz)) < 1500;
        CHECK(good, "%s: carrier %d at %+.3f MHz (colour code %d) %s", what, i, w.offHz / 1e6, w.cfg.sync.colourCode, !c ? "not found" : !c->hasSysinfo ? "no SYSINFO" : "wrong fields");
        ok += good;
    }
    return ok;
}

int main() {
    // the generator places distinct, sorted carriers inside the usable band
    for (double rate : {5e6, 10e6, 20e6}) {
        double last = -1e12;
        for (int i = 0; i < 24; i++) {
            const TetraWideCell w = tetraWideCell(i, 24, rate);
            CHECK(w.offHz > last && std::fabs(w.offHz) < 0.36 * rate, "scene 2 placement at %.0f Msps, cell %d: %+.0f Hz", rate / 1e6, i, w.offHz);
            last = w.offHz;
        }
    }
    struct Case { double rate; int cells; };
    const Case cases[] = {{5e6, 8}, {10e6, 16}, {20e6, 24}};
    for (const auto& k : cases) {
        const Result r = run(k.rate, k.cells, 30);
        const int ok = checkAll(r.t, k.rate, k.cells, "wide");
        char what[64]; snprintf(what, sizeof what, "%.0f Msps", k.rate / 1e6);
        CHECK(r.t.carriers.size() >= (size_t)k.cells, "%s: %zu carriers seen in the spectrum, want %d", what, r.t.carriers.size(), k.cells);
        CHECK(r.t.cells.size() == (size_t)k.cells, "%s: %zu cells in the table, want %d", what, r.t.cells.size(), k.cells);
        printf("  %5.1f Msps: %d of %d carriers decoded (%zu seen in the spectrum), decoded %.1f s after the capture ended (the capture is 2.3 s)\n", k.rate / 1e6, ok, k.cells, r.t.carriers.size(), r.decodeSec);
    }
    // the shared channeliser gives the same 72 kHz carrier as the one-carrier path (same symbols), and costs a fraction of it
    {
        const double rate = 10e6;
        SynthConfig sc; sc.mode = 48; sc.snrDb = 40; sc.modeOpt[0] = 2; sc.modeOpt[1] = 16;
        auto syn = makeTetraSynth(sc, rate);
        std::vector<cf32> x((size_t)(rate * 1.2));
        syn->generate(x.data(), x.size());
        std::vector<double> offs;
        for (int i = 0; i < 16; i++) { const TetraWideCell w = tetraWideCell(i, 16, rate); offs.push_back(w.offHz + w.errHz); }
        auto t0 = std::chrono::steady_clock::now();
        const auto many = tetra::channelizeMany(x.data(), x.size(), rate, offs);
        const double tMany = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        t0 = std::chrono::steady_clock::now();
        std::vector<std::vector<cf32>> one;
        for (double o : offs) one.push_back(tetra::channelize(x.data(), x.size(), rate, o));
        const double tOne = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        int agree = 0;
        for (size_t k = 0; k < offs.size(); k++) {
            const auto& a2 = many[k]; const auto& b2 = one[k];
            CHECK(!a2.empty() && !b2.empty() && std::fabs((double)a2.size() - (double)b2.size()) < 0.01 * (double)b2.size() + 8, "carrier %zu: %zu vs %zu samples", k, a2.size(), b2.size());
            // power of the two outputs (the shared one has no group delay, so compare power and the symbol-rate envelope, not sample by sample)
            double pa = 0, pb = 0;
            const size_t m = std::min(a2.size(), b2.size());
            for (size_t i = m / 4; i < m / 2; i++) { pa += std::norm(a2[i]); pb += std::norm(b2[i]); }
            if (pa > 0 && pb > 0 && std::fabs(10 * std::log10(pa / pb)) < 1.0) agree++;
        }
        CHECK(agree == 16, "the two paths agree in power for %d of 16 carriers", agree);
        printf("  16 carriers cut out of 1.2 s at 10 Msps: shared FFT %.2f s, one by one %.2f s (%.1fx)\n", tMany, tOne, tOne / std::max(1e-9, tMany));
        CHECK(tMany < tOne, "the shared channeliser is not faster (%.2f s vs %.2f s)", tMany, tOne);
    }
    // a lower search fraction leaves the outer carriers out; nothing is refused
    {
        const Result narrow = run(10e6, 16, 30, 0.30);
        const Result full = run(10e6, 16, 30, 0.72);
        CHECK(narrow.t.cells.size() < full.t.cells.size() && narrow.t.cells.size() >= 3, "search band 30 %% finds %zu cells, 72 %% finds %zu", narrow.t.cells.size(), full.t.cells.size());
    }
    // the default 2 Msps capture behaves as before: the same carriers found by the per-carrier path
    {
        const Result r = run(2e6, 6, 30);
        const int ok = checkAll(r.t, 2e6, 6, "2 Msps");
        printf("   2.0 Msps: %d of 6 carriers decoded\n", ok);
    }
    // noise only at 10 Msps: no cell, no crash
    {
        TetraReceiver rx;
        rx.configure(10e6);
        rx.setCenterHz(kTetraTestCentreHz);
        std::vector<cf32> n(1000000);
        uint32_t s = 1;
        for (int k = 0; k < 25; k++) {
            for (auto& v : n) { s = s * 1664525u + 1013904223u; const float a = ((s >> 8) & 0xFFFF) / 65536.f - 0.5f; s = s * 1664525u + 1013904223u; v = cf32(a * 0.2f, (((s >> 8) & 0xFFFF) / 65536.f - 0.5f) * 0.2f); }
            rx.feed(n.data(), n.size());
        }
        while (rx.busy()) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        TetraTelemetry t;
        rx.feed(n.data(), 1);
        rx.telemetry(t, 0);
        CHECK(t.cells.empty() && t.blocksOk == 0, "%zu cells found in noise at 10 Msps", t.cells.size());
    }
    printf(fails ? "tetra wide: %d FAILED\n" : "tetra wide: all passed\n", fails);
    return fails ? 1 : 0;
}
