// Mesh (LoRa: Meshtastic + MeshCore) command line tool.
//   meshtool gen <secs> <file.cs8|.cf32> [--rate <sps>] [--snr <dB>] [--weak <dB>] [--cfo <Hz>] [--sro <ppm>] [--protocols 1|2|3] [--region 0|1] [--seed <n>]
//       writes the test signal (mesh_gen.h): the user's frequency (Meshtastic LongFast) sits at -300 kHz in the samples
//   meshtool rx <file> --rate <sps> [--format cs8|cu8|cf32] [--offset <Hz>] [--tuned <Hz>] [--region 0|1] [--all-presets]
//       [--key <name>=<base64 PSK>] [--mckey <name>=<secret>] [--secs <s>]
//       runs the receiver over a recording and prints every packet, then the node table and the messages.
//       --offset: where the user's frequency (--tuned, default 869.525 MHz) lies in the recording (default -300000, as gen writes it)
#include "dect2/mesh_gen.h"
#include "dect2/mesh_rx.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>
using namespace dect2;

static void usage() {
    printf("usage: meshtool gen <secs> <file.cs8|.cf32> [--rate <sps>] [--snr <dB>] [--weak <dB>] [--cfo <Hz>] [--sro <ppm>] [--protocols 1|2|3] [--region 0|1] [--seed <n>]\n"
           "       meshtool rx <file> --rate <sps> [--format cs8|cu8|cf32] [--offset <Hz>] [--tuned <Hz>] [--region 0|1] [--all-presets]\n"
           "                       [--key <name>=<base64 PSK>] [--mckey <name>=<secret>] [--secs <s>]\n");
}

static bool endsWith(const std::string& s, const char* e) { const size_t n = strlen(e); return s.size() >= n && s.compare(s.size() - n, n, e) == 0; }

static int gen(int argc, char** argv) {
    if (argc < 4) { usage(); return 1; }
    const double secs = atof(argv[2]);
    const std::string path = argv[3];
    double rate = 2e6;
    SynthConfig cfg;
    cfg.mode = 22;
    for (int i = 4; i + 1 < argc; i += 2) {
        const std::string k = argv[i];
        const double v = atof(argv[i + 1]);
        if (k == "--rate") rate = v;
        else if (k == "--snr") cfg.snrDb = v;
        else if (k == "--weak") cfg.modeVal[0] = v;
        else if (k == "--cfo") cfg.cfoHz = v;
        else if (k == "--sro") cfg.sroPpm = v;
        else if (k == "--protocols") cfg.modeOpt[0] = (int)v;
        else if (k == "--region") cfg.modeOpt[1] = (int)v;
        else if (k == "--seed") cfg.modeOpt[2] = (int)v;
        else { printf("unknown option %s\n", k.c_str()); return 1; }
    }
    auto syn = makeMeshSynth(cfg, rate);
    FILE* f = fopen(path.c_str(), "wb");
    if (!syn || !f) { printf("cannot write %s\n", path.c_str()); return 1; }
    const bool cs8 = !endsWith(path, ".cf32");
    std::vector<cf32> buf(65536);
    std::vector<int8_t> b8(2 * buf.size());
    for (size_t done = 0; done < (size_t)(secs * rate);) {
        const size_t n = std::min(buf.size(), (size_t)(secs * rate) - done);
        syn->generate(buf.data(), n);
        if (cs8) {
            for (size_t i = 0; i < n; i++) {
                b8[2 * i] = (int8_t)std::lround(std::max(-127.f, std::min(127.f, buf[i].real() * 127.f)));
                b8[2 * i + 1] = (int8_t)std::lround(std::max(-127.f, std::min(127.f, buf[i].imag() * 127.f)));
            }
            fwrite(b8.data(), 2, n, f);
        } else fwrite(buf.data(), sizeof(cf32), n, f);
        done += n;
    }
    fclose(f);
    printf("wrote %.1f s at %.3f Msps to %s (%s); tune to %.3f MHz\n", secs, rate / 1e6, path.c_str(), cs8 ? "cs8" : "cf32", cfg.modeOpt[1] == 1 ? 906.875 : 869.525);
    return 0;
}

