// VDL Mode 3: the codes (Golay (24,12) from the G matrix of Annex 10, Reed-Solomon (72,62)), the training sequences (S1* and S2* are S1 and
// S2 with every phase change turned by 180 degrees, as the Annex notes), the burst bits of every burst kind, and the receiver on the test
// signal: sample rates, carrier offsets, drift, sample rate offset, the channel off the centre, noise, an adjacent channel station, and noise
// alone (no false bursts). Simulated signals only: no VDL Mode 3 recording exists (the system was trialled, never deployed).
#include "dect2/vdl3/vdl3_gen.h"
#include "dect2/vdl3/vdl3_proto.h"
#include "dect2/vdl3/vdl3_rx.h"
#include "dect2/dsp/gen_util.h"
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <map>
#include <string>
#include <vector>
#include "../common/cpu_report.h"
using namespace dect2;
static int fails = 0;
#define CHECK(c, fmt, ...) do { if (!(c)) { printf("FAIL line %d: " fmt "\n", __LINE__ __VA_OPT__(,) __VA_ARGS__); fails++; } } while (0)

#if defined(__SANITIZE_ADDRESS__) || defined(__has_feature)
#if defined(__SANITIZE_ADDRESS__) || __has_feature(address_sanitizer)
#define SANITIZED 1
#endif
#endif
#ifndef SANITIZED
#define SANITIZED 0
#endif

static uint32_t rs32 = 12345;
static uint32_t rnd() { rs32 ^= rs32 << 13; rs32 ^= rs32 >> 17; rs32 ^= rs32 << 5; return rs32; }
static int pop(uint32_t v) { int n = 0; while (v) { v &= v - 1; n++; } return n; }

static void testGolay() {
    std::map<int, int> w;
    for (uint32_t d = 1; d < 4096; d++) w[pop(vdl3GolayEncode(d))]++;
    CHECK(w.size() == 4 && w[8] == 759 && w[12] == 2576 && w[16] == 759 && w[24] == 1, "Golay weight distribution is not the extended Golay code's (G transcribed wrong?)");
    int bad = 0, detected = 0;
    for (int t = 0; t < 4000; t++) {
        const uint32_t d = rnd() & 0xFFF, c = vdl3GolayEncode(d);
        const int ne = t % 5;   // 0 .. 4 bit errors
        uint32_t e = 0;
        while (pop(e) < ne) e |= 1u << (rnd() % 24);
        uint32_t o = 0;
        const int r = vdl3GolayDecode(c ^ e, o);
        if (ne <= 3) { if (r != ne || o != d) bad++; }
        else if (r < 0) detected++;
        else bad++;
    }
    CHECK(bad == 0, "Golay: %d words wrong", bad);
    CHECK(detected == 800, "Golay: %d of 800 four-error words detected", detected);
    printf("  Golay (24,12): weights 8/12/16/24 = 759/2576/759/1, 0-3 errors corrected, 4 detected\n");
}

static void testRs() {
    int bad = 0, failOk = 0;
    for (int t = 0; t < 600; t++) {
        std::vector<uint8_t> d(1 + rnd() % 62);
        for (auto& v : d) v = (uint8_t)rnd();
        auto cw = vdl3RsEncode(d);
        CHECK(cw.size() == 72, "RS word length %zu", cw.size());
        const int ne = t % 7;   // 0 .. 6 octet errors
        std::vector<int> pos;
        while ((int)pos.size() < ne) { const int p = (int)(rnd() % 72); bool dup = false; for (int q : pos) dup |= q == p; if (!dup) pos.push_back(p); }
        auto rx = cw;
        for (int p : pos) rx[(size_t)p] ^= (uint8_t)(1 + rnd() % 255);
        const int r = vdl3RsDecode(rx);
        if (ne <= 5) { if (r != ne || rx != cw) bad++; }
        else if (r < 0 || rx != cw) failOk++;   // 6 errors: beyond the code (a miscorrection is possible but rare)
    }
    CHECK(bad == 0, "RS (72,62): %d words with up to 5 errors not corrected", bad);
    CHECK(failOk >= 80, "RS (72,62): only %d of ~85 six-error words flagged", failOk);
    printf("  Reed-Solomon (72,62): up to 5 octet errors corrected, %d six-error words not passed as good\n", failOk);
}

