// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Project Horus loopback: the generator into the receiver with noise, a frequency offset and a symbol-rate error, for Horus Binary
// v2 and v1 and RTTY at 50 / 100 / 300 baud; prints the share of packets decoded against the SNR (in 3 kHz) and checks the floor
// at the SNRs where the modes must work. "--sweep" prints a finer table. Sound-card rate (48 kHz) so the test stays quick.
#include "dect2/horus/horus_gen.h"
#include "dect2/horus/horus_rx.h"
#include "../common/test_util.h"
#include <cmath>
#include <cstring>
#include <set>
#include <string>
using namespace dect2;
using namespace dect2::horus;

namespace {
// seconds per packet as the generator counts them for the simulated flight
double sppFor(int what) {
    if (what <= 1) return (8 + frameBytes(what == 1 ? kV1Bytes : kV2Bytes)) * 4.0 / 100 + 1.0;
    const int baud = what == 3 ? 300 : what == 4 ? 50 : 100, bits = what == 3 ? 8 : 7;
    return 75.0 * (bits + 3) / baud + 0.8;
}
// how many packets of a kind end inside T seconds (the generator's timing)
int expected(int what, double T) {
    if (what <= 1) {
        const double per = (8 + frameBytes(what == 1 ? kV1Bytes : kV2Bytes)) * 4.0 / 100 + 1.0;
        const double air = per - 1.0;
        return T < air ? 0 : (int)std::floor((T - air) / per) + 1;
    }
    const int baud = what == 3 ? 300 : what == 4 ? 50 : 100, bits = what == 3 ? 8 : 7;
    const double spp = 75.0 * (bits + 3) / baud + 0.8;
    double t = 0; int k = 0;
    for (;; k++) {
        const std::string s = buildSentence(simPacket(0, k, spp)) + "\n";
        const double end = t + 0.5 + s.size() * (bits + 3) / (double)baud;
        if (end > T) break;
        t = end + 0.3;
    }
    return k;
}

struct Run { int got = 0, want = 0, wrong = 0; float snr = 0; };

Run run(int what, double snrDb, double cfo, double ppm, bool mirror, double seconds, unsigned seed, double rate = 48000) {
    SynthConfig sc;
    sc.snrDb = snrDb; sc.cfoHz = cfo; sc.modeOpt[0] = what; sc.modeOpt[2] = mirror ? 1 : 0; sc.modeOpt[3] = (int)seed; sc.modeVal[0] = ppm;
    auto g = makeHorusSynth(sc, rate);
    HorusReceiver rx;
    rx.configure(rate);
    rx.setSignalOffset(synthDialHz(rate));
    rx.setWideSearch(false);
    std::vector<cf32> buf(4800);
    const size_t total = (size_t)(seconds * rate);
    for (size_t done = 0; done < total; done += buf.size()) { g->generate(buf.data(), buf.size()); rx.feed(buf.data(), buf.size()); }
    HorusTelemetry t;
    rx.telemetry(t, 0);
    Run r;
    std::set<uint32_t> seqs;
    float snrSum = 0;
    const int kind = what <= 1 ? (what == 1 ? 1 : 2) : 0;
    for (const auto& p : t.packets) {
        const Packet ref = simPacket(kind, (int)p.seq - 100, sppFor(what));
        const bool ok = p.kind == kind && p.seq >= 100 && std::fabs(p.lat - ref.lat) < 0.01 && std::fabs(p.lon - ref.lon) < 0.01 && p.callsign == ref.callsign;
        if (!ok) { r.wrong++; continue; }
        if (seqs.insert(p.seq).second) snrSum += p.snrDb;
    }
    r.got = (int)seqs.size(); r.want = expected(what, seconds);
    r.snr = r.got ? snrSum / r.got : 0;
    return r;
}
const char* name(int w) { static const char* n[] = {"Binary v2", "Binary v1", "RTTY 100 7N2", "RTTY 300 8N2", "RTTY 50 7N2"}; return n[w]; }
} // namespace

