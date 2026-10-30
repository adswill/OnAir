// Transmit signal quality of the modes acars .. iono (the first half of the TX registry by id): every generator runs into memory with the session's
// own output stage and limiter, and its IQ is measured: level and clipping, DC, the 99 % occupied band, the leakage into the channels beside it
// (ACLR), the worst spur outside, the key-up fade, silent stretches, and that the samples do not depend on the block size. The limits sit a few dB
// from what the generators make today, so a change that splatters, clips, adds a noise floor or breaks the timing fails here.
// With --rt it also plays a few of them through OnAir's own receiver (the engine) with added noise and a carrier offset. NOTHING is transmitted.
#include "dect2/dsp/fftutil.h"
#include "dect2/dsp/gen_util.h"
#include "dect2/engine/engine.h"
#include "dect2/engine/mode_tuning.h"
#include "dect2/tx/tx.h"
#include "../common/cpu_time.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>
#ifndef _WIN32
#include <unistd.h>
#endif
using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

namespace {

constexpr double kNone = 999;   // not measured (the band beside the channel is outside the span) / not limited

struct Metrics {
    double xRealtime = 0, peak = 0, rms = 0, overKneePct = 0, dcDb = -300, obwHz = 0, aclrDb = kNone, spurDb = kNone, start05Db = -300, first = 0, gapMs = 0, blockDiff = -1;
    size_t nonFinite = 0;
};

// the request every mode is measured with: a valid callsign, grid, text and name (empty fields send the mode's demo instead)
TxRequest request(const char* id, bool demo) {
    TxRequest r;
    r.modeId = id;
    if (!demo) { r.callsign = "N0ABC"; r.grid = "KO24"; r.text = "CQ TEST"; r.name = "TEST"; }
    return r;
}

double cpuSeconds() { return processCpuSecs(); }

// samples as the session makes them: generator, shared output stage (fade-in), limiter
bool make(const TxModeSpec& m, const TxRequest& r, double rate, size_t n, size_t block, std::vector<cf32>& x, double* genCpu, std::vector<cf32>* preLimit) {
    std::string err;
    auto g = makeTxGenerator(m, r, rate, err);
    if (!g) { printf("  %s: no generator: %s\n", m.id, err.c_str()); return false; }
    x.assign(n, cf32(0.f, 0.f));
    double t = 0;
    size_t made = 0;
    while (made < n) {
        const size_t k = std::min(block, n - made);
        const double a = cpuSeconds();
        g->generate(x.data() + made, k);
        t += cpuSeconds() - a;
        txOutputStage(r, x.data() + made, k, made, rate);
        made += k;
        if (g->finished()) break;
    }
    x.resize(made);
    if (preLimit) *preLimit = x;
    txSoftLimit(x.data(), x.size());
    if (genCpu) *genCpu = t;
    return true;
}

Metrics measure(const TxModeSpec& m, const TxRequest& r, double rate, double secs) {
    Metrics k;
    std::vector<cf32> x, pre;
    double t = 0;
    if (!make(m, r, rate, std::max<size_t>(1 << 17, (size_t)(secs * rate)), 1 << 15, x, &t, &pre)) return k;
    const double N = (double)pre.size();
    k.xRealtime = N / rate / std::max(t, 1e-6);
    double pk = 0, acc = 0;
    std::complex<double> mean = 0;
    size_t over = 0;
    for (auto& v : pre) {
        if (!std::isfinite(v.real()) || !std::isfinite(v.imag())) { k.nonFinite++; continue; }
        const double m2 = std::norm(v);
        pk = std::max(pk, m2); acc += m2;
        if (m2 > 0.81) over++;
        mean += std::complex<double>(v.real(), v.imag());
    }
    k.peak = std::sqrt(pk);
    k.rms = std::sqrt(acc / N);
    k.overKneePct = (double)over / N * 100;
    k.dcDb = 20 * std::log10(std::abs(mean / N) / std::max(k.rms, 1e-12) + 1e-15);
    if (k.rms < 1e-9) return k;
    // Welch power spectrum (Hann, half overlap) of what the radio gets; finer bins until the occupied band spans 40 of them
    int nfft = 4096, l2 = 12, a = 0, b = 0, bw = 0;
    std::vector<double> psd;
    double tot = 0;
    for (;;) {
        psd.assign((size_t)nfft, 0.0);
        std::vector<float> re((size_t)nfft), im((size_t)nfft), w((size_t)nfft);
        for (int i = 0; i < nfft; i++) w[(size_t)i] = 0.5f - 0.5f * (float)std::cos(2 * M_PI * i / nfft);
        for (size_t s = 0; s + (size_t)nfft <= x.size(); s += (size_t)nfft / 2) {
            for (int i = 0; i < nfft; i++) { re[(size_t)i] = x[s + (size_t)i].real() * w[(size_t)i]; im[(size_t)i] = x[s + (size_t)i].imag() * w[(size_t)i]; }
            fftSplit(re.data(), im.data(), l2, false);
            for (int i = 0; i < nfft; i++) psd[(size_t)((i + nfft / 2) % nfft)] += (double)re[(size_t)i] * re[(size_t)i] + (double)im[(size_t)i] * im[(size_t)i];
        }
        tot = 0;
        for (double p : psd) tot += p;
        double lo = 0, hi = 0;
        a = 0; b = nfft - 1;
        while (a < b && lo + psd[(size_t)a] <= tot * 0.005) lo += psd[(size_t)a++];
        while (b > a && hi + psd[(size_t)b] <= tot * 0.005) hi += psd[(size_t)b--];
        bw = b - a + 1;
        if (bw >= 40 || (size_t)nfft * 16 > x.size() || nfft >= (1 << 18)) break;
        nfft *= 4; l2 += 2;
    }
    k.obwHz = bw * rate / nfft;
    auto band = [&](int s, int e) { double p = 0; int c = 0; for (int i = std::max(0, s); i <= std::min(nfft - 1, e); i++) { p += psd[(size_t)i]; c++; } return std::make_pair(p, c); };
    const auto ch = band(a, b);
    const int guard = std::max(2, bw / 4);   // the channels beside start a quarter of the occupied width beyond its edges
    auto aclr = [&](std::pair<double, int> adj) { return adj.second < bw / 2 || adj.second < 4 ? kNone : 10 * std::log10(ch.first / std::max(adj.first * bw / adj.second, 1e-30)); };
    k.aclrDb = std::min(aclr(band(a - guard - bw, a - guard - 1)), aclr(band(b + guard + 1, b + guard + bw)));
    double inPk = 1e-30, sp = 1e-30;
    for (int i = a; i <= b; i++) inPk = std::max(inPk, psd[(size_t)i]);
    const int ca = a - bw / 2 - guard, cb = b + bw / 2 + guard;
    for (int i = 0; i < nfft; i++) if (i < ca || i > cb) sp = std::max(sp, psd[(size_t)i]);
    k.spurDb = ca <= 0 && cb >= nfft - 1 ? kNone : 10 * std::log10(sp / inPk);
    const size_t ramp = std::max<size_t>(4, (size_t)(0.0005 * rate));
    double r0 = 0;
    for (size_t i = 0; i < ramp && i < x.size(); i++) r0 += std::norm(x[i]);
    k.start05Db = 10 * std::log10(r0 / (double)ramp / (acc / N) + 1e-30);
    k.first = std::abs(x[0]) / k.rms;
    size_t run = 0, longest = 0;
    bool started = false;
    for (auto& v : x) { if (std::norm(v) < 1e-12) { if (started) run++; } else { started = true; longest = std::max(longest, run); run = 0; } }
    k.gapMs = (double)longest / rate * 1e3;
    // the same request in blocks of 4093 samples: the first half second must not change
    std::vector<cf32> y;
    const size_t nc = std::min<size_t>(x.size(), (size_t)(0.5 * rate));
    if (make(m, r, rate, nc, 4093, y, nullptr, nullptr)) {
        k.blockDiff = 0;
        for (size_t i = 0; i < nc && i < y.size(); i++) k.blockDiff = std::max(k.blockDiff, (double)std::abs(y[i] - x[i]));
    }
    return k;
}

// per mode: the most it may clip and peak, the worst spur and the least ACLR (kNone: not limited), the most DC, and whether the
// samples are the same whatever the block size (false where the signal follows the clock: DAB time, DMR, FLARM, e-Czas, FT slots)
struct Limit { const char* id; double maxPeak, maxOverPct, maxSpurDb, minAclrDb, maxDcDb; bool blockExact; };
#include "test_tx_quality_a_limits.inc"

const Limit* limitFor(const std::string& id) {
    for (const Limit& l : kLimits) if (id == l.id) return &l;
    return nullptr;
}

void qualityChecks(bool demo) {
    printf("%s:\n", demo ? "demo signals (empty fields)" : "signals with a callsign, text and name");
    printf("  %-11s %8s %7s %8s %7s %9s %7s %7s %7s %7s\n", "id", "x real", "peak", "clip %", "DC dB", "OBW kHz", "ACLR", "spur", "fade", "block");
    for (const Limit& l : kLimits) {
        const TxModeSpec* m = txModeById(l.id);
        CHECK(m, "%s is in the TX registry", l.id);
        if (!m) continue;
        TxRequest r = request(l.id, demo);
        const bool ftx = txPayloadKind(l.id) == TxPayloadKind::Ftx;
        double rate = txRequestRate(*m, r, {});
        double secs = 1.5;
        if (ftx) { rate = std::max(250e3, m->minRateHz); secs = 16; }   // a whole slot, at a quarter of the rate
        if (!strcmp(l.id, "goesdcs")) secs = 4;   // its first message starts after 1.5 s
        const Metrics k = measure(*m, r, rate, secs);
        printf("  %-11s %8.1f %7.3f %8.4f %7.1f %9.1f %7.1f %7.1f %7.1f %7.2g\n", l.id, k.xRealtime, k.peak, k.overKneePct, k.dcDb, k.obwHz / 1e3, k.aclrDb, k.spurDb, k.start05Db, k.blockDiff);
        CHECK(k.nonFinite == 0, "%s: %zu samples are NaN or infinite", l.id, k.nonFinite);
        CHECK(k.rms > 1e-6, "%s: the generator made silence", l.id);
        if (k.rms <= 1e-6) continue;
        CHECK(k.peak <= l.maxPeak, "%s: peak %.3f before the limiter (limit %.3f): the level is wrong or the signal clips", l.id, k.peak, l.maxPeak);
        CHECK(k.overKneePct <= l.maxOverPct, "%s: %.4f %% of the samples reach the limiter's knee (limit %.4f %%)", l.id, k.overKneePct, l.maxOverPct);
        CHECK(k.dcDb <= l.maxDcDb, "%s: DC %.1f dB below the rms (limit %.1f dB)", l.id, k.dcDb, l.maxDcDb);
        if (l.maxSpurDb < kNone && k.spurDb < kNone) CHECK(k.spurDb <= l.maxSpurDb, "%s: worst spur outside the channel %.1f dB (limit %.1f dB)", l.id, k.spurDb, l.maxSpurDb);
        if (l.minAclrDb < kNone && k.aclrDb < kNone) CHECK(k.aclrDb >= l.minAclrDb, "%s: adjacent-channel leakage ratio %.1f dB (at least %.1f dB)", l.id, k.aclrDb, l.minAclrDb);
        CHECK(k.first == 0, "%s: the first sample is %.2f x the rms: the key-up is not faded in", l.id, k.first);
        CHECK(k.start05Db < -10, "%s: the first 0.5 ms are at %.1f dB of the rms: the key-up fade is missing", l.id, k.start05Db);
        if (l.blockExact && !ftx) CHECK(k.blockDiff >= 0 && k.blockDiff < 0.01, "%s: blocks of 4093 samples give other samples (max difference %.3g): a seam at block edges", l.id, k.blockDiff);
        CHECK(k.xRealtime > 0.25, "%s: the generator makes %.2f s of signal per CPU second: far from real time", l.id, k.xRealtime);
    }
}

// one pass of a session: the last samples fade out (no key-down click)
struct MemSink : TxSink {
    std::vector<cf32>* out;
    double rate = 0;
    explicit MemSink(std::vector<cf32>* o) : out(o) {}
    bool open(const TxTune& t, std::string&) override { rate = t.sampleRate; return true; }
    bool write(const cf32* x, size_t n, std::string&) override { out->insert(out->end(), x, x + n); return true; }
    bool retune(const TxTune&, std::string&) override { return true; }
    void close() override {}
    double sampleRate() const override { return rate; }
    bool realtime() const override { return false; }   // a recording: the session waits for samples, no underrun zeros
    std::string name() const override { return "memory"; }
};

void fadeChecks() {
    printf("key-up and key-down fades of a one-pass session:\n");
    for (const char* id : {"airband", "drm", "fm", "cell"}) {
        std::vector<cf32> got;
        TxSession s;
        TxRequest r = request(id, false);
        r.loop = false; r.onceSeconds = 0.3;
        std::string err;
        CHECK(s.start(std::make_unique<MemSink>(&got), r, {}, err), "%s: start: %s", id, err.c_str());
        for (int i = 0; i < 2000 && s.running(); i++) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        s.stop();
        CHECK(got.size() > 1000, "%s: %zu samples", id, got.size());
        if (got.size() <= 1000) continue;
        const float first = std::abs(got.front()), last = std::abs(got.back());
        double pk = 0;
        for (auto& v : got) pk = std::max(pk, (double)std::abs(v));
        printf("  %-10s first %.4f last %.4f peak %.3f\n", id, first, last, pk);
        CHECK(first < 0.01 * pk && last < 0.01 * pk, "%s: the transmission starts at %.4f and ends at %.4f of its peak %.3f (no fade)", id, first, last, pk);
    }
}

void ftxValidation() {
    std::string err;
    TxRequest r = request("ft8", false);
    r.callsign = "N0CALL";   // four letters after the digit: not a standard FT8 call
    auto g = makeTxGenerator(*txModeById("ft8"), r, 250e3, err);
    CHECK(!g && err.find("cannot be sent") != std::string::npos, "FT8 with an unpackable call is refused with a reason (got '%s')", err.c_str());
    r.callsign = "N0ABC";
    g = makeTxGenerator(*txModeById("ft8"), r, 250e3, err);
    CHECK(g != nullptr, "FT8 with a valid call: %s", err.c_str());
}

// the round trip: the transmitted samples, with noise in the occupied band and a carrier offset, through the engine; the receiver must lock
bool roundTrip(const char* id, double snrDb, double cfoHz, double secs) {
    const TxModeSpec* m = txModeById(id);
    if (!m) return false;
    TxRequest r = request(id, false);
    const double rate = txRequestRate(*m, r, {});
    std::vector<cf32> x;
    if (!make(*m, r, rate, (size_t)(secs * rate), 1 << 15, x, nullptr, nullptr)) return false;
    double p = 0;
    size_t on = 0;
    for (auto& v : x) { const double q = std::norm(v); if (q > 1e-9) { p += q; on++; } }
    p /= (double)std::max<size_t>(1, on);
    const Metrics k = measure(*m, r, rate, std::min(secs, 1.5));
    const double sigma = std::sqrt(p * rate / std::max(k.obwHz, 100.0) / std::pow(10.0, snrDb / 10) / 2);
    double ph = 0;
    const double w = 2 * M_PI * cfoHz / rate;
    for (auto& v : x) { v *= std::polar(1.f, (float)ph); ph += w; if (ph > M_PI) ph -= 2 * M_PI; }
    genutil::NoiseSource ns(7);
    ns.add(x.data(), x.size(), (float)sigma);
    char path[256];
#ifdef _WIN32
    snprintf(path, sizeof path, "onair_txq_%s.cf32", id);
#else
    snprintf(path, sizeof path, "/tmp/onair_txq_%s_%d.cf32", id, (int)getpid());
#endif
    FILE* f = fopen(path, "wb");
    if (!f) return false;
    fwrite(x.data(), sizeof(cf32), x.size(), f);
    fclose(f);
    Engine e;
    DeviceInfo dev; dev.kind = DeviceInfo::File; dev.name = "tx recording";
    const ModeTuning* mt = modeTuning(m->stdMode);
    TuneSettings t;
    t.sampleRate = rate; t.centerHz = 100e6;
    t.bandwidthMhz = mt ? mt->bandwidthMhz : 8;
    FileOptions fo; fo.path = path; fo.format = FileFormat::CF32; fo.sampleRate = rate; fo.loop = true;
    e.setStandard(m->stdMode);
    bool locked = false;
    if (e.start(dev, t, fo)) {
        RxTelemetry rx;
        uint64_t seq = 0;
        const auto t0 = std::chrono::steady_clock::now();
        while (!locked && std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < 60) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            if (e.latestRx(rx, seq)) { seq = rx.seq; locked = rx.state == 2 && rx.dataValid; }
        }
        e.stop();
    }
    remove(path);
    printf("  %-10s SNR %4.0f dB in %.1f kHz, offset %+5.0f Hz: %s\n", id, snrDb, k.obwHz / 1e3, cfoHz, locked ? "locked" : "NOT locked");
    return locked;
}