static void testSyncs() {
    auto steps = [](Vdl3Sync s) { std::vector<int> v; const auto& P = vdl3SyncPhases(s); for (size_t i = 1; i < P.size(); i++) v.push_back(((P[i] - P[i - 1]) % 8 + 8) % 8); return v; };
    for (auto pr : {std::pair{Vdl3Sync::S1, Vdl3Sync::S1Star}, std::pair{Vdl3Sync::S2, Vdl3Sync::S2Star}}) {
        const auto a = steps(pr.first), b = steps(pr.second);
        bool ok = a.size() == 15 && b.size() == 15;
        for (size_t i = 0; ok && i < a.size(); i++) ok = (a[i] + 4) % 8 == b[i];
        CHECK(ok, "%s and %s: the phase changes are not 180 degrees apart (sequence transcribed wrong?)", vdl3SyncName(pr.first), vdl3SyncName(pr.second));
        CHECK(vdl3SyncBits(pr.first).size() == 48 && vdl3SyncPhases(pr.first)[0] == 0, "sync length / first symbol");
    }
    // S1 and S2 must not look alike: differential cross-correlation at every shift
    const auto s1 = steps(Vdl3Sync::S1), s2 = steps(Vdl3Sync::S2);
    double worst = 0;
    for (int sh = -14; sh <= 14; sh++) {
        std::complex<double> c = 0;
        for (int i = 0; i < 15; i++) { const int j = i + sh; if (j < 0 || j >= 15) continue; c += std::polar(1.0, (s1[(size_t)i] - s2[(size_t)j]) * M_PI / 4); }
        worst = std::max(worst, std::abs(c) / 15);
    }
    CHECK(worst < 0.45, "S1 / S2 cross-correlation %.2f", worst);
    printf("  training sequences: S1*/S2* are S1/S2 turned by 180 degrees per change; S1-S2 cross-correlation at most %.2f\n", worst);
}

static void testBits() {
    const Vdl3Kind kinds[] = {Vdl3Kind::MUplink, Vdl3Kind::MUplink3T, Vdl3Kind::Handoff, Vdl3Kind::PollResponse, Vdl3Kind::MDownlink, Vdl3Kind::NetEntry, Vdl3Kind::VdData, Vdl3Kind::VdVoice};
    for (Vdl3Kind k : kinds) {
        std::vector<uint32_t> info;
        for (int i = 0; i < vdl3GolayWords(k); i++) info.push_back(rnd() & 0xFFF);
        std::vector<uint8_t> data = {'H', 'E', 'L', 'L', 'O'}, voice;
        for (int i = 0; i < 576; i++) voice.push_back((uint8_t)(rnd() & 1));
        auto b = vdl3BurstBits(k, info, data, voice);
        CHECK(b.size() == (size_t)(48 + 3 * vdl3PayloadSymbols(k)), "%s: %zu bits", vdl3KindName(k), b.size());
        std::vector<uint8_t> p(b.begin() + 48, b.end());
        for (int e = 0; e < 3 && k != Vdl3Kind::VdVoice; e++) p[(size_t)(rnd() % p.size())] ^= 1;   // a few channel errors
        vdl3Scramble(p);
        Vdl3Decoded d;
        CHECK(vdl3DecodePayload(vdl3SyncFor(k), vdl3PayloadSymbols(k), p, d), "%s: not decoded", vdl3KindName(k));
        CHECK(d.kind == k, "%s decoded as %s", vdl3KindName(k), vdl3KindName(d.kind));
        if (k != Vdl3Kind::VdData) CHECK(d.golayOk && d.info12 == info, "%s: Golay words", vdl3KindName(k));
        if (k == Vdl3Kind::VdData) CHECK(d.rsOk && vdl3PrintableText(d.data) == "HELLO", "data burst: \"%s\"", vdl3PrintableText(d.data).c_str());
    }
    printf("  burst bits: all eight kinds round trip with bit errors\n");
}

static bool g_quant8 = false;   // the samples as an 8-bit radio (or the engine's test source) delivers them: a noise floor below one step is digital silence
struct RunRes { int expect = 0, got = 0, falseKinds = 0, bad = 0; double cpu = 0; };