int main(int argc, char** argv) {
    const bool sweep = argc > 1 && !std::strcmp(argv[1], "--sweep");
    // decode share against SNR, with 1.2 kHz frequency error and 1000 ppm clock error
    const std::vector<double> snrs = sweep ? std::vector<double>{-7, -6, -5, -4, -3, -2, -1, 0, 1, 2, 3, 4, 6, 10, 20} : std::vector<double>{-6, -3, 0, 3, 6, 10, 20};
    printf("decoded packets (%%) against SNR in 3 kHz, offset +1.2 kHz, clock +1000 ppm\n%-14s", "");
    for (double s : snrs) printf("%7.0f", s);
    printf("\n");
    for (int w = 0; w < 5; w++) {
        printf("%-14s", name(w));
        for (double s : snrs) {
            if (!sweep && s < 0 && w >= 2) { printf("%7s", "-"); continue; }
            const double secs = sweep ? 200 : w == 4 ? 60 : 40;
            const Run r = run(w, s, 1200, 1000, false, secs, 3 + (unsigned)w);
            printf("%6.0f%%", r.want ? 100.0 * r.got / r.want : 0.0);
            if (sweep && r.got) printf("(%+.0f)", r.snr);
            fflush(stdout);
            CHECK(r.wrong == 0, "%s at %.0f dB: %d packets with wrong content passed the CRC", name(w), s, r.wrong);
            // where each mode must work (measured on this generator; the margins leave room for the noise draw)
            // (measured: binary about 50 % at -7 dB and 92 % at -6 dB; RTTY 50 % at about -4.5 / -1.5 / +3.5 dB for 50 / 100 / 300 baud)
            if (w <= 1 && s >= -3) CHECK(r.got >= r.want - 1, "%s at %.0f dB: %d of %d", name(w), s, r.got, r.want);
            if ((w == 2 || w == 4) && s >= 3) CHECK(r.got >= r.want - 1, "%s at %.0f dB: %d of %d", name(w), s, r.got, r.want);
            if (w == 3 && s >= 10) CHECK(r.got >= r.want - 1, "%s at %.0f dB: %d of %d", name(w), s, r.got, r.want);
            if (w <= 1 && s == 20) CHECK(std::fabs(r.snr - 20) < 6, "%s SNR estimate %.1f dB at 20 dB", name(w), r.snr);
        }
        printf("\n");
    }
    // impairments: a mirrored spectrum (LSB), a far offset, a large clock error, another tone spacing
    {
        const Run m = run(0, 10, -800, 0, true, 30, 21);
        CHECK(m.got >= m.want - 1 && m.want >= 6, "v2 mirrored: %d of %d", m.got, m.want);
        const Run o = run(0, 10, 3000, 0, false, 30, 22);
        CHECK(o.got >= o.want - 1, "v2 at +3 kHz: %d of %d", o.got, o.want);
        const Run c = run(1, 10, 0, -3000, false, 30, 23);
        CHECK(c.got >= c.want - 1, "v1 at -3000 ppm: %d of %d", c.got, c.want);
        const Run r = run(3, 15, 500, 2000, true, 30, 24);
        CHECK(r.got >= r.want - 1, "RTTY 300 mirrored, +2000 ppm: %d of %d", r.got, r.want);
    }
    // noise only: nothing decodes
    {
        SynthConfig sc; sc.snrDb = -40; sc.modeOpt[3] = 5;
        auto g = makeHorusSynth(sc, 48000);
        HorusReceiver rx; rx.configure(48000); rx.setSignalOffset(0);
        std::vector<cf32> buf(4800);
        for (int i = 0; i < 300; i++) { g->generate(buf.data(), buf.size()); rx.feed(buf.data(), buf.size()); }
        HorusTelemetry t; rx.telemetry(t, 0);
        CHECK(t.packets.empty(), "noise: %zu packets", t.packets.size());
    }
    return testSummary("test_horus_loopback");
}
