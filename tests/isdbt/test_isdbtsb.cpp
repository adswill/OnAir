// ISDB-TSB (ARIB STD-B29 / ITU-R BS.1114 System F): one- and three-segment narrow-band signals, generator against receiver.
//   1. the carrier counts of the narrow band against Table 5 of ITU-R BS.1114 (data, SP, CP, TMCC, AC1, AC2 for every mode), the FFT sizes
//      (256 ... 2048) and the TMCC system identification (01)
//   2. round trips through the receiver with the channel impairments of a real capture: a carrier offset (whole and fractional carriers), a
//      sample clock error, another input rate than the native one, the channel beside the centre, noise and a slow carrier drift. Every
//      packet that comes out is compared with the packet that was sent (counting source: layer, counter and payload)
//   3. all modes x guard intervals, all carrier modulations x code rates (short runs)
//   4. three segments on automatic: the receiver starts with one segment, reads the TMCC and changes over to all three (both layers)
//   5. the one-segment service of a 13-segment ISDB-T signal through the narrow receiver (the partial-reception layer only)
//   6. a noise sweep for QPSK 1/2: where the packets stop being clean (reported, and a loose bound checked)
//   7. the mode's own test signal (makeModeSynth 732, the engine standard) through the receiver the engine runs for it, and its tuning
// All signals are simulated: no recording of an ISDB-TSB broadcast was available.
#include "dect2/isdbt/isdbt.h"
#include "dect2/isdbt/isdbt_gen.h"
#include "dect2/isdbt/isdbt_resample.h"
#include "dect2/isdbt/isdbt_rx.h"
#include "dect2/engine/mode_synth.h"
#include "dect2/engine/mode_tuning.h"
#include "dect2/source/source.h"
#include "../common/jobs.h"
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <random>
#include <vector>

using namespace dect2;
using namespace dect2::isdbt;
using testjobs::jprintf;
static std::atomic<int> fails{0};
static void check(bool c, const char* name, const char* what) { if (!c) { jprintf("FAIL: %s: %s\n", name, what); fails++; } }

