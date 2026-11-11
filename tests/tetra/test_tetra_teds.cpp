// TETRA TEDS (3.9): the extended services broadcast, pi/8-D8PSK slots and their signalling, and the QAM carrier identifier, on the test
// signal only (no TEDS recording was available: simulated only).
#include "dect2/tetra/tetra_gen.h"
#include "dect2/cell/cell_dsp.h"
#include "dect2/common/stable_random.h"
#include "dect2/engine/modes.h"
#include "dect2/source/source.h"
#include "dect2/tetra/tetra_phy.h"
#include "dect2/tetra/tetra_rx.h"
#include "dect2/tetra/tetra_tel.h"
#include "dect2/tetra/tetra_teds.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <random>
#include <thread>
using namespace dect2;
using namespace dect2::tetra;
#include "../common/test_util.h"

namespace {

void addNoise(std::vector<cf32>& x, double sigma, uint32_t seed) {
    std::mt19937 g(seed);
    dect2::normal_distribution<float> nd(0.f, 1.f);
    for (auto& v : x) v += cf32((float)sigma * nd(g), (float)sigma * nd(g));
}

// one cell at 72 kHz from its phase steps, with a tuning error, a clock error and noise in 25 kHz
std::vector<cf32> cellAt72k(const TetraCellCfg& c0, double snrDb, double cfoHz, double ppm, uint32_t seed) {
    TetraCellCfg c = c0;
    Modulator mod;
    std::vector<cf32> sym;
    const size_t n = (size_t)(2.3 * kRate);
    for (int k = 0; k < (int)(n / kSlotSamples) + 4; k++) mod.pushSteps(cellSlotSteps(c, c.startSlot + (uint32_t)k), sym);
    std::vector<cf32> x(n);
    cell::Resampler rs(0.45);
    rs.run(sym.data(), sym.size(), rs.halfLen() + 2 + 123.4, 1 + ppm * 1e-6, x.data(), n);
    cell::mixDown(x.data(), n, -cfoHz, kRate);
    if (snrDb < 99) addNoise(x, std::sqrt(kRate / 25e3 / std::pow(10.0, snrDb / 10) / 2), seed);
    return x;
}

// a QAM carrier (QamGen) at fs, offHz from the centre, tuning error cfoHz, SNR in its own bandwidth
std::vector<cf32> qamAt(int bwK, int order, double fs, double offHz, double snrDb, double secs, uint32_t seed) {
    QamGen q(bwK, order, seed);
    std::vector<cf32> b;
    q.push((int)(secs * kQamSymRate) + 40, b);
    const size_t n = (size_t)(secs * fs);
    std::vector<cf32> x(n);
    cell::Resampler rs(std::min(0.5, 0.5 * fs / kQamGenRate) * 0.95);
    rs.run(b.data(), b.size(), rs.halfLen() + 2, kQamGenRate / fs, x.data(), n);
    cell::mixDown(x.data(), n, -offHz, fs);
    if (snrDb < 99) addNoise(x, std::sqrt(fs / (bwK * 1e3) / std::pow(10.0, snrDb / 10) / 2), seed + 1);
    return x;
}

} // namespace

