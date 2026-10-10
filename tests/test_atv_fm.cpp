// Analog TV, FM video (analog FPV links): the generator's composite video is frequency modulated, as a video transmitter does, and goes through the
// receiver in FM mode. What the synthetic signal cannot say is which end of the swing real transmitters call the sync tip, the deviation and the
// pre-emphasis: the receiver finds the polarity itself, the rest does not matter to it; a real recording is the check for those.
#include "dect2/atv_testkit.h"
#include "dect2/exact_resampler.h"
#include <cmath>
#include <cstdio>
#include <random>

using namespace dect2;
using namespace dect2::atvkit;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

namespace {

struct FmOptions {
    double rate = 20e6;
    bool syncLow = true;        // the sync tip is the lowest frequency
    double cnrDb = 30;          // carrier to noise in 17 MHz
    double cfoHz = 1.5e6;       // the transmitter is not on the tuned frequency
    double devHz = 8e6;         // the sync tip to white swing is +-devHz
    int pattern = 0;            // the test card (its colour bars are what the picture is judged on)
    double acTauMs = 0;         // > 0: the transmitter is AC coupled (a high-pass with this time constant before the modulator): the baseline follows the
                                // picture, and the long broad pulses of the field sync shift it (what analog FPV video transmitters do)
};

// the generator's composite video (sync -0.43, blanking 0, white 1) at the radio's rate, as a frequency modulated carrier, 8 bits
std::vector<cf32> fmSignal(double secs, const FmOptions& o) {
    AtvGenConfig c; c.sys = kAtvG; c.colour = kAtvPal; c.pattern = o.pattern; c.cnrDb = 100; c.rate = AtvGenerator::kInternalRate;
    AtvGenerator g(c);
    std::vector<float> comp;
    g.setCompositeTap([&](const float* v, size_t n) { comp.insert(comp.end(), v, v + n); });
    std::vector<cf32> sink((size_t)(secs * AtvGenerator::kInternalRate));
    g.generate(sink.data(), sink.size());
    if (o.acTauMs > 0) {   // single-pole high-pass at the composite rate
        const double a = std::exp(-1.0 / (o.acTauMs * 1e-3 * AtvGenerator::kInternalRate));
        double y = 0, xp = comp.empty() ? 0.0 : comp[0];
        for (float& c : comp) { const double x = c; y = a * (y + x - xp); xp = x; c = (float)y; }
    }
    std::vector<cf32> in(comp.size()), v;
    for (size_t i = 0; i < comp.size(); i++) in[i] = cf32(comp[i], 0.f);
    ExactResampler rs;
    rs.configure(AtvGenerator::kInternalRate, o.rate);
    rs.process(in.data(), in.size(), v);
    std::mt19937 rng(7);
    std::normal_distribution<float> nd(0.f, 1.f);
    const double amp = 0.3, nPow = amp * amp / std::pow(10.0, o.cnrDb / 10) * (o.rate / 17e6);
    const float ns = (float)std::sqrt(nPow / 2);
    std::vector<cf32> out(v.size());
    double ph = 0;
    const double sgn = o.syncLow ? 1.0 : -1.0;
    for (size_t i = 0; i < v.size(); i++) {
        const double f = o.cfoHz + sgn * o.devHz * (v[i].real() - 0.285) / 0.715;
        ph += 2 * M_PI * f / o.rate;
        if (ph > M_PI) ph -= 2 * M_PI; else if (ph < -M_PI) ph += 2 * M_PI;
        auto q = [](float x) { return std::round(std::min(127.f, std::max(-128.f, x * 128.f))) / 128.f; };
        out[i] = cf32(q((float)(amp * std::cos(ph)) + ns * nd(rng)), q((float)(amp * std::sin(ph)) + ns * nd(rng)));
    }
    return out;
}

struct Result {
    AtvTelemetry tel;
    std::shared_ptr<const AtvFrame> frame;
    std::vector<std::string> log;
    int lockedAfterMs = -1;
    double cpuPct = 0;          // CPU time of feed() against the seconds of signal
};

Result decode(const std::vector<cf32>& x, double rate) {
    Result r;
    AtvReceiver rx;
    rx.setSilent(true);
    rx.setModulation(1);
    rx.setLogCallback([&](const std::string& m) { r.log.push_back(m); });
    rx.configure(rate);
    uint64_t seq = 0, fseq = 0;
    double cpu = 0;
    const size_t chunk = 65536;
    for (size_t done = 0; done < x.size();) {
        const size_t n = std::min(chunk, x.size() - done);
        const std::clock_t c0 = std::clock();
        rx.feed(x.data() + done, n);
        cpu += (double)(std::clock() - c0) / CLOCKS_PER_SEC;
        done += n;
        AtvTelemetry t;
        if (rx.telemetry(t, seq)) { seq = t.seq; r.tel = t; if (t.state == 2 && r.lockedAfterMs < 0) r.lockedAfterMs = (int)(1000.0 * (double)done / rate); }
        while (auto f = rx.frame(fseq)) r.frame = f;
    }
    r.cpuPct = 100.0 * cpu / ((double)x.size() / rate);
    return r;
}

// the colour bars of the decoded picture against the EBU bars
double barError(const AtvFrame& f) {
    AtvFormat fmt; atvMakeFormat(kAtvG, kAtvPal, fmt);
    AtvCard card(fmt);
    double c[8][3];
    barColours(f, card, c);
    double worst = 0;
    for (int i = 0; i < 8; i++) {
        float rgb[3]; atvEbuBar(i, rgb);
        for (int k = 0; k < 3; k++) worst = std::max(worst, std::fabs(c[i][k] - rgb[k]));
    }
    return worst;
}

void check(const char* name, const FmOptions& o, double secs, int maxLockMs, double maxBarErr) {
    const std::vector<cf32> x = fmSignal(secs, o);
    const Result r = decode(x, o.rate);
    const AtvTelemetry& t = r.tel;
    printf("%s: state %d, %d lines, %s, locked after %d ms, sync tip %s, burst %.2f, sync depth %.1f %%, video SNR %.1f dB\n", name, t.state, t.lines, t.colourSystem.c_str(), r.lockedAfterMs, t.fmSyncLow ? "lowest" : "highest", t.burstLevel, t.syncDepthPct, t.snrDb);
    for (const auto& l : r.log) printf("  log: %s\n", l.c_str());
    printf("  receiver: %.0f %% of real time\n", r.cpuPct);
    CHECK(r.cpuPct < 85, "%s: %.0f %% of real time", name, r.cpuPct);
    CHECK(t.modulation == 1, "%s: not in FM mode", name);
    CHECK(t.state == 2 && t.dataValid, "%s: not locked (state %d)", name, t.state);
    CHECK(r.lockedAfterMs >= 0 && r.lockedAfterMs <= maxLockMs, "%s: locked after %d ms (at most %d)", name, r.lockedAfterMs, maxLockMs);
    CHECK(t.lines == 625 && t.colourSystem == "PAL" && t.colour, "%s: %d lines, colour '%s'", name, t.lines, t.colourSystem.c_str());
    CHECK(t.fmSyncLow == o.syncLow, "%s: the polarity found is the wrong one", name);
    CHECK(t.blocksBad <= 4, "%s: damaged fields %llu", name, (unsigned long long)t.blocksBad);
    CHECK(r.frame && r.frame->width == 768 && r.frame->height == 576, "%s: picture size", name);
    if (r.frame) {
        const double e = barError(*r.frame);
        printf("  colour bars: worst error %.3f\n", e);
        CHECK(e < maxBarErr, "%s: colour bars off by %.3f", name, e);
    }
}

// A recording of a real analog FPV link (cs8, 20 Msps), when ATV_FM_REAL names one: AC coupled, a sync of an eighth of the swing, and a baseline
// that creeps by twice that over the lines after every field sync. Before the levels followed the baseline of a missed line the receiver lost the
// line sync every few fields (152 times in 10 s) and never held a picture.
void realRecording(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) { printf("FAIL: cannot open %s\n", path); fails++; return; }
    AtvReceiver rx;
    rx.setSilent(true);
    rx.setModulation(1);
    int lost = 0;
    rx.setLogCallback([&](const std::string& m) { if (m.find("line sync lost") != std::string::npos) lost++; });
    rx.configure(20e6);
    std::vector<int8_t> raw(2 * 65536);
    std::vector<cf32> buf(65536);
    AtvTelemetry t;
    uint64_t seq = 0, total = 0;
    for (size_t got; (got = fread(raw.data(), 2, 65536, f)) > 0;) {
        for (size_t i = 0; i < got; i++) buf[i] = cf32(raw[2 * i] / 128.f, raw[2 * i + 1] / 128.f);
        rx.feed(buf.data(), got);
        total += got;
        AtvTelemetry n;
        if (rx.telemetry(n, seq)) { seq = n.seq; t = n; }
    }
    fclose(f);
    const double secs = (double)total / 20e6;
    printf("real FPV recording %s: %.1f s, state %d, %d lines, %s, fields ok %llu bad %llu, line sync lost %d times\n", path, secs, t.state, t.lines, t.colourSystem.c_str(),
           (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad, lost);
    CHECK(t.state == 2 && t.lines == 625, "the real recording is not locked at the end (state %d, %d lines)", t.state, t.lines);
    CHECK(t.blocksOk > 40 * secs * 0.7, "only %llu fields decoded in %.1f s", (unsigned long long)t.blocksOk, secs);
    CHECK(t.blocksBad <= 5, "damaged fields %llu", (unsigned long long)t.blocksBad);
    CHECK(lost <= 2, "the line sync was lost %d times", lost);
}

}   // namespace