static int rx(int argc, char** argv) {
    if (argc < 3) { usage(); return 1; }
    const std::string path = argv[2];
    double rate = 0, offset = -meshTuning().tuneOffsetHz, tuned = meshTuning().defMhz * 1e6, maxSecs = 1e18;
    std::string format = endsWith(path, ".cf32") ? "cf32" : endsWith(path, ".cu8") ? "cu8" : "cs8";
    MeshReceiver r;
    for (int i = 3; i < argc; i++) {
        const std::string k = argv[i];
        const char* v = i + 1 < argc ? argv[i + 1] : "";
        if (k == "--all-presets") { r.setPresetSearch(true); continue; }
        i++;
        if (k == "--rate") rate = atof(v);
        else if (k == "--format") format = v;
        else if (k == "--offset") offset = atof(v);
        else if (k == "--tuned") tuned = atof(v);
        else if (k == "--region") r.setRegion(atoi(v));
        else if (k == "--secs") maxSecs = atof(v);
        else if (k == "--key" || k == "--mckey") {
            const std::string s = v;
            const size_t eq = s.find('=');
            const bool ok = eq != std::string::npos && (k == "--key" ? r.addMeshtasticChannel(s.substr(0, eq), s.substr(eq + 1)) : r.addMeshCoreChannel(s.substr(0, eq), s.substr(eq + 1)));
            if (!ok) { printf("bad channel key %s\n", v); return 1; }
        } else { printf("unknown option %s\n", k.c_str()); return 1; }
    }
    if (rate <= 0) { usage(); return 1; }
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) { printf("cannot open %s\n", path.c_str()); return 1; }
    r.setLogCallback([](const std::string& s) { printf("log: %s\n", s.c_str()); });
    r.configure(rate);
    r.setSignalOffset(offset);
    r.setTunedHz(tuned);
    const size_t bytes = format == "cf32" ? 8 : 2;
    std::vector<uint8_t> raw(65536 * bytes);
    std::vector<cf32> buf(65536);
    MeshTelemetry t;
    uint64_t last = 0;
    size_t shown = 0, total = 0;
    double cpu = 0;
    auto show = [&](const MeshTelemetry& tt) {
        // the packet table keeps the last 200: print the ones not shown yet
        const size_t have = tt.blocksOk + tt.blocksBad;
        const size_t fresh = std::min(have - std::min(have, shown), tt.packets.size());
        for (size_t i = tt.packets.size() - fresh; i < tt.packets.size(); i++) {
            const auto& p = tt.packets[i];
            printf("%9.3f s  %-10s %.4f MHz SF%-2d %5.1f kHz 4/%d  SNR %5.1f dB  %3d B  %s%s %s -> %s %s%s%s\n", p.timeSec, p.protocol == 1 ? "Meshtastic" : "MeshCore",
                   p.freqHz / 1e6, p.sf, p.bwHz / 1e3, p.cr, p.snrDb, p.size, p.crcOk ? "" : "CRC ERROR ", p.type.c_str(), p.from.c_str(), p.to.c_str(),
                   p.decrypted ? "" : (p.note.empty() ? "" : ("[" + p.note + "] ").c_str()), p.detail.c_str(), p.channel.empty() ? "" : ("  #" + p.channel).c_str());
        }
        shown = have;
    };
    while (total < (size_t)(maxSecs * rate)) {
        const size_t got = fread(raw.data(), bytes, buf.size(), f);
        if (got == 0) break;
        for (size_t i = 0; i < got; i++) {
            if (format == "cf32") memcpy(&buf[i], &raw[8 * i], 8);
            else if (format == "cu8") buf[i] = cf32(((float)raw[2 * i] - 127.5f) / 127.5f, ((float)raw[2 * i + 1] - 127.5f) / 127.5f);
            else buf[i] = cf32((float)(int8_t)raw[2 * i] / 127.f, (float)(int8_t)raw[2 * i + 1] / 127.f);
        }
        const std::clock_t a = std::clock();
        r.feed(buf.data(), got);
        cpu += (double)(std::clock() - a) / CLOCKS_PER_SEC;
        total += got;
        if (r.telemetry(t, last)) { last = t.seq; show(t); }
    }
    fclose(f);
    // a little silence lets the last frame finish
    std::vector<cf32> z(buf.size(), cf32(0, 0));
    for (int i = 0; i < 8; i++) r.feed(z.data(), z.size());
    r.telemetry(t, 0);
    show(t);
    printf("\n%s\n%.1f s of signal, receiver %.2f s of CPU (%.0fx real time)\n", meshSummary(t).c_str(), total / rate, cpu, cpu > 0 ? total / rate / cpu : 0.0);
    for (const auto& d : t.decoders)
        printf("  %-12s %.4f MHz SF%-2d %5.1f kHz%s: %llu frames, %llu CRC errors, %llu bad headers\n", d.preset.c_str(), d.freqHz / 1e6, d.sf, d.bwHz / 1e3, d.inBand ? "" : " (outside the band)",
               (unsigned long long)d.frames, (unsigned long long)d.crcBad, (unsigned long long)d.headerBad);
    printf("nodes:\n");
    for (const auto& n : t.nodes) {
        printf("  %-10s %-14s %-24s %-5s %-24s %-9s", n.protocol == 1 ? "Meshtastic" : "MeshCore", n.id.c_str(), n.longName.c_str(), n.shortName.c_str(), n.hwModel.c_str(), n.role.c_str());
        if (n.hasPosition) printf(" %.5f %.5f %.0f m", n.lat, n.lon, n.altM);
        if (n.batteryPct >= 0) printf(" battery %.0f %%", n.batteryPct);
        if (n.voltage >= 0) printf(" %.2f V", n.voltage);
        if (n.hasEnv) printf(" %.1f C %.0f %% %.1f hPa", n.tempC, n.humidity, n.pressureHpa);
        printf(" hops %d SNR %.1f dB last %.1f s, %u packets\n", n.hopsAway, n.lastSnrDb, n.lastHeard, n.packets);
    }
    printf("messages:\n");
    for (const auto& m : t.messages)
        printf("  %9.3f s %-10s #%-10s %s%s -> %s: %s\n", m.timeSec, m.protocol == 1 ? "Meshtastic" : "MeshCore", m.channel.c_str(), m.from.c_str(),
               m.fromName.empty() || m.fromName == m.from ? "" : (" (" + m.fromName + ")").c_str(), m.to.empty() ? "all" : m.to.c_str(), m.text.c_str());
    return 0;
}

int main(int argc, char** argv) {
    if (argc >= 2 && !strcmp(argv[1], "gen")) return gen(argc, argv);
    if (argc >= 2 && !strcmp(argv[1], "rx")) return rx(argc, argv);
    usage();
    return 1;
}
