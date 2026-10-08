// Inmarsat-C command line tool: write the test signal to a file, or decode a recording.
//   inmctool gen <secs> <file.cs8|.cf32> [--rate sps] [--ebn0 dB] [--cfo Hz] [--drift Hz/s] [--sro ppm] [--seed n] [--invert] [--noiseless] [--offset Hz]
//   inmctool rx <file> --rate <sps> [--format cs8|cu8|cf32] [--offset Hz]
// The channel is expected at --offset Hz from the centre of the recording (default -50000, as the engine tunes the radio 50 kHz above the channel).
#include "dect2/inmc_gen.h"
#include "dect2/inmc_rx.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace dect2;

static bool endsWith(const std::string& s, const char* e) { const size_t n = strlen(e); return s.size() >= n && s.compare(s.size() - n, n, e) == 0; }

static double gNow = 0;     // seconds of signal fed so far, for the log lines

static void usage() {
    printf("usage: inmctool gen <secs> <file.cs8|.cf32> [--rate sps] [--ebn0 dB] [--cfo Hz] [--drift Hz/s] [--sro ppm] [--seed n] [--invert] [--noiseless] [--offset Hz]\n"
           "       inmctool rx <file> --rate <sps> [--format cs8|cu8|cf32] [--offset Hz]\n");
}

static int cmdGen(int argc, char** argv) {
    if (argc < 4) { usage(); return 1; }
    const double secs = atof(argv[2]);
    const std::string file = argv[3];
    InmcGenConfig c;
    c.rate = 2e6;
    for (int i = 4; i < argc; i++) {
        const std::string a = argv[i];
        auto val = [&](double& v) { if (i + 1 < argc) v = atof(argv[++i]); };
        if (a == "--rate") val(c.rate);
        else if (a == "--ebn0") val(c.ebn0Db);
        else if (a == "--cfo") val(c.cfoHz);
        else if (a == "--drift") val(c.driftHzS);
        else if (a == "--sro") val(c.sroPpm);
        else if (a == "--offset") val(c.offsetHz);
        else if (a == "--seed") { double v = 1; val(v); c.seed = (uint32_t)v; }
        else if (a == "--invert") c.invert = true;
        else if (a == "--noiseless") c.noiseless = true;
    }
    auto g = makeInmcGenerator(c);
    if (!g) { printf("rate too low\n"); return 1; }
    FILE* f = fopen(file.c_str(), "wb");
    if (!f) { printf("cannot write %s\n", file.c_str()); return 1; }
    const bool cs8 = endsWith(file, ".cs8");
    const size_t total = (size_t)(secs * c.rate);
    std::vector<cf32> buf(65536);
    std::vector<int8_t> b8(buf.size() * 2);
    for (size_t done = 0; done < total;) {
        const size_t n = std::min(buf.size(), total - done);
        g->generate(buf.data(), n);
        if (cs8) {
            for (size_t k = 0; k < n; k++) {
                b8[2 * k] = (int8_t)std::max(-127.f, std::min(127.f, std::round(buf[k].real() * 127.f)));
                b8[2 * k + 1] = (int8_t)std::max(-127.f, std::min(127.f, std::round(buf[k].imag() * 127.f)));
            }
            fwrite(b8.data(), 1, n * 2, f);
        } else fwrite(buf.data(), sizeof(cf32), n, f);
        done += n;
    }
    fclose(f);
    printf("wrote %.1f s at %.0f sps (%s), Eb/N0 %.1f dB\n", secs, c.rate, cs8 ? "cs8" : "cf32", c.ebn0Db);
    return 0;
}

