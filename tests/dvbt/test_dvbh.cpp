// DVB-H physical layer (EN 300 744 annex F) on the DVB-T receiver: generator against receiver.
//   1. tables: the 4K permutation is a permutation of 3024 cells, the 4K pilot and TPS carriers of tables F.4 / F.5, every TPS combination
//      (2K / 4K / 8K, native / in-depth, the DVB-H bits s48 s49, hierarchy) coded and decoded again
//   2. round trips with impairments (carrier offset of whole and fractional carriers, sample clock error, another input rate, noise):
//      4K native, 4K in-depth, 2K in-depth (whose blocks of four symbols run across the frames), 8K with the DVB-H bits; every packet
//      compared with the one sent, and the TPS flags read back
// All signals are simulated: no recording of a DVB-H transmission was available.
#include "dect2/dvbt/dvbt.h"
#include "dect2/dvbt/dvbt_gen.h"
#include "dect2/dvbt/dvbt_rx.h"
#include "dect2/isdbt/isdbt_resample.h"
#include "dect2/t2/t2.h"
#include "../common/jobs.h"
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <random>
#include <vector>

using namespace dect2;
using testjobs::jprintf;
static std::atomic<int> fails{0};
static void check(bool c, const char* name, const char* what) { if (!c) { jprintf("FAIL: %s: %s\n", name, what); fails++; } }

static void testTables() {
    for (int mode = 0; mode < 3; mode++) {
        const auto& H = dvbt::symbolPermutation(mode);
        std::vector<int> seen((size_t)dvbt::dataCarriers(mode), 0);
        bool ok = (int)H.size() == dvbt::dataCarriers(mode);
        for (int h : H) { if (h < 0 || h >= (int)seen.size() || seen[(size_t)h]++) ok = false; }
        check(ok, "tables", "the symbol permutation is a permutation of the data cells");
        // data carriers = K - pilots: the roles add up for every symbol
        for (int s = 0; s < 4; s++) {
            std::vector<uint8_t> roles;
            dvbt::carrierRoles(mode, s, roles);
            int nd = 0;
            for (uint8_t r : roles) nd += r == 0;
            check(nd == dvbt::dataCarriers(mode), "tables", "data carriers per symbol");
        }
    }
    check(dvbt::continualPilots(dvbt::k4K).size() == 89 && dvbt::continualPilots(dvbt::k4K).back() == 3408, "tables", "4K: 89 continual pilots up to 3408 (F.4)");
    check(dvbt::tpsCarriers(dvbt::k4K).size() == 34 && dvbt::tpsCarriers(dvbt::k4K).back() == 3391, "tables", "4K: 34 TPS carriers up to 3391 (F.5)");
    check(dvbt::fftN(dvbt::k4K) == 4096 && dvbt::carriersK(dvbt::k4K) == 3409, "tables", "4K: 4096-point FFT, 3409 carriers");
    int combos = 0;
    for (int mode = 0; mode < 3; mode++)
        for (int indepth = 0; indepth < 2; indepth++)
            for (int ts = 0; ts < 2; ts++)
                for (int fec = 0; fec < 2; fec++)
                    for (int hier = 0; hier < 4; hier++)
                        for (int fr = 0; fr < 4; fr++) {
                            if (indepth && mode == dvbt::k8K) continue;
                            dvbt::Params p; p.mode = mode; p.indepth = indepth; p.dvbh = true; p.timeSlicing = ts; p.mpeFec = fec; p.hier = hier;
                            p.mod = hier ? dvbt::k16Qam : dvbt::kQpsk; p.cellId = 0x1234;
                            const auto b = dvbt::tpsBits(p, fr);
                            dvbt::Params q; int fi = -1; bool odd = false;
                            const bool ok = dvbt::tpsDecode(b.data(), q, fi, odd, 0);
                            check(ok && fi == fr && q.mode == mode && q.indepth == (bool)indepth && q.dvbh && q.timeSlicing == (bool)ts && q.mpeFec == (bool)fec && q.hier == hier,
                                  "tps", "DVB-H TPS round trip");
                            combos++;
                        }
    // an 8K signal with the in-depth flag is not valid
    dvbt::Params p8; p8.mode = dvbt::k8K; auto b = dvbt::tpsBits(p8, 0);
    b[27] = 1;
    {   // recompute the BCH with s27 set by coding a 2K block with the flag and moving its mode bits: simpler, code it as 4K then flip the mode
        dvbt::Params q; int fi; bool odd;
        check(!dvbt::tpsDecode(b.data(), q, fi, odd, 0), "tps", "a damaged block is refused");
    }
    // plain DVB-T stays plain: no DVB-H flags
    dvbt::Params pt; pt.mode = dvbt::k8K; pt.cellIdLength = true;
    { const auto bt = dvbt::tpsBits(pt, 1); dvbt::Params q; int fi; bool odd; check(dvbt::tpsDecode(bt.data(), q, fi, odd, 0) && !q.dvbh && !q.indepth && q.cellIdLength, "tps", "DVB-T without DVB-H bits"); }
    jprintf("  TPS: %d DVB-H combinations coded and decoded\n", combos);
}