int main() {
    // ---- the extended services broadcast: every section 1 combination and every QAM mask survive encode / parse
    {
        int bad = 0;
        for (int b = 0; b < 128; b++) {
            ExtServices e; e.security = 0xA5; e.sdsTl = 2; e.gck = true; e.section = 0;
            e.dataPriority = b & 64; e.extAdvLinks = b & 32; e.qos = b & 16; e.d8psk = b & 8; e.sec2 = b & 4; e.sec3 = b & 2; e.sec4 = b & 1;
            ExtServices r;
            if (!parseExtServices(3, encodeExtServices(e), r) || r.section != 0 || r.d8psk != e.d8psk || r.qos != e.qos || r.sec2 != e.sec2 ||
                r.sec4 != e.sec4 || r.security != 0xA5 || r.sdsTl != 2 || !r.gck) bad++;
        }
        for (int m = 0; m < 16; m++) {
            ExtServices e; e.section = 1; e.qamMask = m;
            ExtServices r;
            if (!parseExtServices(3, encodeExtServices(e), r) || r.section != 1 || r.qamMask != m) bad++;
        }
        ExtServices r;
        CHECK(!parseExtServices(2, 0x12345, r), "optional field 2 is not the extended services broadcast");
        CHECK(bad == 0, "extended services round trips: %d bad", bad);
        TedsCaps caps;
        ExtServices s1; s1.section = 0; s1.d8psk = true; s1.sec2 = true;
        ExtServices s2; s2.section = 1; s2.qamMask = 5;
        caps.add(s1); CHECK(caps.d8psk == 1 && caps.qamMask == -1, "section 1 alone: QAM still unknown");
        caps.add(s2); CHECK(caps.d8psk == 1 && caps.qamMask == 5, "both sections");
    }
    // ---- pi/8-D8PSK steps as table 5.2
    {
        const int want[8] = {1, 3, -1, -3, 7, 5, -7, -5};   // b1 b2 b3 = 000 001 010 011 100 101 110 111
        int bad = 0;
        for (int v = 0; v < 8; v++) if (tribitPhase(v >> 2, (v >> 1) & 1, v & 1) != want[v]) bad++;
        CHECK(bad == 0, "tribit phases");
        // a pi/8-D8PSK burst: the phase adjustment symbols balance the training sequences as table 9.16 says
        Bits a(324), b(324), bb(30);
        std::mt19937 g(3);
        for (auto& v : a) v = (uint8_t)(g() & 1);
        for (auto& v : b) v = (uint8_t)(g() & 1);
        for (auto& v : bb) v = (uint8_t)(g() & 1);
        const std::vector<int> st = normalBurstP8(a, bb, b, false);
        CHECK(st.size() == 255, "P8 burst symbols %zu", st.size());
        int s1 = 0, s2 = 0;
        for (int k = 7; k <= 122; k++) s1 += st[(size_t)(k - 1)];
        for (int k = 123; k <= 250; k++) s2 += st[(size_t)(k - 1)];
        CHECK(((s1 % 16) + 16) % 16 == 1 && ((s2 % 16) + 16) % 16 == 0, "phase balance %d %d", s1, s2);
        CHECK(std::abs(st[6]) % 2 == 1 && std::abs(st[249]) % 2 == 1, "adjustment symbols are pi/8-D8PSK steps");
    }
    // ---- a TEDS cell (scene 3): pi/8-D8PSK slots found and their SCH-P8 blocks decoded, the capabilities read from SYSINFO, under impairments
    {
        struct Imp { double snr, cfo, ppm; };
        for (const Imp& im : {Imp{99, 0, 0}, Imp{14, 900, 0}, Imp{14, -600, 40}, Imp{10, 300, -25}}) {
            TetraCellCfg c = tetraTestCell(3, 0);
            const std::vector<cf32> x = cellAt72k(c, im.snr, im.cfo, im.ppm, 11);
            const CarrierDecode d = decodeCarrier(x.data(), x.size());
            int d8 = 0, qam = 0;
            for (const auto& m : d.macs) {
                ExtServices e;
                if (m.sysinfo && parseExtServices(m.si.optFlag, m.si.optValue, e)) { if (e.section == 0 && e.d8psk) d8++; if (e.section == 1 && e.qamMask == 3) qam++; }
            }
            int si = 0;
            for (const auto& m : d.macs) if (m.sysinfo && m.si.la == c.si.la && m.si.mainCarrier == c.si.mainCarrier) si++;
            printf("TEDS cell, SNR %4.0f dB, %+5.0f Hz, %+3.0f ppm: synced %d, P8 slots %d, P8 blocks %d ok / %d bad, SYSINFO %d (D8PSK %d, QAM %d)\n",
                   im.snr, im.cfo, im.ppm, d.synced, d.p8Slots, d.p8BlocksOk, d.p8BlocksBad, si, d8, qam);
            CHECK(d.synced, "TEDS cell synced (%.0f dB)", im.snr);
            CHECK(d.p8Slots >= 30, "P8 slots %d (about 37 in a capture)", d.p8Slots);   // timeslot 3 of frames 1 - 17
            CHECK(d.p8BlocksOk >= (im.snr >= 99 ? 2 * d.p8Slots : d.p8Slots), "P8 blocks %d of %d slots", d.p8BlocksOk, d.p8Slots);
            if (im.snr >= 99) CHECK(d.p8BlocksBad == 0 && d.blocksBad == 0, "clean: no bad block (%d P8, %d all)", d.p8BlocksBad, d.blocksBad);
            CHECK(d8 >= 1 && qam >= 1, "extended services sections 1 and 2 read (%d, %d)", d8, qam);
            CHECK(si >= d.p8BlocksOk / 2 - 2, "SYSINFO sent with pi/8-D8PSK parsed: %d", si);
        }
        // a cell without pi/8-D8PSK never shows any
        const std::vector<cf32> x = cellAt72k(tetraTestCell(0, 0), 12, 500, 20, 12);
        const CarrierDecode d = decodeCarrier(x.data(), x.size());
        CHECK(d.synced && d.p8Slots == 0, "no P8 slots on a pi/4-DQPSK cell (%d)", d.p8Slots);
    }
    // ---- the identifier: QAM bandwidths and constellations, a TETRA carrier, a pi/8-D8PSK carrier, noise
    {
        const double fs = 1e6;
        int bad = 0;
        for (int bw : {25, 50, 100, 150})
            for (int order : {4, 16, 64}) {
                const double snr = order == 4 ? 15 : 32;
                std::vector<cf32> x = qamAt(bw, order, fs, 150e3 + 700, snr, 0.4, 100u + (uint32_t)(bw + order));
                const TedsCarrierClass c = classifyCarrier(x.data(), x.size(), fs, 150e3, bw * 1e3);
                const bool ok = c.kind == CarrierKind::Qam && c.subcarriers == bw / 25 * 8 && std::fabs(c.bwHz - bw * 1e3) < 1 &&
                                (order == 4 ? c.qamOrder == 4 : (c.qamOrder == 16 || c.qamOrder == 64)) && (!c.orderSure || c.qamOrder == order);
                printf("QAM %3d kHz %2d-QAM at %2.0f dB: %s %d sub-carriers, %d-QAM%s, score %.2f\n", bw, order, snr, carrierKindText(c.kind), c.subcarriers,
                       c.qamOrder, c.orderSure ? "" : " (not sure)", c.score);
                if (!ok) bad++;
            }
        CHECK(bad == 0, "QAM identification: %d wrong", bad);
        // weak: 4-QAM 25 kHz at 6 dB is still QAM
        {
            std::vector<cf32> x = qamAt(25, 4, fs, -80e3, 6, 0.4, 7);
            const TedsCarrierClass c = classifyCarrier(x.data(), x.size(), fs, -80e3, 25e3);
            CHECK(c.kind == CarrierKind::Qam && c.subcarriers == 8, "weak QAM: %s %d", carrierKindText(c.kind), c.subcarriers);
        }
        // a pi/4-DQPSK TETRA carrier and a carrier sending only pi/8-D8PSK slots
        for (int k = 0; k < 2; k++) {
            TetraCellCfg c = tetraTestCell(0, 0);
            if (k == 1) c.d8pskTs = 0;
            Modulator mod;
            std::vector<cf32> sym;
            for (int s = 0; s < 40; s++) {
                if (k == 0) mod.push(cellSlot(c, (uint32_t)s), sym);
                else {   // every slot in pi/8-D8PSK
                    Bits a(324), b(324), bb(30);
                    std::mt19937 g((uint32_t)s);
                    for (auto& v : a) v = (uint8_t)(g() & 1);
                    for (auto& v : b) v = (uint8_t)(g() & 1);
                    mod.pushSteps(normalBurstP8(a, bb, b, s & 1), sym);
                }
            }
            const size_t n = (size_t)(0.5 * fs);
            std::vector<cf32> x(n);
            cell::Resampler rs(0.45 * kRate / fs * 2 > 0.5 ? 0.45 : 0.45);
            rs.run(sym.data(), sym.size(), rs.halfLen() + 2, kRate / fs, x.data(), std::min(n, (size_t)((double)(sym.size() - 40) * fs / kRate)));
            cell::mixDown(x.data(), n, -(-200e3 + 400), fs);
            addNoise(x, std::sqrt(fs / 25e3 / std::pow(10.0, 20.0 / 10) / 2), 5);
            const TedsCarrierClass cc = classifyCarrier(x.data(), x.size(), fs, -200e3, 25e3);
            printf("%s carrier: %s\n", k == 0 ? "pi/4-DQPSK" : "pi/8-D8PSK", carrierKindText(cc.kind));
            CHECK(cc.kind == (k == 0 ? CarrierKind::Dqpsk : CarrierKind::D8psk), "phase carrier %d: %s", k, carrierKindText(cc.kind));
        }
        // an analogue FM voice channel (constant envelope, +-5 kHz deviation, no symbol clock) is not QAM
        {
            const size_t n = (size_t)(0.4 * fs);
            std::vector<cf32> x(n);
            std::mt19937 g(21);
            dect2::normal_distribution<float> nd(0.f, 1.f);
            double ph = 0, m = 0;
            for (size_t i = 0; i < n; i++) {
                m = 0.995 * m + 0.1 * nd(g);   // a low-passed noise "voice"
                ph += 2 * M_PI * (100e3 + 5e3 * std::max(-2.0, std::min(2.0, m / 2))) / fs;
                x[i] = cf32((float)std::cos(ph), (float)std::sin(ph));
            }
            addNoise(x, std::sqrt(fs / 25e3 / std::pow(10.0, 25.0 / 10) / 2), 22);
            const TedsCarrierClass c = classifyCarrier(x.data(), x.size(), fs, 100e3, 20e3);
            CHECK(c.kind != CarrierKind::Qam, "FM voice called %s", carrierKindText(c.kind));
        }
        std::vector<cf32> nz((size_t)(0.4 * fs), cf32(0, 0));
        addNoise(nz, 0.01, 9);
        CHECK(classifyCarrier(nz.data(), nz.size(), fs, 100e3, 50e3).kind == CarrierKind::Unknown, "noise is not a carrier");
    }
    // ---- the receiver on scene 3 at 300 kHz: the cell with its TEDS capabilities and pi/8-D8PSK slots, the QAM carrier as one 50 kHz entry
    {
        const double rate = 300e3;
        SynthConfig sc; sc.mode = 48; sc.snrDb = 25; sc.modeOpt[0] = 3; sc.cfoHz = 400;
        auto syn = makeTetraSynth(sc, rate);
        TetraReceiver rx;
        rx.configure(rate);
        rx.setCenterHz(kTetraTestCentreHz);
        std::vector<cf32> x(32768);
        for (int i = 0; i < 50; i++) {   // two captures
            syn->generate(x.data(), x.size());
            rx.feed(x.data(), x.size());
            while (rx.busy()) std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        rx.feed(x.data(), 1);
        TetraTelemetry t;
        rx.telemetry(t, 0);
        const TetraCell* cell = t.cells.empty() ? nullptr : &t.cells[0];
        CHECK(cell && cell->tedsD8psk == 1 && cell->tedsQamMask == 3 && cell->p8Slots > 0, "cell TEDS: D8PSK %d QAM mask %d P8 slots %llu",
              cell ? cell->tedsD8psk : -9, cell ? cell->tedsQamMask : -9, cell ? (unsigned long long)cell->p8Slots : 0ull);
        int qam = 0;
        for (const auto& c : t.carriers) {
            printf("carrier %.4f MHz kind %d bw %.0f kHz sub-carriers %d order %d\n", c.freqHz / 1e6, c.kind, c.bwHz / 1e3, c.subcarriers, c.qamOrder);
            if (c.kind == 3 && std::fabs(c.freqHz - (kTetraTestCentreHz + kTetraTedsQamOffHz + 400)) < 3e3 && std::fabs(c.bwHz - 50e3) < 1 && c.subcarriers == 16) qam++;
        }
        CHECK(qam == 1, "the 50 kHz QAM carrier listed once (%d)", qam);
    }
    return testSummary("tetra teds");
}