int main() {
    // the usual polarity: sync tip lowest, a comfortable C/N
    check("sync lowest, 30 dB", FmOptions(), 3.0, 1500, 0.35);
    // the other polarity: found after one failed trial
    { FmOptions o; o.syncLow = false; check("sync highest, 30 dB", o, 4.5, 3500, 0.35); }
    // a weaker signal, another deviation and a larger offset from the tuned frequency
    { FmOptions o; o.cnrDb = 22; o.devHz = 6e6; o.cfoHz = -2.5e6; check("22 dB, +-6 MHz, -2.5 MHz off", o, 3.0, 2000, 0.45); }
    // an AC coupled transmitter: the baseline follows the picture and is shifted by the long pulses of the field sync. (A real transmitter's sync is
    // an eighth of the swing, not 30 %, and what the slicer has to follow is then much worse than here: the real recording below is the test
    // of that; this one only checks that such a signal decodes.)
    { FmOptions o; o.acTauMs = 2; check("AC coupled, 2 ms", o, 4.0, 2500, 0.45); }
    // only noise: no picture, and no trouble
    {
        std::mt19937 rng(3); std::normal_distribution<float> nd(0.f, 0.15f);
        std::vector<cf32> x((size_t)(20e6 * 2));
        for (auto& s : x) s = cf32(nd(rng), nd(rng));
        const Result r = decode(x, 20e6);
        CHECK(r.tel.state != 2 && !r.tel.dataValid, "noise decoded as a picture (state %d)", r.tel.state);
    }
    if (const char* real = getenv("ATV_FM_REAL")) realRecording(real);
    printf(fails ? "atv_fm: %d FAILED\n" : "atv_fm: ok\n", fails);
    return fails ? 1 : 0;
}