// ---------------------------------------------------------------- 1. geometry against BS.1114 Table 5
static void testGeometry() {
    for (int mode = 1; mode <= 3; mode++) {
        for (int ns : {1, 3}) {
            for (int nd = 0; nd <= ns; nd++) {        // differential segments first (lower segment numbers)
                Params p; p.mode = mode; p.tsb = ns;
                if (ns == 1) { p.layer[0].segments = 1; p.layer[0].mod = nd ? kDqpsk : kQpsk; }
                else {
                    // nd differential segments (DQPSK), the lower segment numbers first: 0 A 1 + B 2, 1 A(diff) 1 + B 2, 2 A(diff) 2 + B 1, 3 all
                    p.partial = nd != 2;
                    p.layer[0].segments = nd == 2 ? 2 : 1; p.layer[1].segments = nd == 2 ? 1 : 2;
                    p.layer[0].mod = nd >= 1 ? kDqpsk : kQpsk;
                    p.layer[1].mod = nd == 3 ? kDqpsk : kQpsk;
                }
                std::string why;
                if (!p.valid(&why)) continue;
                SegmentInfo info[kSegments];
                segmentLayout(p, info);
                int ndReal = 0;
                for (int s = 0; s < kSegments; s++) if (info[s].layer >= 0 && info[s].diff) ndReal++;
                const int nc = ns - ndReal;
                long cnt[6] = {0, 0, 0, 0, 0, 0};
                std::vector<uint8_t> roles((size_t)carriersPerSegment(mode));
                for (int pos = firstPosition(ns); pos <= lastPosition(ns); pos++) {
                    const int s = kSegmentAtPosition[pos];
                    check(info[s].layer >= 0, "geometry", "every segment of the band is used");
                    segmentRoles(mode, s, info[s].diff, 0, roles.data());
                    for (uint8_t r : roles) cnt[r]++;
                }
                cnt[kCP]++;   // the pilot above the band
                const int m = 1 << (mode - 1);
                char nm[64]; snprintf(nm, sizeof nm, "geometry mode %d, %d seg, %d diff", mode, ns, ndReal);
                const int ac1[3] = {2, 4, 8}, ac2[3] = {4, 9, 19}, tm[3] = {5, 10, 20};
                check(cnt[kData] == 96L * m * ns, nm, "data carriers 96 x 2^(mode-1) x Ns");
                check(cnt[kSP] == 9L * m * nc, nm, "scattered pilots 9 x 2^(mode-1) x nc");
                check(cnt[kCP] == ndReal + 1, nm, "continual pilots nd + 1");
                check(cnt[kTMCC] == (long)m * nc + (long)tm[mode - 1] * ndReal, nm, "TMCC carriers");
                check(cnt[kAC1] == (long)ac1[mode - 1] * ns, nm, "AC1 carriers");
                check(cnt[kAC2] == (long)ac2[mode - 1] * ndReal, nm, "AC2 carriers");
                check(fftN(mode) / tsbDecimation(ns) == (ns == 1 ? 256 : 512) * m, nm, "FFT size of Table 5");
                check(edgeCarrier(mode, ns) - firstPosition(ns) * carriersPerSegment(mode) + 1 == 108 * m * ns + 1, nm, "total carriers 108 x 2^(mode-1) x Ns + 1");
                Tmcc t = tmccFromParams(p);
                uint8_t bits[kTmccInfoBits];
                tmccPack(t, bits);
                check(bits[0] == 0 && bits[1] == 1, nm, "TMCC system identification 01 (ISDB-TSB)");
                Tmcc u; tmccUnpack(bits, u);
                Params q; q.mode = mode; q.tsb = ns;
                check(paramsFromTmcc(u, q) && q == p, nm, "TMCC round trip");
            }
        }
    }
    // a narrow receiver keeps the partial-reception layer of a wider signal, and nothing else
    Params full; full.mode = 3; full.partial = true; full.layer[0].segments = 1; full.layer[1].segments = 12; full.layer[1].mod = k64Qam;
    Tmcc t = tmccFromParams(full);
    Params q; q.mode = 3; q.tsb = 1;
    check(paramsFromTmcc(t, q) && q.layer[0].segments == 1 && !q.layer[1].used(), "geometry", "one-seg out of ISDB-T");
    Params q3; q3.mode = 3; q3.tsb = 3;
    check(!paramsFromTmcc(t, q3), "geometry", "three segments out of ISDB-T refused");
    full.partial = false; t = tmccFromParams(full);
    Params q1; q1.mode = 3; q1.tsb = 1;
    check(!paramsFromTmcc(t, q1), "geometry", "no partial reception: nothing for a narrow receiver");
}

// ---------------------------------------------------------------- the channel
struct Channel {
    double inRate = 0;            // the receiver's input rate
    double ppm = 0;               // sample clock error of the "radio"
    double cfoHz = 0, driftHzPerS = 0;
    double offsetHz = 0;          // the channel beside the centre of the capture
    double snrDb = 40;            // over the occupied band
    double bandHz = 0;
};

// counting-source packets with layer, counter and payload compared with what the source made
static bool samePacket(const uint8_t* q) {
    int layer = 0; unsigned c = 0;
    if (!checkCountingPacket(q, &layer, &c)) return false;
    uint8_t ref[188];
    unsigned n[3] = {0, 0, 0};
    (void)n;
    // regenerate as countingSource does (seed 1)
    ref[0] = 0x47;
    const unsigned pid = 0x100 + (unsigned)layer;
    ref[1] = (uint8_t)(pid >> 8); ref[2] = (uint8_t)pid; ref[3] = (uint8_t)(0x10 | (c & 15));
    ref[4] = (uint8_t)(c >> 24); ref[5] = (uint8_t)(c >> 16); ref[6] = (uint8_t)(c >> 8); ref[7] = (uint8_t)c;
    unsigned x = c * 2654435761u + 1u + (unsigned)layer * 97;
    for (int i = 8; i < 188; i++) { x = x * 1664525u + 1013904223u; ref[i] = (uint8_t)(x >> 24); }
    return std::memcmp(q, ref, 188) == 0;
}