struct Channel { double inRate = 0, ppm = 0, cfoHz = 0, snrDb = 30; };
struct Result { long good = 0, bad = 0, lost = 0; bool tps = false, indepth = false, dvbh = false, ts = false, fec = false; int fftN = 0; };

static Result run(const dvbt::Params& p, const Channel& ch, double seconds, double bwMhz = 8) {
    const double fn = nativeRateHz(bwMhz);
    uint32_t counter = 0;
    dvbt::Generator gen(p, [&](uint8_t* pkt) {
        pkt[0] = 0x47; pkt[1] = 0x01; pkt[2] = 0x00; pkt[3] = 0x10;
        memcpy(pkt + 4, &counter, 4);
        for (int i = 8; i < 188; i++) pkt[i] = (uint8_t)(counter * 31 + i * 7);
        counter++;
    });
    const double inRate = ch.inRate > 0 ? ch.inRate : fn;
    DvbtReceiver rx;
    rx.configure(inRate, bwMhz);
    Result r;
    uint32_t expect = 0; bool started = false;
    rx.setPacketCallback([&](const uint8_t* pk, size_t n, double) {
        for (size_t i = 0; i < n; i++) {
            const uint8_t* q = pk + i * 188;
            if (q[1] & 0x80) { if (started) r.bad++; continue; }
            if (q[1] != 0x01) continue;
            uint32_t c; memcpy(&c, q + 4, 4);
            bool ok = q[0] == 0x47;
            for (int j = 8; j < 188 && ok; j++) ok = q[j] == (uint8_t)(c * 31 + j * 7);
            if (!ok) { if (started) r.bad++; continue; }
            if (started && c != expect) r.lost += (long)(c - expect);
            started = true; expect = c + 1; r.good++;
        }
    });
    isdbt::TrackingResampler rs;
    rs.configure(fn, inRate * (1.0 + ch.ppm * 1e-6), false);
    std::mt19937 rng(5);
    std::normal_distribution<float> g(0.f, 1.f);
    const float sigma = (float)std::sqrt(std::pow(10.0, -ch.snrDb / 10.0) * (inRate / fn) / 2.0);
    std::vector<cf32> sym, block, out;
    double ph = 0, sent = 0;
    const double dph = 2 * M_PI * ch.cfoHz / inRate;
    while (sent < seconds) {
        block.clear();
        for (int s = 0; s < 34; s++) { gen.nextSymbol(sym); block.insert(block.end(), sym.begin(), sym.end()); }
        sent += block.size() / fn;
        out.clear();
        rs.process(block.data(), block.size(), out);
        for (auto& v : out) { ph = std::remainder(ph + dph, 2 * M_PI); v = v * cf32((float)std::cos(ph), (float)std::sin(ph)) + cf32(sigma * g(rng), sigma * g(rng)); }
        rx.feed(out.data(), out.size());
    }
    RxTelemetry t;
    rx.telemetry(t, 0);
    r.tps = t.dvbt.tpsOk; r.indepth = t.dvbt.indepth; r.dvbh = t.dvbt.dvbh; r.ts = t.dvbt.timeSlicing; r.fec = t.dvbt.mpeFec; r.fftN = t.fftN;
    return r;
}

static void expect(const char* name, const dvbt::Params& p, const Result& r, long minGood) {
    jprintf("  %s: TPS %d, FFT %d, in-depth %d, DVB-H %d (TS %d, MPE-FEC %d), good %ld, bad %ld, lost %ld\n", name, (int)r.tps, r.fftN, (int)r.indepth, (int)r.dvbh, (int)r.ts, (int)r.fec, r.good, r.bad, r.lost);
    check(r.tps, name, "TPS decoded");
    check(r.fftN == dvbt::fftN(p.mode), name, "FFT size found");
    check(r.indepth == p.indepth && r.dvbh == p.dvbh && r.ts == p.timeSlicing && r.fec == p.mpeFec, name, "DVB-H TPS flags read back");
    check(r.good >= minGood, name, "packets flowing");
    check(r.bad * 100 <= r.good && r.lost * 100 <= r.good, name, "no damaged or missing packets once locked");
}