static int cmdRx(int argc, char** argv) {
    if (argc < 3) { usage(); return 1; }
    const std::string file = argv[2];
    double rate = 0, offset = -50000;
    std::string fmt = endsWith(file, ".cs8") ? "cs8" : endsWith(file, ".cu8") ? "cu8" : "cf32";
    for (int i = 3; i < argc; i++) {
        const std::string a = argv[i];
        if (a == "--rate" && i + 1 < argc) rate = atof(argv[++i]);
        else if (a == "--format" && i + 1 < argc) fmt = argv[++i];
        else if (a == "--offset" && i + 1 < argc) offset = atof(argv[++i]);
    }
    if (rate <= 0) { usage(); return 1; }
    FILE* f = fopen(file.c_str(), "rb");
    if (!f) { printf("cannot read %s\n", file.c_str()); return 1; }
    InmcReceiver rx;
    rx.configure(rate);
    rx.setSignalOffset(offset);
    rx.setLogCallback([](const std::string& s) { printf("[%6.1f s] %s\n", gNow, s.c_str()); });
    if (!rx.ready()) { printf("sample rate too low\n"); return 1; }
    const size_t bps = fmt == "cf32" ? 8 : 2;
    std::vector<uint8_t> raw(65536 * bps);
    std::vector<cf32> x(65536);
    size_t samples = 0;
    for (;;) {
        const size_t got = fread(raw.data(), bps, 65536, f);
        if (!got) break;
        for (size_t k = 0; k < got; k++) {
            if (fmt == "cs8") x[k] = cf32(((int8_t)raw[2 * k]) / 128.f, ((int8_t)raw[2 * k + 1]) / 128.f);
            else if (fmt == "cu8") x[k] = cf32((raw[2 * k] - 127.5f) / 128.f, (raw[2 * k + 1] - 127.5f) / 128.f);
            else { float v[2]; memcpy(v, &raw[8 * k], 8); x[k] = cf32(v[0], v[1]); }
        }
        samples += got;
        gNow = samples / rate;
        rx.feed(x.data(), got);
    }
    fclose(f);
    InmcTelemetry t;
    if (!rx.telemetry(t, 0)) { printf("no report\n"); return 1; }
    printf("%.1f s: %s\n", samples / rate, inmcSummary(t).c_str());
    printf("carrier offset %+.1f Hz, drift %+.2f Hz/s, Es/N0 %.1f dB (Eb/N0 %.1f dB)\n", t.cfoHz, t.driftHzS, t.esn0Db, t.ebn0Db);
    printf("frames found %llu, bulletin boards ok %llu, not decoded %llu, UW errors last %d (avg %.1f), channel symbol errors %.2f %%\n",
           (unsigned long long)t.framesFound, (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad, t.uwErrors, t.uwErrorsAvg, 100.0 * t.symbolErrorRate);
    printf("packets ok %llu, bad %llu\n", (unsigned long long)t.packetsOk, (unsigned long long)t.packetsBad);
    if (t.ncs.valid)
        printf("NCS: %s, station %d %s, %s, frame %u (%s), status: %s\n  services: %s\n", t.ncs.region.c_str(), t.ncs.lesId, t.ncs.lesName.c_str(),
               t.ncs.channelTypeName.c_str(), t.ncs.frameNo, t.ncs.frameTime.c_str(), t.ncs.statusText.c_str(), t.ncs.servicesText.c_str());
    for (auto it = t.messages.rbegin(); it != t.messages.rend(); ++it) {
        const InmcMessage& m = *it;
        printf("\n[%s] id %u, %s, priority %d, %d packets, %s, seen %u, addr0 %02X (tentative: sat %d station %d %s), area bytes %s\n%s\n", m.rxTime.c_str(),
               m.id, m.serviceText.c_str(), m.priority, m.packets, m.complete ? "complete" : "partial", m.seen, m.addr0, m.sat, m.lesId, m.lesName.c_str(),
               m.area.c_str(), m.text.c_str());
    }
    return 0;
}

int main(int argc, char** argv) {
    if (argc < 2) { usage(); return 1; }
    if (!strcmp(argv[1], "gen")) return cmdGen(argc, argv);
    if (!strcmp(argv[1], "rx")) return cmdRx(argc, argv);
    usage();
    return 1;
}
