// Radiosonde command line tool.
//   sondetool gen <secs> <file.cs8|.cf32> [--rate sps] [--snr dB] [--cfo Hz] [--sro ppm] [--types mask] [--count n] [--seed n]
//                                          [--east m/s] [--drift Hz/min] [--spacing Hz]
//   sondetool rx <file> --rate <sps> [--format cs8|cu8|cf32] [--center MHz] [--chunk n]
#include "dect2/sonde_gen.h"
#include "dect2/sonde_rx.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
using namespace dect2;

static void usage() {
    printf("usage: sondetool gen <secs> <file.cs8|.cf32> [--rate sps] [--snr dB] [--cfo Hz] [--sro ppm] [--types mask] [--count n] [--seed n] [--east m/s] [--drift Hz/min] [--spacing Hz]\n"
           "       sondetool rx <file> --rate <sps> [--format cs8|cu8|cf32] [--center MHz] [--chunk n]\n");
}

static std::string fmtOf(const std::string& path) {
    const size_t d = path.rfind('.');
    return d == std::string::npos ? "cf32" : path.substr(d + 1);
}

static int gen(int argc, char** argv) {
    if (argc < 4) { usage(); return 2; }
    const double secs = atof(argv[2]);
    const std::string path = argv[3];
    double rate = 8e6;
    SynthConfig cfg;
    for (int i = 4; i + 1 < argc; i += 2) {
        const std::string a = argv[i];
        const double v = atof(argv[i + 1]);
        if (a == "--rate") rate = v; else if (a == "--snr") cfg.snrDb = v; else if (a == "--cfo") cfg.cfoHz = v;
        else if (a == "--sro") cfg.sroPpm = v; else if (a == "--types") cfg.modeOpt[0] = (int)v; else if (a == "--count") cfg.modeOpt[1] = (int)v;
        else if (a == "--seed") cfg.modeOpt[2] = (int)v; else if (a == "--east") cfg.modeVal[0] = v; else if (a == "--drift") cfg.modeVal[1] = v;
        else if (a == "--spacing") cfg.modeVal[2] = v;
        else { printf("unknown option %s\n", a.c_str()); return 2; }
    }
    cfg.mode = 15;
    auto syn = makeSondeSynth(cfg, rate);
    if (!syn) { printf("cannot make the signal at %.0f sps\n", rate); return 1; }
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) { perror(path.c_str()); return 1; }
    const std::string fmt = fmtOf(path);
    const size_t total = (size_t)(secs * rate);
    std::vector<cf32> buf(65536);
    std::vector<int8_t> b8(buf.size() * 2);
    for (size_t done = 0; done < total; ) {
        const size_t m = std::min(buf.size(), total - done);
        syn->generate(buf.data(), m);
        if (fmt == "cf32") fwrite(buf.data(), sizeof(cf32), m, f);
        else {
            for (size_t i = 0; i < m; i++) {
                const float re = std::max(-1.f, std::min(1.f, buf[i].real())), im = std::max(-1.f, std::min(1.f, buf[i].imag()));
                if (fmt == "cu8") { b8[2 * i] = (int8_t)(uint8_t)std::lround(127.5f + 127.f * re); b8[2 * i + 1] = (int8_t)(uint8_t)std::lround(127.5f + 127.f * im); }
                else { b8[2 * i] = (int8_t)std::lround(127.f * re); b8[2 * i + 1] = (int8_t)std::lround(127.f * im); }
            }
            fwrite(b8.data(), 1, 2 * m, f);
        }
        done += m;
    }
    fclose(f);
    printf("wrote %.1f s at %.0f sps (%s) to %s\n", secs, rate, fmt.c_str(), path.c_str());
    return 0;
}

