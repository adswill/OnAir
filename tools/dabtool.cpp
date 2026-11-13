// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// DAB test signal tool: writes the built-in DAB / DAB+ ensemble as a .cs8 file (signed 8 bit I/Q like a HackRF), runs a .cs8 file through the
// DAB receiver, and lists the ensemble that the generator transmits.
//   dabtool gen <out.cs8> <seconds> [sampleRate=2048000] [snrDb=30] [cfoHz=0] [sroPpm=0]
//   dabtool rx <in.cs8> <sampleRate> [subChannel]
//   dabtool info
#include "dect2/dab/dab.h"
#include "dect2/dab/dab_gen.h"
#include "dect2/dab/dab_fig.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

using namespace dect2;

static int usage() {
    fprintf(stderr,
            "usage: dabtool gen <out.cs8> <seconds> [sampleRate=2048000] [snrDb=30] [cfoHz=0] [sroPpm=0]\n"
            "       dabtool rx <in.cs8> <sampleRate> [subChannel]\n"
            "       dabtool info\n");
    return 2;
}

static int gen(int argc, char** argv) {
    if (argc < 4) return usage();
    SynthConfig sc;
    const double secs = atof(argv[3]);
    const double rate = argc > 4 ? atof(argv[4]) : 2048000.0;
    sc.snrDb = argc > 5 ? atof(argv[5]) : 30;
    sc.cfoHz = argc > 6 ? atof(argv[6]) : 0;
    sc.sroPpm = argc > 7 ? atof(argv[7]) : 0;
    auto syn = makeDabSynth(sc, rate);
    if (!syn) { fprintf(stderr, "sample rate not supported (2 to 21 Msps)\n"); return 1; }
    FILE* f = fopen(argv[2], "wb");
    if (!f) { perror(argv[2]); return 1; }
    const size_t total = (size_t)(secs * rate), chunk = 65536;
    std::vector<cf32> buf(chunk);
    std::vector<int8_t> q(2 * chunk);
    size_t done = 0;
    const auto t0 = std::chrono::steady_clock::now();
    while (done < total) {
        const size_t n = std::min(chunk, total - done);
        syn->generate(buf.data(), n);
        for (size_t i = 0; i < n; i++) {
            q[2 * i] = (int8_t)std::max(-127.f, std::min(127.f, std::round(buf[i].real() * 127.f)));
            q[2 * i + 1] = (int8_t)std::max(-127.f, std::min(127.f, std::round(buf[i].imag() * 127.f)));
        }
        fwrite(q.data(), 1, 2 * n, f);
        done += n;
    }
    fclose(f);
    const double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    printf("wrote %zu samples (%.1f s of signal) in %.2f s: %.1fx real time\n", total, secs, el, secs / el);
    return 0;
}