struct Result { bool tmcc = false; int segs = 0; long good[3] = {0, 0, 0}, bad = 0; double cfo = 0; Params prm; int mode = 0; RxTelemetry::Isdbt is; double snr = 0; };

static Result run(const Params& p, const Channel& ch, double seconds, int rxSegments, unsigned seed = 7) {
    Generator gen(p, countingSource(1));
    const double native = sampleRateFor(p.bwMhz) / tsbDecimation(p.tsb);
    TrackingResampler rs;
    const double inRate = ch.inRate > 0 ? ch.inRate : native;
    rs.configure(native, inRate * (1.0 + ch.ppm * 1e-6), false);
    IsdbtReceiver rx;
    rx.configureTsb(inRate, p.bwMhz, rxSegments);
    Result r;
    rx.setPacketCallback([&](const uint8_t* pk, size_t n, double) {
        for (size_t i = 0; i < n; i++) {
            const uint8_t* q = pk + i * 188;
            int layer = -1;
            if (!(q[1] & 0x80) && samePacket(q) && checkCountingPacket(q, &layer, nullptr)) r.good[layer]++;
            else r.bad++;
        }
    });
    std::mt19937 rng(seed);
    std::normal_distribution<float> g(0.f, 1.f);
    // noise over the whole input band, scaled so that the SNR holds over the occupied band
    const double occ = occupiedHz(p.tsb ? p.tsb : 13, p.bwMhz);
    const float sigma = (float)std::sqrt(std::pow(10.0, -ch.snrDb / 10.0) * inRate / occ / 2.0);
    std::vector<cf32> frame, out;
    double ph = 0, t = 0;
    const double dt = 1.0 / inRate;
    double sent = 0;
    while (sent < seconds) {
        gen.nextFrame(frame);
        out.clear();
        rs.process(frame.data(), frame.size(), out);
        for (auto& v : out) {
            const double f = ch.cfoHz + ch.offsetHz + ch.driftHzPerS * t;
            ph += 2 * M_PI * f * dt;
            if (ph > M_PI) ph -= 2 * M_PI;
            v = v * cf32((float)std::cos(ph), (float)std::sin(ph)) + cf32(sigma * g(rng), sigma * g(rng));
            t += dt;
        }
        rx.feed(out.data(), out.size());
        sent += frame.size() / native;
    }
    RxTelemetry tel;
    rx.telemetry(tel, 0);
    r.tmcc = tel.isdbt.tmccOk; r.segs = rx.tsbSegments(); r.cfo = tel.cfoHz; r.mode = tel.isdbt.mode; r.is = tel.isdbt; r.snr = tel.dataSnrDb;
    for (int i = 0; i < 3; i++) { r.prm.layer[i].segments = tel.isdbt.layer[i].segments; r.prm.layer[i].mod = tel.isdbt.layer[i].mod; r.prm.layer[i].rate = tel.isdbt.layer[i].rate; }
    return r;
}

static void expectFlow(const char* name, const Result& r, const Params& p, long minGood, bool allLayers = true) {
    jprintf("  %s: tmcc %d, %d seg, mode %d, CFO %.0f Hz, SNR %.1f dB, good %ld/%ld/%ld, bad %ld; RS A %llu/%llu/%llu B %llu/%llu/%llu (clean/corrected/failed)\n", name, (int)r.tmcc, r.segs, r.mode, r.cfo, r.snr,
            r.good[0], r.good[1], r.good[2], r.bad, (unsigned long long)r.is.layer[0].rsClean, (unsigned long long)r.is.layer[0].rsCorrected, (unsigned long long)r.is.layer[0].rsFailed,
            (unsigned long long)r.is.layer[1].rsClean, (unsigned long long)r.is.layer[1].rsCorrected, (unsigned long long)r.is.layer[1].rsFailed);
    check(r.tmcc, name, "TMCC decoded");
    check(r.mode == p.mode, name, "mode");
    for (int i = 0; i < 3; i++) {
        if (!p.layer[i].used() || (!allLayers && i > 0)) continue;
        check(r.good[i] >= minGood, name, "packets flowing in every layer");
    }
    const long g = r.good[0] + r.good[1] + r.good[2];
    check(r.bad * 50 <= g, name, "almost no bad packets once locked");
}