int main() {
    testTables();
    testjobs::Jobs jobs;
    int caseNo = 0;
    auto add = [&](std::function<void()> f) { const char* o = getenv("DVBH_CASE"); if (!o || atoi(o) == caseNo) jobs.add(std::move(f)); caseNo++; };
    add([] {
        dvbt::Params p; p.mode = dvbt::k4K; p.guard = dvbt::kGi8; p.mod = dvbt::k16Qam; p.crHp = dvbt::kR12; p.dvbh = true; p.timeSlicing = true; p.mpeFec = true; p.cellId = 77;
        Channel ch; ch.inRate = 10e6; ch.ppm = 20; ch.cfoHz = 3 * 2232.143 + 410; ch.snrDb = 22;
        expect("4K native 16-QAM 1/2, 10 Msps, +20 ppm, 3.2 carriers off, 22 dB", p, run(p, ch, 2.0), 2000);
    });
    add([] {
        dvbt::Params p; p.mode = dvbt::k4K; p.guard = dvbt::kGi4; p.mod = dvbt::kQpsk; p.crHp = dvbt::kR23; p.indepth = true; p.dvbh = true; p.timeSlicing = true;
        Channel ch; ch.inRate = 9.142857e6; ch.ppm = -30; ch.cfoHz = -5100; ch.snrDb = 14;
        expect("4K in-depth QPSK 2/3, -30 ppm, -5.1 kHz, 14 dB", p, run(p, ch, 2.0), 1000);
    });
    add([] {
        dvbt::Params p; p.mode = dvbt::k2K; p.guard = dvbt::kGi16; p.mod = dvbt::k16Qam; p.crHp = dvbt::kR23; p.indepth = true; p.dvbh = true; p.mpeFec = true;
        Channel ch; ch.inRate = 12e6; ch.ppm = 10; ch.cfoHz = 1234; ch.snrDb = 22;
        expect("2K in-depth 16-QAM 2/3, 12 Msps, +1.2 kHz, 22 dB", p, run(p, ch, 2.0), 2000);
    });
    add([] {
        dvbt::Params p; p.mode = dvbt::k2K; p.guard = dvbt::kGi32; p.mod = dvbt::k64Qam; p.hier = 2; p.crHp = dvbt::kR12; p.crLp = dvbt::kR23; p.indepth = true; p.dvbh = true; p.timeSlicing = true;
        Channel ch; ch.cfoHz = -700; ch.snrDb = 28;
        expect("2K in-depth hierarchical alpha 2 (HP stream)", p, run(p, ch, 2.0), 1000);
    });
    add([] {
        dvbt::Params p; p.mode = dvbt::k8K; p.guard = dvbt::kGi4; p.mod = dvbt::k16Qam; p.crHp = dvbt::kR12; p.dvbh = true; p.mpeFec = true; p.cellId = 5;
        Channel ch; ch.inRate = 10e6; ch.cfoHz = 2900; ch.snrDb = 20; ch.ppm = 5;
        expect("8K with DVB-H signalling, 7 MHz raster", p, run(p, ch, 2.5, 7), 1500);
    });
    // all three modes x native / in-depth, QPSK 1/2, short runs
    for (int mode = 0; mode < 3; mode++)
        for (int indepth = 0; indepth < 2; indepth++) {
            if (indepth && mode == dvbt::k8K) continue;
            add([mode, indepth] {
                dvbt::Params p; p.mode = mode; p.guard = dvbt::kGi8; p.mod = dvbt::kQpsk; p.crHp = dvbt::kR12; p.indepth = indepth; p.dvbh = true;
                Channel ch; ch.cfoHz = 600; ch.snrDb = 18;
                char nm[64]; snprintf(nm, sizeof nm, "%s %s", mode == 0 ? "2K" : mode == 1 ? "8K" : "4K", indepth ? "in-depth" : "native");
                expect(nm, p, run(p, ch, 1.5), 300);
            });
        }
    jobs.run();
    jprintf(fails ? "dvbh: FAILED\n" : "dvbh: ok\n");
    return fails ? 1 : 0;
}
