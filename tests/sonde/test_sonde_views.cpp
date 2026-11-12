// The radiosonde-type entries (families 684 .. 717, sonde_rx.h: sondeViews): views onto the radiosonde receiver that report one type only.
// The simulated flights of sonde_gen (every type, with carrier offsets, 8-bit quantisation and noise) are fed to one receiver per view and one without a view, from the same samples:
//   - a view lists the sondes of its type exactly as the receiver without a view lists them, and no other type;
//   - the counts of a view are the sums over its sondes; the receiver without a view is not changed by the others;
//   - a type that is not on the air gives an empty list in its view while the other views are not empty.
// Simulated signals only (no recording of iMet, iMet-54, MRZ, iMS-100 or LMS6 was available; LMS6 is checked for the table and the mask only).
#include "dect2/sonde/sonde_testkit.h"
#include <algorithm>
#include <cstring>
#include <memory>
using namespace dect2;
using namespace dect2::sondetest;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

// the same samples into one receiver per mask (0 = no view); returns each report
static std::vector<SondeTelemetry> runMasks(const SynthConfig& cfg, double rate, double secs, const std::vector<uint32_t>& masks, bool quant8) {
    auto syn = makeSondeSynth(cfg, rate);
    std::vector<std::unique_ptr<SondeReceiver>> rx;
    for (uint32_t m : masks) { rx.emplace_back(new SondeReceiver); rx.back()->setSynchronous(true); rx.back()->configure(rate); rx.back()->setCenterMhz(403.0); rx.back()->setKindMask(m); }
    const size_t total = (size_t)(secs * rate);
    std::vector<cf32> buf(65536);
    for (size_t done = 0; done < total; ) {
        const size_t m = std::min(buf.size(), total - done);
        syn->generate(buf.data(), m);
        if (quant8) for (size_t i = 0; i < m; i++) buf[i] = cf32(std::round(buf[i].real() * 127.f) / 127.f, std::round(buf[i].imag() * 127.f) / 127.f);
        for (auto& r : rx) r->feed(buf.data(), m);
        done += m;
    }
    std::vector<SondeTelemetry> out(rx.size());
    for (size_t i = 0; i < rx.size(); i++) rx[i]->telemetry(out[i], 0);
    return out;
}

static std::vector<std::string> serialsOf(const SondeTelemetry& t, int kind) { std::vector<std::string> v; for (auto& s : t.sondes) if (s.kind == kind) v.push_back(s.serial); std::sort(v.begin(), v.end()); return v; }