// The receiver on the test signal against the traffic the generator sent. Only bursts at or above minEsN0 (ground SNR + the burst's level) count.
static RunRes runRx(double rate, double cfo, double snr, double drift, double sro, double chanOff, bool adjacent, double secs, double minEsN0) {
    SynthConfig sc;
    sc.mode = 642;
    sc.snrDb = snr;
    sc.cfoHz = cfo;
    sc.sroPpm = sro;
    sc.modeOpt[1] = 7;
    sc.modeOpt[2] = adjacent ? 1 : 0;
    sc.modeVal[0] = drift;
    sc.modeVal[1] = chanOff;
    auto syn = makeVdl3Synth(sc, rate);
    Vdl3Receiver rx;
    rx.configure(rate);
    rx.setSignalOffset(chanOff - vdl3Tuning().tuneOffsetHz);   // the generator puts the channel tuneOffsetHz below the centre, as a radio tuned off the channel delivers it
    std::vector<cf32> buf(32768);
    RunRes res;
    const size_t total = (size_t)(rate * secs);
    for (size_t done = 0; done < total;) {
        const size_t n = std::min(buf.size(), total - done);
        syn->generate(buf.data(), n);
        if (g_quant8) for (size_t i = 0; i < n; i++) { auto q = [](float x) { return std::round(std::min(127.f, std::max(-128.f, x * 128.f))) / 128.f; }; buf[i] = cf32(q(buf[i].real()), q(buf[i].imag())); }
        const auto t0 = std::chrono::steady_clock::now();
        rx.feed(buf.data(), n);
        res.cpu += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        done += n;
    }
    Vdl3Telemetry t;
    rx.telemetry(t, ~0ull);
    Vdl3GenOptions o;
    o.seed = 7;
    o.adjacent = adjacent;
    Vdl3Traffic tr(o);
    // strong bursts must decode; weak ones may, and a weak one decoded right is not "other"
    std::map<std::string, int> want, weak;   // kind | info -> count
    std::map<std::string, double> sentAt;
    for (;;) {
        const Vdl3TxBurst b = tr.next();
        if (b.t > secs + 0.01) break;
        if (b.chanHz != 0) continue;   // the adjacent station is not ours
        const std::string key = std::string(vdl3KindName(b.kind)) + "|" + vdl3InfoHex(b.info12);
        sentAt[key] = b.t;
        if (snr + b.levelDb < minEsN0 || b.t > secs - 0.04) { weak[key]++; continue; }   // weak, or too near the end to be required
        want[key]++;
        res.expect++;
    }
    int shown = 0;
    for (const auto& m : t.bursts) {
        if (!m.golayOk || m.evmPct >= 25) { res.bad++; continue; }
        const std::string key = m.kindName + "|" + m.info;
        auto it = want.find(key);
        if (it != want.end() && it->second > 0) { it->second--; res.got++; continue; }
        auto w = weak.find(key);
        if (w != weak.end() && w->second > 0) { w->second--; continue; }
        if (m.snrDb < minEsN0 + 2) continue;   // weak as measured: a data burst whose RS word failed shows as voice, which is right
        res.falseKinds++;
        if (shown++ < 4) printf("    other: t %.4f %s %s %d symbols, info %s, EVM %.0f %%, SNR %.0f, RS %d\n", m.timeSec, m.kindName.c_str(), m.sync.c_str(), m.symbols, m.info.c_str(), m.evmPct, m.snrDb, (int)m.rsOk);
    }
    shown = 0;
    for (const auto& [k, n] : want) if (n > 0 && shown++ < 4) printf("    missing: %s at %.4f s\n", k.c_str(), sentAt[k]);
    return res;
}

