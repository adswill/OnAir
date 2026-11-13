// Mode Builder "Signal" view: the plot data of Receiver::signalView() from generated signals of every modulation (cluster count and
// positions, thresholds, measured deviation and offset, PSK constellation and EVM), with a frequency offset and noise, and noise only:
// no clusters may be found in noise. Simulated signals only (the generator of the library); nothing here was checked on air.
#include "dect2/modebuilder/modebuilder.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

using namespace dect2::modebuilder;

static int fails = 0, checks = 0;
#define CHECKM(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d  %s  ", __FILE__, __LINE__, #c); printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint32_t rnd(uint32_t& s) { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }

static std::string mkDef(const std::string& mod, const std::string& more, const std::string& root = "") {
    return R"({"onair_mode":1,"name":"t","sample_rate":96000,"modulation":)" + mod + R"(,"sync":{"word":"0x2DD4E5A9","bits":32,"max_errors":2},)" +
           R"("frame":{"length":{"mode":"fixed","bytes":16}},"crc":{"preset":"CRC-16/CCITT-FALSE"})" + more + root + "}";
}
static Definition defOf(const std::string& json) {
    ParseResult r = parseDefinition(json);
    if (!r.ok) for (const Issue& i : r.issues) printf("  issue: %s\n", i.text().c_str());
    CHECKM(r.ok, "definition does not parse");
    return r.def;
}

struct Case { const char* name; std::string mod, more; };
static const Case kCases[] = {
    {"2FSK", R"({"type":"FSK","symbol_rate":4800,"deviation_hz":2400})", ""},
    {"4FSK", R"({"type":"FSK","order":4,"symbol_rate":4800,"deviation_hz":3600})", R"(,"preamble":{"pattern":"0x22","bits":32})"},
    {"GFSK", R"({"type":"GFSK","symbol_rate":9600,"deviation_hz":4800,"bt":0.5})", ""},
    {"GMSK", R"({"type":"GMSK","symbol_rate":4800})", ""},
    {"OOK", R"({"type":"OOK","symbol_rate":2000})", R"(,"preamble":{"pattern":"0xAAAA","bits":32})"},
    {"ASK4", R"({"type":"ASK","order":4,"symbol_rate":2400})", R"(,"preamble":{"pattern":"0x22","bits":32})"},
    {"BPSK", R"({"type":"PSK","order":2,"symbol_rate":4800})", ""},
    {"QPSK", R"({"type":"PSK","order":4,"symbol_rate":4800})", ""},
    {"8PSK", R"({"type":"PSK","order":8,"symbol_rate":2400})", R"(,"preamble":{"pattern":"0x1C7","bits":36})"},
};

static SignalView viewOf(const Definition& d, double snr, double cfo, int nFrames, uint32_t seed) {
    GenOptions g;
    g.snrDb = snr;
    g.cfoHz = cfo;
    g.seed = seed;
    uint32_t rng = seed * 7 + 1;
    std::vector<std::vector<uint8_t>> frames;
    for (int i = 0; i < nFrames; i++) frames.push_back(randomFrame(d, rng));
    const std::vector<cf> iq = generate(d, frames, g);
    Receiver r(d);
    r.configure(g.fs > 0 ? g.fs : d.sampleRate);
    for (size_t i = 0; i < iq.size(); i += 1000) r.feed(iq.data() + i, std::min<size_t>(1000, iq.size() - i));
    return r.signalView();
}

static bool isFsk(Mod m) { return m == Mod::FSK || m == Mod::GFSK || m == Mod::GMSK; }