static int rx(int argc, char** argv) {
    if (argc < 4) return usage();
    FILE* f = fopen(argv[2], "rb");
    if (!f) { perror(argv[2]); return 1; }
    DabReceiver r;
    r.configure(atof(argv[3]));
    DabAudio& a = r.audio();
    a.setSilent(true);
    if (argc > 4) r.select(atoi(argv[4]));
    std::vector<int8_t> q(2 * 65536);
    std::vector<cf32> x(65536);
    size_t n, total = 0;
    const char* mx = getenv("DABTOOL_MAXSEC");   // stop after this many seconds of the file
    const size_t maxSamples = mx ? (size_t)(atof(mx) * atof(argv[3])) : (size_t)-1;
    while (total < maxSamples && (n = fread(q.data(), 2, 65536, f)) > 0) {
        total += n;
        for (size_t i = 0; i < n; i++) x[i] = cf32(q[2 * i] / 127.f, q[2 * i + 1] / 127.f);
        r.feed(x.data(), n);
    }
    fclose(f);
    DabTelemetry t;
    r.telemetry(t, 0);
    printf("state %d, frames %llu, FIB ok %llu bad %llu, CFO %+.0f Hz, SNR %.1f dB, cir %.0f\n", t.state, (unsigned long long)t.frames,
           (unsigned long long)t.fibOk, (unsigned long long)t.fibBad, t.cfoHz, t.snrDb, t.cirPeak);
    const DabEnsemble e = r.ensemble();
    printf("ensemble \"%s\" id %04X, %zu services, utc %lld\n", e.label.c_str(), e.eid, e.services.size(), (long long)e.utc);
    for (const auto& kv : e.services) {
        const DabComponent* c = kv.second.audio();
        printf("  %04X \"%s\" sub %d %s\n", (unsigned)kv.first, kv.second.label.c_str(), c ? c->subId : -1, kv.second.dabPlus() ? "DAB+" : "DAB");
    }
    const DabFicInfo fi = dabFicInfo(r.ficAnalysis());
    for (const auto& kv : e.services) {
        for (const DabComponent& c : kv.second.comps) printf("  %04X comp tmid %d sub %d scid %d dscty %d%s\n", (unsigned)kv.first, c.tmid, c.subId, c.scid, c.dscty, c.primary ? " primary" : "");
        auto ua = fi.userApps.find(kv.first);
        if (ua != fi.userApps.end()) for (int t : ua->second) printf("  %04X user app 0x%03X %s\n", (unsigned)kv.first, t, dabUserAppName(t).c_str());
    }
    for (const auto& kv : e.packet) printf("  packet scid %d: sub %d address %d dscty %d dg %d\n", kv.first, kv.second.subId, kv.second.address, kv.second.dscty, kv.second.dg);
    if (fi.hasTime) printf("  FIG 0/10 MJD %d %02d:%02d, LTO %s%d half hours\n", fi.mjd, fi.hour, fi.minute, fi.hasLto ? "" : "(none) ", fi.ltoHalfHours);
    for (const auto& kv : e.subs) printf("  sub %d: start %d size %d CU, %d kbit/s, EEP %d-%c\n", kv.first, kv.second.start, kv.second.size, kv.second.bitrate, kv.second.level + 1, kv.second.option ? 'B' : 'A');
    if (getenv("DABTOOL_FIC")) {   // everything the FIC said beyond the tables, and the TII the null symbols gave
        const DabFicAnalysis fa = r.ficAnalysis();
        const DabFicInfo fi = dabFicInfo(fa);
        printf("TII (%d null symbols):", t.tiiFrames);
        for (const auto& x : t.tii) printf(" %d/%d(%.1f dB)", x.mainId, x.subId, x.levelDb);
        printf("\nECC %s%02X, LTO %s%+.1f h, table %d, %zu freq lists, %zu main ids, %zu other-ensemble entries\n", fi.hasEcc ? "" : "none ", fi.ecc, fi.hasLto ? "" : "none ", fi.ltoHalfHours / 2.0, fi.intlTable,
               fi.freqs.size(), fi.transmitters.size(), fi.otherEnsembles.size());
        if (atoi(getenv("DABTOOL_FIC")) > 1) printf("%s\n", dabFicText(fa).c_str());
    }
    const DabAudioStats s = a.stats();
    if (s.sub >= 0) printf("audio sub %d: %s %d Hz %d ch, superframes ok %llu bad %llu, AU ok %llu bad %llu, RS fixed %llu, PCM frames %llu\n", s.sub, s.codec.c_str(), s.sampleRate, s.channels,
                           (unsigned long long)s.superframesOk, (unsigned long long)s.superframesBad, (unsigned long long)s.auOk, (unsigned long long)s.auBad,
                           (unsigned long long)s.rsCorrected, (unsigned long long)s.pcmFrames);
    return 0;
}