static int rx(int argc, char** argv) {
    if (argc < 3) { usage(); return 2; }
    const std::string path = argv[2];
    double rate = 0, center = 0;
    std::string fmt = "cs8";
    size_t chunk = 65536;
    for (int i = 3; i + 1 < argc; i += 2) {
        const std::string a = argv[i];
        if (a == "--rate") rate = atof(argv[i + 1]); else if (a == "--format") fmt = argv[i + 1];
        else if (a == "--center") center = atof(argv[i + 1]); else if (a == "--chunk") chunk = (size_t)atol(argv[i + 1]);
        else { printf("unknown option %s\n", a.c_str()); return 2; }
    }
    if (rate <= 0) { usage(); return 2; }
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) { perror(path.c_str()); return 1; }
    SondeReceiver r;
    r.setSynchronous(true);
    r.configure(rate);
    if (center > 0) r.setCenterMhz(center);
    r.setLogCallback([](const std::string& s) { printf("%s\n", s.c_str()); });
    std::vector<cf32> buf(chunk);
    std::vector<int8_t> b8(chunk * 2);
    uint64_t total = 0, last = 0;
    SondeTelemetry t;
    const auto t0 = std::chrono::steady_clock::now();
    for (;;) {
        size_t got;
        if (fmt == "cf32") got = fread(buf.data(), sizeof(cf32), chunk, f);
        else {
            got = fread(b8.data(), 2, chunk, f);
            for (size_t i = 0; i < got; i++) {
                if (fmt == "cu8") buf[i] = cf32(((uint8_t)b8[2 * i] - 127.5f) / 127.5f, ((uint8_t)b8[2 * i + 1] - 127.5f) / 127.5f);
                else buf[i] = cf32(b8[2 * i] / 127.f, b8[2 * i + 1] / 127.f);
            }
        }
        if (!got) break;
        r.feed(buf.data(), got);
        total += got;
        r.telemetry(t, last);
    }
    fclose(f);
    const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    r.telemetry(t, 0);
    printf("%.1f s of signal in %.2f s (%.1fx real time)\n", (double)total / rate, wall, (double)total / rate / std::max(wall, 1e-9));
    printf("%s\n", sondeSummary(t).c_str());
    printf("frames ok %llu bad %llu, channels %d, state %d\n", (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad, t.channelsUsed, t.state);
    for (const auto& c : t.carriers) printf("  carrier %+9.1f kHz  %5.1f dB  %s\n", c.offsetHz / 1e3, c.levelDb, c.assigned ? "following" : "");
    for (const auto& s : t.sondes) {
        printf("%s %s  %+.1f kHz  snr %.1f dB  frame %d  ok %llu bad %llu  last heard %.1f s ago\n", s.subtype.empty() ? s.type.c_str() : s.subtype.c_str(), s.serial.c_str(),
               s.offsetHz / 1e3, s.snrDb, s.frame, (unsigned long long)s.framesOk, (unsigned long long)s.framesBad, s.lastHeardS);
        if (s.hasPos) printf("    %.5f %.5f  %.0f m", s.lat, s.lon, s.altM);
        if (s.hasVel) printf("  v %.1f m/s  h %.1f m/s  dir %.0f", s.vSpeed, s.hSpeed, s.headingDeg);
        if (s.sats >= 0) printf("  sats %d", s.sats);
        printf("\n");
        if (s.hasTemp) printf("    T %.1f C", s.tempC);
        if (s.hasHumidity) printf("  RH %.0f %%", s.humidity);
        if (s.hasPressure) printf("  P %.1f hPa", s.pressureHpa);
        if (s.batteryV >= 0) printf("  battery %.1f V", s.batteryV);
        if (!s.note.empty()) printf("  [%s]", s.note.c_str());
        if (s.hasTemp || s.hasHumidity || s.batteryV >= 0 || !s.note.empty()) printf("\n");
        printf("    track points %zu\n", s.track.size());
    }
    return 0;
}

int main(int argc, char** argv) {
    if (argc < 2) { usage(); return 2; }
    if (!strcmp(argv[1], "gen")) return gen(argc, argv);
    if (!strcmp(argv[1], "rx")) return rx(argc, argv);
    usage();
    return 2;
}