void roundTrips() {
    printf("round trips through the receiver (noise in the occupied band, carrier offset):\n");
    struct Rt { const char* id; double snr, cfo, secs; };
    const Rt list[] = {
        {"drm", 25, 0, 6}, {"drm", 15, 40, 6},
        {"contestia", 20, 0, 12}, {"contestia", 10, 25, 12},
        {"dominoex", 20, 0, 8}, {"dominoex", 12, 15, 8},
        {"fsq", 20, 0, 12}, {"ifkp", 20, 0, 12},
        {"ale", 20, 0, 6}, {"ale", 12, 30, 6},
        {"ble", 25, 0, 1}, {"ble", 15, 20000, 1},
        {"cubesat", 25, 0, 4}, {"datawell", 25, 0, 4},
        {"cw", 20, 0, 6}, {"cw", 10, 30, 6},
    };
    for (const Rt& t : list) CHECK(roundTrip(t.id, t.snr, t.cfo, t.secs), "%s did not decode at %.0f dB, %+.0f Hz", t.id, t.snr, t.cfo);
}

} // namespace

int main(int argc, char** argv) {
    bool rt = false;
    for (int i = 1; i < argc; i++) if (!strcmp(argv[i], "--rt")) rt = true;
    if (rt) roundTrips();
    else {
        ftxValidation();
        fadeChecks();
        qualityChecks(false);
        qualityChecks(true);
    }
    printf(fails ? "%d failure(s)\n" : "all passed\n", fails);
    return fails ? 1 : 0;
}