// dabtool spi <in.cs8> <sampleRate> [dumpDir]: decodes the programme guide (SPI) of the ensemble in the file and prints what it holds
static int spiCmd(int argc, char** argv) {
    if (argc < 4) return usage();
    FILE* f = fopen(argv[2], "rb");
    if (!f) { perror(argv[2]); return 1; }
    DabReceiver r;
    const double rate = atof(argv[3]);
    r.configure(rate);
    r.audio().setSilent(true);
    const char* dump = argc > 4 ? argv[4] : nullptr;
    int nobj = 0;
    if (const char* fr = getenv("DABTOOL_FRAMES")) {
        FILE* ff = fopen(fr, "wb");
        r.setFrameTap([ff](int, const uint8_t* b, int n) { if (ff) fwrite(b, 1, (size_t)n, ff); });
    }
    r.spi().setObjectTap([&](const MotObject& o) {
        nobj++;
        if (nobj <= 6 || nobj % 50 == 0) printf("  object %d: tid %u name \"%s\" type %d/%d size %zu params %zu%s\n", nobj, o.tid, o.name.c_str(), o.contentType, o.subtype, o.body.size(), o.params.size(), o.fromDirectory ? " (dir)" : "");
        if (dump) { char b[512]; snprintf(b, sizeof b, "%s/obj%04d.bin", dump, nobj); FILE* g = fopen(b, "wb"); if (g) { fwrite(o.body.data(), 1, o.body.size(), g); fclose(g); } }
    });
    std::vector<int8_t> q(2 * 65536);
    std::vector<cf32> x(65536);
    size_t n, total = 0;
    const char* mx = getenv("DABTOOL_MAXSEC");
    const size_t maxSamples = mx ? (size_t)(atof(mx) * rate) : (size_t)-1;
    bool sel = false;
    double nextReport = 10;
    while (total < maxSamples && (n = fread(q.data(), 2, 65536, f)) > 0) {
        total += n;
        for (size_t i = 0; i < n; i++) x[i] = cf32(q[2 * i] / 127.f, q[2 * i + 1] / 127.f);
        r.feed(x.data(), n);
        if (!sel) {
            const DabEnsemble e = r.ensemble();
            for (const auto& kv : e.services) {
                if (!kv.second.spi) continue;
                for (const DabComponent& c : kv.second.comps) {
                    auto pk = c.tmid == 3 ? e.packet.find(c.scid) : e.packet.end();
                    if (pk != e.packet.end() && e.subs.count(pk->second.subId)) { r.selectSpi(pk->second.subId, pk->second.address); sel = true; printf("SPI service %08X: sub %d address %d, profiles", (unsigned)kv.first, pk->second.subId, pk->second.address); for (int p : kv.second.spiProfiles) printf(" %d", p); printf("\n"); }
                }
            }
        }
        if (total / rate >= nextReport) {
            nextReport += 10;
            const DabSpiDecoder::Progress p = r.spi().progress();
            const SpiStats st = r.spi().store().stats();
            printf("t=%.0fs: groups %llu directory %d have %d (%.0f%%), objects %llu (bad %llu) SI %llu PI %llu events %llu\n", total / rate, (unsigned long long)p.groups, p.expected, p.have, p.percent,
                   (unsigned long long)st.objects, (unsigned long long)st.objectsBad, (unsigned long long)st.siObjects, (unsigned long long)st.piObjects, (unsigned long long)st.events);
        }
    }
    fclose(f);
    const SpiSnapshot s = r.spi().store().snapshot();
    const SpiStats st = r.spi().store().stats();
    printf("final: objects %llu (bad %llu, binary %llu, xml %llu, gzip %llu), SI %llu PI %llu, services %zu, programmes %zu, events %zu\n", (unsigned long long)st.objects, (unsigned long long)st.objectsBad,
           (unsigned long long)st.binary, (unsigned long long)st.xml, (unsigned long long)st.compressed, (unsigned long long)st.siObjects, (unsigned long long)st.piObjects, s.services.size(), s.programmes.size(), s.events.size());
    for (const auto& kv : s.services) printf("  svc %08X \"%s\" / \"%s\" media %zu\n", (unsigned)kv.first, kv.second.mediumName.c_str(), kv.second.longName.c_str(), kv.second.media.size());
    int shown = 0;
    for (const SpiEvent& e : s.events) {
        if (shown++ >= 12) break;
        auto p = s.programmes.find(e.progKey);
        printf("  %08X %s +%ds \"%s\"\n", (unsigned)e.sid, spiFormatTime(e.start, e.offMin).c_str(), e.duration, p != s.programmes.end() ? p->second.title().c_str() : "?");
    }
    for (const MotObject& m : r.spi().directoryEntries()) {
        std::string ps;
        for (const MotParam& p : m.params) { char b[24]; snprintf(b, sizeof b, " %02X[", p.id); ps += b; for (size_t i = 0; i < p.data.size() && i < 8; i++) { snprintf(b, sizeof b, "%02X", p.data[i]); ps += b; } ps += "]"; }
        printf("  dir tid %5u name \"%s\" type %d/%d body %zu%s\n", m.tid, m.name.c_str(), m.contentType, m.subtype, m.bodySize, ps.c_str());
    }
    const DabReceiver::SpiLink lk = r.spiLink();
    printf("packets: good %llu bad %llu lost %llu orphan %llu other-id %llu empty %llu; data units ok %llu dropped %llu\n", (unsigned long long)lk.packets, (unsigned long long)lk.bad, (unsigned long long)lk.lost, (unsigned long long)lk.orphan,
           (unsigned long long)lk.other, (unsigned long long)lk.empty, (unsigned long long)lk.unitsOk, (unsigned long long)lk.unitsDropped);
    const MotStats ms = r.spi().motStats();
    printf("mot: repeats %llu groups %llu bad %llu directories %llu dir segs %llu body segs %llu objects %llu\n", (unsigned long long)ms.repeats, (unsigned long long)ms.groups, (unsigned long long)ms.groupsBad, (unsigned long long)ms.directories, (unsigned long long)ms.directorySegments, (unsigned long long)ms.bodySegments, (unsigned long long)ms.objects);
    return 0;
}

static int info() {
    dabgen::TxConfig c;
    dabgen::Transmitter tx(c);
    printf("ensemble \"%s\" id %04X\n", tx.config().ensembleLabel.c_str(), tx.config().eid);
    for (size_t i = 0; i < tx.layout().size(); i++) {
        const auto& l = tx.layout()[i];
        const auto& s = tx.config().services[i];
        printf("  %04X \"%s\": sub %d, CU %d..%d, %d kbit/s EEP %d-%c, %s %d Hz\n", (unsigned)s.sid, s.label.c_str(), l.subId, l.start, l.start + l.size - 1, l.bitrate, l.level + 1,
               l.option ? 'B' : 'A', s.dabPlus ? "DAB+ AAC-LC" : "DAB MP2", s.sampleRate);
    }
    return 0;
}

int main(int argc, char** argv) {
    if (argc < 2) return usage();
    if (!strcmp(argv[1], "gen")) return gen(argc, argv);
    if (!strcmp(argv[1], "rx")) return rx(argc, argv);
    if (!strcmp(argv[1], "spi")) return spiCmd(argc, argv);
    if (!strcmp(argv[1], "info")) return info();
    return usage();
}