int main() {
    int nv = 0;
    const SondeView* views = sondeViews(nv);
    CHECK(nv == 10, "%d views", nv);
    for (int i = 0; i < nv; i++) {
        CHECK(sondeViewForActive(views[i].family + 1) == &views[i] && sondeViewForStd(views[i].family + 2) == &views[i], "view %d lookup", i);
        const ModeTuning t = sondeViewTuning(i);
        CHECK(t.stdMode == views[i].family + 2 && std::string(t.id) == views[i].id && t.defMhz == 403, "view %d tuning", i);
        CHECK(views[i].kind == i + 1, "view %d kind %d", i, views[i].kind);
        for (int j = 0; j < i; j++) CHECK(views[j].family != views[i].family && std::strcmp(views[j].id, views[i].id) != 0, "views %d and %d clash", j, i);
    }
    CHECK(sondeViewForActive(14) == nullptr && sondeViewForStd(15) == nullptr, "the Radiosonde mode is not a view");

    int checkedKinds = 0;
    // scene A: RS41, DFM, M10, M20 and RS92 together, carrier offsets and noise, once with 8-bit samples
    for (int variant = 0; variant < 2; variant++) {
        SynthConfig c; c.mode = 15; c.snrDb = variant ? 24 : 28; c.cfoHz = variant ? -4000 : 2500; c.modeOpt[0] = 31; c.modeOpt[1] = 7;
        std::vector<uint32_t> masks{0};
        const int kinds[] = {SondeRs41, SondeRs92, SondeDfm, SondeM10, SondeM20};
        for (int k : kinds) masks.push_back(1u << k);
        const auto r = runMasks(c, 10e6, 14, masks, variant == 1);
        CHECK(r[0].sondes.size() >= 3, "scene A %d: %zu sondes without a view", variant, r[0].sondes.size());
        size_t total = 0;
        for (size_t i = 0; i < 5; i++) {
            const int k = kinds[i];
            const auto want = serialsOf(r[0], k), got = serialsOf(r[i + 1], k);
            total += r[i + 1].sondes.size();
            CHECK(r[i + 1].sondes.size() == got.size(), "scene A %d: view %s lists another type", variant, sondeKindName(k));
            CHECK(want == got, "scene A %d: view %s lists %zu sondes, the receiver without a view %zu of that type", variant, sondeKindName(k), got.size(), want.size());
            if (!want.empty()) {
                checkedKinds++;
                uint64_t ok = 0; for (auto& s : r[i + 1].sondes) ok += s.framesOk;
                CHECK(r[i + 1].blocksOk == ok && r[i + 1].dataValid && r[i + 1].state >= 1, "scene A %d: view %s counts %llu vs %llu", variant, sondeKindName(k), (unsigned long long)r[i + 1].blocksOk, (unsigned long long)ok);
            } else CHECK(r[i + 1].sondes.empty() && !r[i + 1].dataValid && r[i + 1].blocksOk == 0, "scene A %d: view %s not empty", variant, sondeKindName(k));
            printf("  scene A %d, view %-5s: %zu sondes (without a view: %zu of that type)\n", variant, sondeKindName(k), got.size(), want.size());
        }
        CHECK(total == r[0].sondes.size(), "scene A %d: the views list %zu sondes, the receiver without a view %zu", variant, total, r[0].sondes.size());
    }
    // scene B: iMet, iMet-54, MRZ and iMS-100, one type on the air at a time, with carrier offsets
    struct B { int mask, kind; double cfo; };
    for (const B& b : {B{64, SondeImet, 0.0}, B{128, SondeImet54, 20000.0}, B{256, SondeMrz, -20000.0}, B{512, SondeMeisei, 0.0}}) {
        SynthConfig c; c.mode = 15; c.snrDb = 30; c.modeOpt[0] = b.mask; c.modeOpt[1] = 1; c.cfoHz = b.cfo;
        const int other = b.kind == SondeImet ? SondeMrz : SondeImet;
        const auto r = runMasks(c, 8e6, 16, {0u, 1u << b.kind, 1u << other}, false);
        const auto want = serialsOf(r[0], b.kind), got = serialsOf(r[1], b.kind);
        CHECK(!want.empty(), "type %s: nothing decoded without a view", sondeKindName(b.kind));
        CHECK(want == got && r[1].sondes.size() == got.size(), "type %s: view lists %zu, without a view %zu", sondeKindName(b.kind), got.size(), want.size());
        CHECK(r[2].sondes.empty() && !r[2].dataValid, "type %s: the view of another type listed %zu sondes", sondeKindName(b.kind), r[2].sondes.size());
        printf("  type %-8s offset %6.0f Hz: %zu sonde(s) in its view, %zu in another view\n", sondeKindName(b.kind), b.cfo, got.size(), r[2].sondes.size());
        if (!want.empty()) checkedKinds++;
    }
    // noise only and the LMS6 view: nothing
    { SynthConfig c; c.mode = 15; c.snrDb = -30; c.modeOpt[0] = 1; c.modeOpt[1] = 1; const auto r = runMasks(c, 8e6, 6, {0u, 1u << SondeLms6}, false); CHECK(r[0].sondes.empty() && r[1].sondes.empty(), "noise: %zu / %zu sondes", r[0].sondes.size(), r[1].sondes.size()); }
    { SynthConfig c; c.mode = 15; c.snrDb = 28; c.modeOpt[0] = 1; c.modeOpt[1] = 1; const auto r = runMasks(c, 8e6, 14, {1u << SondeLms6}, false); CHECK(r[0].sondes.empty(), "LMS6 view heard an RS41"); }
    CHECK(checkedKinds >= 6, "%d type checks with data", checkedKinds);
    printf(fails ? "sonde views: %d FAILED\n" : "sonde views: all passed\n", fails);
    return fails ? 1 : 0;
}