static void testClusters() {
    for (const Case& c : kCases) {
        const Definition d = defOf(mkDef(c.mod, c.more));
        const int M = d.levels();
        for (double cfo : {0.0, 400.0}) {
            if (cfo != 0 && !isFsk(d.mod)) continue;   // the PSK loop and the envelope do not care; the FSK offset is measured below
            const SignalView v = viewOf(d, d.mod == Mod::ASK || M == 8 ? 32 : 28, cfo, 12, 5);
            CHECKM(v.have && v.samples >= 64, "%s cfo %.0f: no view (%llu symbols)", c.name, cfo, (unsigned long long)v.samples);
            CHECKM(v.resolved && v.clusters == M, "%s cfo %.0f: %d clusters, separation %.1f, want %d", c.name, cfo, v.clusters, v.separation, M);
            CHECKM(v.separation >= 4 && v.snrDb > 6, "%s: separation %.1f, SNR %.1f dB", c.name, v.separation, v.snrDb);
            if (d.mod == Mod::PSK) {
                CHECKM(v.points.size() >= 100 && v.evmPct > 0 && v.evmPct < 30, "%s: %zu points, EVM %.1f %%", c.name, v.points.size(), v.evmPct);
                CHECKM(v.hasEbn0 && std::fabs(v.ebn0Db - (v.snrDb - 10 * std::log10((double)d.bitsPerSymbol()))) < 1e-6, "%s: Eb/N0 %.2f", c.name, v.ebn0Db);
                int near = 0;
                for (const cf& p : v.points) {
                    double best = 1e9;
                    for (int k = 0; k < M; k++) best = std::min(best, (double)std::abs(p - cf((float)std::cos(2 * M_PI * k / M), (float)std::sin(2 * M_PI * k / M))));
                    near += best < 0.5;
                }
                CHECKM(near >= (int)v.points.size() * 95 / 100, "%s: %d of %zu points near an ideal point", c.name, near, v.points.size());
                continue;
            }
            CHECKM(v.hist.size() == 64 && v.histMax > v.histMin, "%s: histogram", c.name);
            float total = 0;
            for (float h : v.hist) total += h;
            CHECKM(total >= (float)v.samples * 0.98f, "%s: histogram holds %.0f of %llu", c.name, (double)total, (unsigned long long)v.samples);
            CHECKM((int)v.centres.size() == M && (int)v.measuredThresholds.size() == M - 1 && (int)v.thresholds.size() == M - 1, "%s: centres %zu thresholds %zu/%zu", c.name,
                   v.centres.size(), v.measuredThresholds.size(), v.thresholds.size());
            if ((int)v.centres.size() != M) continue;
            for (int k = 0; k + 1 < M; k++) {
                CHECKM(v.centres[(size_t)k] < v.centres[(size_t)k + 1], "%s: centres not ascending", c.name);
                CHECKM(v.measuredThresholds[(size_t)k] > v.centres[(size_t)k] && v.measuredThresholds[(size_t)k] < v.centres[(size_t)k + 1], "%s: threshold %d outside its clusters", c.name, k);
                // the slicer's own threshold must also fall between the two clusters it separates
                CHECKM(v.thresholds[(size_t)k] > v.centres[(size_t)k] && v.thresholds[(size_t)k] < v.centres[(size_t)k + 1], "%s: slicer threshold %d %.3f outside %.3f .. %.3f", c.name, k,
                       v.thresholds[(size_t)k], v.centres[(size_t)k], v.centres[(size_t)k + 1]);
            }
            if (isFsk(d.mod)) {
                const double dev = d.deviation();
                // Gaussian filtering pulls the symbol-time frequency in (GMSK: the matched filter as well): only plain FSK is exact
                const double lowT = d.mod == Mod::FSK ? 0.88 : d.mod == Mod::GFSK ? 0.75 : 0.5, highT = d.mod == Mod::FSK ? 1.12 : 1.15;
                CHECKM(v.deviationHz > lowT * dev && v.deviationHz < highT * dev, "%s cfo %.0f: deviation %.0f Hz, nominal %.0f", c.name, cfo, v.deviationHz, dev);
                CHECKM(std::fabs(v.offsetHz - cfo) < 0.15 * dev + 100, "%s: offset %.0f Hz, want %.0f", c.name, v.offsetHz, cfo);
                if (d.mod == Mod::FSK) {   // the tone grid: levels at -1, -1/3, 1/3, 1 deviations for 4FSK
                    for (int k = 0; k < M; k++) {
                        const double want = (-1 + 2.0 * k / (M - 1)) * dev + cfo;
                        CHECKM(std::fabs(v.centres[(size_t)k] - want) < 0.15 * dev, "%s: centre %d at %.0f Hz, want %.0f", c.name, k, v.centres[(size_t)k], want);
                    }
                }
            } else {
                CHECKM(v.centres[0] < v.centres[(size_t)M - 1] * 0.6, "%s: lowest level %.3f vs highest %.3f", c.name, v.centres[0], v.centres[(size_t)M - 1]);
            }
        }
    }
}