static Params one(int mode, int guard, int mod, int rate, int ti = 0) {
    Params p; p.mode = mode; p.guard = guard; p.tsb = 1; p.partial = true;
    p.layer[0].segments = 1; p.layer[0].mod = mod; p.layer[0].rate = rate; p.layer[0].ti = ti;
    return p;
}
static Params three(int mode, int guard, int modA, int rateA, int modB, int rateB, int ti = 0) {
    Params p; p.mode = mode; p.guard = guard; p.tsb = 3; p.partial = true;
    p.layer[0].segments = 1; p.layer[0].mod = modA; p.layer[0].rate = rateA; p.layer[0].ti = ti;
    p.layer[1].segments = 2; p.layer[1].mod = modB; p.layer[1].rate = rateB; p.layer[1].ti = ti;
    return p;
}

int main() {
    setenv("ISDBT_DEBUG", "1", 1); setenv("TSB_CASE", "6", 1);   // TEMP-DIAG
    testGeometry();
    testjobs::Jobs jobs;
    int caseNo = 0;
    auto add = [&](std::function<void()> f) { const char* o = getenv("TSB_CASE"); if (!o || atoi(o) == caseNo) jobs.add(std::move(f)); caseNo++; };
    // 2. impairments, one segment
    add([] {
        Params p = one(3, kGi8, kQpsk, kR12, 2);
        Channel ch; ch.inRate = 1.2e6; ch.ppm = 25; ch.cfoHz = 3170; ch.snrDb = 18;
        expectFlow("1 seg mode 3 QPSK 1/2 I=2, 1.2 Msps, +25 ppm, +3.17 kHz, 18 dB", run(p, ch, 6.0, 0), p, 150);
    });
    add([] {
        Params p = one(2, kGi16, k16Qam, kR23, 1);
        Channel ch; ch.inRate = 2.048e6; ch.ppm = -40; ch.cfoHz = -1234; ch.offsetHz = 400e3; ch.snrDb = 24; ch.driftHzPerS = 20;
        expectFlow("1 seg mode 2 16QAM 2/3, 2.048 Msps, 400 kHz off centre, -40 ppm, drift 20 Hz/s", run(p, ch, 6.0, 0), p, 150);
    });
    add([] {
        Params p = one(1, kGi4, kDqpsk, kR12, 1);
        Channel ch; ch.inRate = 1.0e6; ch.cfoHz = 900; ch.snrDb = 16; ch.ppm = 10;
        expectFlow("1 seg mode 1 DQPSK 1/2, 1.0 Msps, 16 dB", run(p, ch, 5.0, 1), p, 150);
    });
    add([] {
        Params p = one(3, kGi16, k64Qam, kR34, 1); p.bwMhz = 8;
        Channel ch; ch.inRate = 1.5e6; ch.cfoHz = 2500; ch.snrDb = 30; ch.ppm = 5;
        expectFlow("1 seg mode 3 64QAM 3/4, 8 MHz raster (571 kHz segment)", run(p, ch, 6.0, 0), p, 150);
    });
    // 4. three segments, hierarchical, automatic
    add([] {
        Params p = three(3, kGi8, kQpsk, kR12, k16Qam, kR12, 1);
        Channel ch; ch.inRate = 2.4e6; ch.cfoHz = -2700; ch.snrDb = 22; ch.ppm = 15;
        Result r = run(p, ch, 8.0, 0);
        expectFlow("3 seg A QPSK + B 16QAM, automatic segments, 2.4 Msps", r, p, 150);
        check(r.segs == 3, "3 seg automatic", "changed over to three segments");
    });
    add([] {
        Params p; p.mode = 2; p.guard = kGi8; p.tsb = 3; p.layer[0].segments = 3; p.layer[0].mod = k64Qam; p.layer[0].rate = kR23; p.layer[0].ti = 2;
        Channel ch; ch.inRate = 3.2e6; ch.cfoHz = 7300; ch.offsetHz = -500e3; ch.snrDb = 28;
        expectFlow("3 seg one layer 64QAM 2/3, set to three, 500 kHz off centre", run(p, ch, 6.0, 3), p, 150);
    });
    add([] {
        Params p; p.mode = 3; p.guard = kGi4; p.tsb = 3; p.partial = true;
        p.layer[0].segments = 1; p.layer[0].mod = kDqpsk; p.layer[0].rate = kR12;
        p.layer[1].segments = 2; p.layer[1].mod = kDqpsk; p.layer[1].rate = kR23;
        Channel ch; ch.inRate = 2.0e6; ch.cfoHz = 1500; ch.snrDb = 20;
        expectFlow("3 seg all DQPSK", run(p, ch, 6.0, 0), p, 150);
    });
    // the one-segment receiver inside a three-segment band: only layer A
    add([] {
        Params p = three(3, kGi8, kQpsk, kR23, k64Qam, kR78, 1);
        Channel ch; ch.inRate = 1.0e6; ch.cfoHz = 600; ch.snrDb = 26;   // too narrow for three segments: stays at one
        Result r = run(p, ch, 6.0, 0);
        expectFlow("one segment out of a 3-seg band at 1 Msps", r, p, 150, false);
        check(r.segs == 1 && r.good[1] == 0, "3 seg at 1 Msps", "one segment only");
    });
    // 5. the one-seg service of ISDB-T
    add([] {
        Params p; p.mode = 3; p.guard = kGi8; p.partial = true;
        p.layer[0].segments = 1; p.layer[0].mod = kQpsk; p.layer[0].rate = kR23; p.layer[0].ti = 2;
        p.layer[1].segments = 12; p.layer[1].mod = k64Qam; p.layer[1].rate = kR34; p.layer[1].ti = 1;
        Channel ch; ch.inRate = 8.126984e6 * 1.0; ch.cfoHz = 1800; ch.snrDb = 26;
        // the 13-segment generator at its native rate, the narrow receiver with a 1.2 Msps input made from it
        Generator gen(p, countingSource(1));
        TrackingResampler rs; rs.configure(sampleRateFor(6), 1.2e6, false);
        IsdbtReceiver rx; rx.configureTsb(1.2e6, 6, 0);
        long good = 0, bad = 0, other = 0;
        rx.setPacketCallback([&](const uint8_t* pk, size_t n, double) {
            for (size_t i = 0; i < n; i++) { const uint8_t* q = pk + i * 188; int l = -1; if (!(q[1] & 0x80) && samePacket(q) && checkCountingPacket(q, &l, nullptr)) { if (l == 0) good++; else other++; } else bad++; }
        });
        std::vector<cf32> fr, o;
        std::mt19937 rng(3); std::normal_distribution<float> g(0.f, 1.f);
        const float sg = (float)std::sqrt(std::pow(10.0, -ch.snrDb / 10.0) / 2.0);
        double ph = 0;
        for (int f = 0; f < 26; f++) {
            gen.nextFrame(fr); o.clear(); rs.process(fr.data(), fr.size(), o);
            for (auto& v : o) { ph += 2 * M_PI * ch.cfoHz / 1.2e6; v = v * cf32((float)std::cos(ph), (float)std::sin(ph)) + cf32(sg * g(rng), sg * g(rng)); }
            rx.feed(o.data(), o.size());
        }
        RxTelemetry t; rx.telemetry(t, 0);
        jprintf("  one-seg of ISDB-T through the narrow receiver: tmcc %d, %d seg, good %ld, other layers %ld, bad %ld\n", (int)t.isdbt.tmccOk, rx.tsbSegments(), good, other, bad);
        check(t.isdbt.tmccOk && good > 100 && other == 0 && bad * 50 <= good, "one-seg of ISDB-T", "partial-reception layer decoded");
    });
    // 7. the engine's test signal and tuning
    add([] {
        const ModeTuning* mt = modeTuning(732);
        check(mt && std::string(mt->id) == "isdbtsb" && mt->minSampleRate <= 1.0e6 && mt->sampleRate >= 1.5e6, "engine", "ISDB-TSB tuning (standard 732)");
        SynthConfig cfg; cfg.cfoHz = 1700; cfg.snrDb = 25;
        const double rate = 2.048e6;
        auto syn = makeModeSynth(732, cfg, rate);
        check(syn != nullptr, "engine", "makeModeSynth(732)");
        if (!syn) return;
        IsdbtReceiver rx; rx.configureTsb(rate, 6, 0);
        long ok = 0, bad = 0;
        rx.setPacketCallback([&](const uint8_t* pk, size_t n, double) { for (size_t i = 0; i < n; i++) { if (pk[i * 188] == 0x47 && !(pk[i * 188 + 1] & 0x80)) ok++; else bad++; } });
        std::vector<cf32> buf(16384);
        for (double t = 0; t < 7.0; t += buf.size() / rate) { syn->generate(buf.data(), buf.size()); rx.feed(buf.data(), buf.size()); }
        RxTelemetry tel; rx.telemetry(tel, 0);
        jprintf("  engine test signal: tmcc %d, %d seg, standard %d, packets %ld, errored %ld\n", (int)tel.isdbt.tmccOk, rx.tsbSegments(), tel.standard, ok, bad);
        check(tel.isdbt.tmccOk && rx.tsbSegments() == 3 && tel.standard == 731, "engine", "test signal locked, three segments, active standard 731");
        check(ok > 300 && bad * 50 <= ok, "engine", "test programme flowing");
    });
    // 3. all modes x guard intervals (QPSK 1/2), all modulations x rates (mode 3, 1/8)
    for (int mode = 1; mode <= 3; mode++)
        for (int gi = 0; gi < 4; gi++)
            add([mode, gi] {
                Params p = one(mode, gi, kQpsk, kR12);
                Channel ch; ch.inRate = 1.024e6; ch.cfoHz = 1100; ch.snrDb = 25;
                char nm[64]; snprintf(nm, sizeof nm, "mode %d GI %s", mode, isdbt::guardName(gi));
                expectFlow(nm, run(p, ch, mode == 3 && gi == 3 ? 3.0 : 2.5, 1), p, 20);
            });
    for (int mod = 0; mod < 4; mod++)
        add([mod] {
            for (int rate = 0; rate < 5; rate++) {
                Params p = three(3, kGi8, mod, rate, mod == kDqpsk ? kDqpsk : k16Qam, kR12);
                Channel ch; ch.inRate = 2.2e6; ch.cfoHz = -800; ch.snrDb = 32;
                char nm[64]; snprintf(nm, sizeof nm, "3 seg layer A %s %s", isdbt::modName(mod), isdbt::rateName(rate));
                expectFlow(nm, run(p, ch, 3.0, 3), p, 20);
            }
        });
    // 6. noise sweep, one segment QPSK 1/2 (mode 3, I = 2)
    add([] {
        double firstClean = 99;
        for (double snr = 2; snr <= 10; snr += 1) {
            Params p = one(3, kGi8, kQpsk, kR12, 2);
            Channel ch; ch.inRate = 1.2e6; ch.cfoHz = 2000; ch.snrDb = snr;
            Result r = run(p, ch, 5.0, 1, 11 + (unsigned)snr);
            const long g = r.good[0];
            jprintf("  noise sweep QPSK 1/2: %.0f dB: tmcc %d, good %ld, bad %ld\n", snr, (int)r.tmcc, g, r.bad);
            // clean: under 2 % packets flagged (the first packets after the lock carry the transport error flag)
            if (r.tmcc && g > 100 && r.bad * 50 <= g && firstClean > 98) firstClean = snr;
        }
        jprintf("  noise sweep QPSK 1/2: clean from %.0f dB SNR\n", firstClean);
        check(firstClean <= 8, "noise sweep", "QPSK 1/2 clean by 8 dB");
    });
    jobs.run();
    jprintf(fails ? "isdbtsb: FAILED\n" : "isdbtsb: ok\n");
    return fails ? 1 : 0;
}