int main() {
    testGolay();
    testRs();
    testSyncs();
    testBits();

    // modem sweep: rates, offsets, noise
    // the full sweep with ONAIR_TEST_LONG=1; by default one corner of each axis
    const bool longRun = std::getenv("ONAIR_TEST_LONG") != nullptr;
    const std::vector<double> rates = longRun ? std::vector<double>{250e3, 1e6, 2.4e6} : std::vector<double>{250e3, 2.4e6};
    const std::vector<double> cfos = longRun ? std::vector<double>{0, 1500, -1500} : std::vector<double>{-1500};
    for (double rate : rates) {
        for (double cfo : cfos) {
            for (double snr : {30.0, 21.0}) {
                const double secs = 1.2;
                RunRes r = runRx(rate, cfo, snr, 0, 0, 0, false, secs, 20);
                const double frac = r.expect ? (double)r.got / r.expect : 0;
                printf("  %.2f Msps cfo %+5.0f Es/N0 %2.0f dB: %d of %d bursts, %d other, %d Golay-failed, %.2f s CPU\n", rate / 1e6, cfo, snr, r.got, r.expect, r.falseKinds, r.bad, r.cpu);
                CHECK(r.expect >= 40, "too few bursts sent (%d)", r.expect);
                CHECK(frac >= 0.95, "%.2f Msps cfo %.0f Es/N0 %.0f: %d of %d bursts decoded", rate / 1e6, cfo, snr, r.got, r.expect);
                CHECK(r.falseKinds <= (snr > 25 ? 0 : 1), "%.2f Msps cfo %.0f Es/N0 %.0f: %d bursts decoded with the wrong kind or bits", rate / 1e6, cfo, snr, r.falseKinds);
                cpuCal("vdl3_rx", cpuCalMsps(rate) + ", cfo " + std::to_string((long long)cfo), r.cpu / secs, "cpu-s/s", '<', 4, !SANITIZED);
                CHECK(SANITIZED || r.cpu < secs * 4, "%.2f Msps: %.2f s CPU for %.1f s", rate / 1e6, r.cpu, secs);
            }
        }
    }
    // 8-bit samples with gaps of digital silence between the bursts (the QA sweep's file path; 3.9 never started a burst: its floor was zero)
    {
        g_quant8 = true;
        RunRes r = runRx(1e6, 0, 60, 0, 0, 0, false, 1.2, 20);
        g_quant8 = false;
        printf("  8-bit samples, silence between bursts: %d of %d, %d other\n", r.got, r.expect, r.falseKinds);
        CHECK(r.expect >= 20 && r.got >= r.expect * 95 / 100 && r.falseKinds <= 1, "8-bit samples: %d of %d bursts (%d other)", r.got, r.expect, r.falseKinds);
    }
    // drift, sample rate offset, the channel 100 kHz below the centre (the mode's tuning offset), all at once
    {
        RunRes r = runRx(1e6, 800, 25, 150, 60, -100e3, false, 1.5, 20);
        printf("  drift 150 Hz/s, SRO 60 ppm, channel at -100 kHz: %d of %d, %d other\n", r.got, r.expect, r.falseKinds);
        CHECK(r.expect && r.got >= r.expect * 97 / 100 && r.falseKinds == 0, "drift / SRO / offset: %d of %d (%d other)", r.got, r.expect, r.falseKinds);
    }
    // an adjacent ground station 25 kHz above, at the same level and overlapping in time
    {
        RunRes r = runRx(1e6, 0, 28, 0, 0, 0, true, 1.2, 20);
        printf("  adjacent channel station: %d of %d, %d other\n", r.got, r.expect, r.falseKinds);
        CHECK(r.expect && r.got >= r.expect * 95 / 100 && r.falseKinds == 0, "adjacent channel: %d of %d (%d other)", r.got, r.expect, r.falseKinds);
    }
    // where it stops: report only
    for (double snr : {18.0, 15.0}) {
        RunRes r = runRx(1e6, 300, snr, 0, 0, 0, false, 1.2, snr);
        printf("  Es/N0 %.0f dB (ground bursts only): %d of %d\n", snr, r.got, r.expect);
    }
    // noise alone: nothing decoded
    {
        const double rate = 1e6;
        Vdl3Receiver rx;
        rx.configure(rate);
        genutil::NoiseSource ns(3);
        std::vector<cf32> buf(65536);
        for (int i = 0; i < 40; i++) { std::fill(buf.begin(), buf.end(), cf32(0, 0)); ns.add(buf.data(), buf.size(), 0.05f); rx.feed(buf.data(), buf.size()); }
        Vdl3Telemetry t;
        rx.telemetry(t, ~0ull);
        printf("  noise only, %.1f s: %llu syncs, %llu good, %llu bad\n", t.nowSec, (unsigned long long)t.syncs, (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad);
        CHECK(t.blocksOk == 0, "noise produced %llu good bursts", (unsigned long long)t.blocksOk);
        CHECK(t.state == 0 && vdl3Summary(t).find("listening") != std::string::npos, "noise: %s", vdl3Summary(t).c_str());
    }
    // unusable rates: no crash, the rate is reported
    for (double r : {0.0, -5.0, 1.0, 1e5, 1e12, std::nan("")}) {
        Vdl3Receiver rx;
        rx.configure(r);
        std::vector<cf32> zz(4096, cf32(0, 0));
        rx.feed(zz.data(), zz.size());
        rx.feed(zz.data(), 0);
        Vdl3Telemetry t;
        rx.telemetry(t, ~0ull);
        CHECK(!t.rateOk && vdl3Summary(t).find("too low") != std::string::npos, "rate %g must be refused: %s", r, vdl3Summary(t).c_str());
    }
    if (fails) printf("vdl3: %d FAILED\n", fails);
    else printf("vdl3: all passed\n");
    return fails ? 1 : 0;
}
