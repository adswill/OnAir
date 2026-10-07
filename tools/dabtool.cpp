// DAB test signal tool: writes the built-in DAB / DAB+ ensemble as a .cs8 file (signed 8 bit I/Q like a HackRF), runs a .cs8 file through the
// DAB receiver, and lists the ensemble that the generator transmits.
//   dabtool gen <out.cs8> <seconds> [sampleRate=2048000] [snrDb=30] [cfoHz=0] [sroPpm=0]
//   dabtool rx <in.cs8> <sampleRate> [subChannel]
//   dabtool info
#include "dect2/dab.h"
#include "dect2/dab_gen.h"
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
    size_t n;
    while ((n = fread(q.data(), 2, 65536, f)) > 0) {
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
    for (const auto& kv : e.subs) printf("  sub %d: start %d size %d CU, %d kbit/s, EEP %d-%c\n", kv.first, kv.second.start, kv.second.size, kv.second.bitrate, kv.second.level + 1, kv.second.option ? 'B' : 'A');
    const DabAudioStats s = a.stats();
    if (s.sub >= 0) printf("audio sub %d: %s %d Hz %d ch, superframes ok %llu bad %llu, AU ok %llu bad %llu, RS fixed %llu, PCM frames %llu\n", s.sub, s.codec.c_str(), s.sampleRate, s.channels,
                           (unsigned long long)s.superframesOk, (unsigned long long)s.superframesBad, (unsigned long long)s.auOk, (unsigned long long)s.auBad,
                           (unsigned long long)s.rsCorrected, (unsigned long long)s.pcmFrames);
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
    if (!strcmp(argv[1], "info")) return info();
    return usage();
}