// the EVM must follow the noise: a QPSK signal at 30 dB is cleaner than at 14 dB, and the SNR estimate moves with it
static void testPskEvmFollowsNoise() {
    const Definition d = defOf(mkDef(R"({"type":"PSK","order":4,"symbol_rate":4800})", ""));
    const SignalView hi = viewOf(d, 30, 0, 12, 9), lo = viewOf(d, 14, 0, 12, 9);
    CHECKM(hi.have && lo.have, "QPSK views");
    CHECKM(lo.evmPct > hi.evmPct * 1.8, "EVM %.1f %% at 14 dB vs %.1f %% at 30 dB", lo.evmPct, hi.evmPct);
    const double dsnr = hi.snrDb - lo.snrDb;
    CHECKM(dsnr > 8 && dsnr < 24, "estimated SNR %.1f dB vs %.1f dB (generated 16 dB apart)", hi.snrDb, lo.snrDb);
}

// the clusters melt together as the noise grows: a view must not claim clusters that are not there
static void testLowSnrNotResolved() {
    for (const Case& c : kCases) {
        const Definition d = defOf(mkDef(c.mod, c.more));
        const SignalView hi = viewOf(d, 32, 0, 12, 3), lo = viewOf(d, -4, 0, 12, 3);
        CHECKM(hi.resolved, "%s at 32 dB resolved", c.name);
        CHECKM(!lo.have || !lo.resolved || lo.separation < hi.separation, "%s at -4 dB: resolved %d, separation %.1f (32 dB: %.1f)", c.name, lo.resolved, lo.separation, hi.separation);
    }
}

// noise only: no clusters, with the default squelch (no view at all) and with the squelch off (the demodulator sees noise all the time)
static void testNoiseOnly() {
    for (const Case& c : kCases) {
        for (int open = 0; open < 2; open++) {
            const Definition d = defOf(mkDef(c.mod, c.more, open ? R"(,"squelch_db":0)" : ""));
            for (uint32_t seed : {99u, 7u}) {
                std::vector<cf> iq((size_t)(d.sampleRate * 1.5));
                uint32_t s = seed;
                for (cf& x : iq) {
                    const double u1 = std::max(1e-12, (rnd(s) >> 8) / 16777216.0), u2 = (rnd(s) >> 8) / 16777216.0;
                    const double r = std::sqrt(-2 * std::log(u1)) * 0.1;
                    x = cf((float)(r * std::cos(6.2831853 * u2)), (float)(r * std::sin(6.2831853 * u2)));
                }
                Receiver rx(d);
                rx.configure(d.sampleRate);
                for (size_t i = 0; i < iq.size(); i += 1000) rx.feed(iq.data() + i, std::min<size_t>(1000, iq.size() - i));
                const SignalView v = rx.signalView();
                CHECKM(!v.resolved && v.clusters == 0 && v.centres.empty(), "%s noise only (squelch %s, seed %u): %d clusters, separation %.2f, %llu symbols", c.name,
                       open ? "off" : "default", seed, v.clusters, v.separation, (unsigned long long)v.samples);
                if (!open) CHECKM(!v.have, "%s: a view from a closed squelch (%llu symbols)", c.name, (unsigned long long)v.samples);
            }
        }
    }
}

int main() {
    testClusters();
    testPskEvmFollowsNoise();
    testLowSnrNotResolved();
    testNoiseOnly();
    printf("%d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
