// Marine command line tool: write a test signal to a file, or decode a recording.
//   marinetool gen <secs> <file.cs8|.cf32> [--service 1|2|3|4] [--rate sps] [--snr dB] [--cfo Hz] [--sro ppm] [--fade 1] [--mist Hz]
//                                          [--offset Hz] [--lpm n] [--ioc 576|288] [--lines n] [--idle secs] [--phasing secs]
//   marinetool rx <file> --rate <sps> [--format cs8|cu8|cf32] [--service 0..3] [--freq Hz] [--offset Hz] [--pgm out.pgm]
// Services: 1 NAVTEX, 2 DSC MF/HF, 3 weather fax, 4 DSC VHF channel 70 (gen); 0 auto, 1 NAVTEX, 2 DSC, 3 fax (rx).
#include "dect2/marine_gen.h"
#include "dect2/marine_rx.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
using namespace dect2;

static const char* opt(int argc, char** argv, const char* name, const char* def = nullptr) {
    for (int i = 1; i + 1 < argc; i++) if (!strcmp(argv[i], name)) return argv[i + 1];
    return def;
}
static bool endsWith(const std::string& s, const char* e) { const size_t n = strlen(e); return s.size() >= n && s.compare(s.size() - n, n, e) == 0; }

static int gen(int argc, char** argv) {
    if (argc < 4) { printf("marinetool gen <secs> <file.cs8|.cf32> [options]\n"); return 2; }
    const double secs = atof(argv[2]);
    const std::string path = argv[3];
    SynthConfig cfg;
    cfg.mode = 17;
    const double rate = atof(opt(argc, argv, "--rate", "2000000"));
    cfg.snrDb = atof(opt(argc, argv, "--snr", "30"));
    cfg.cfoHz = atof(opt(argc, argv, "--cfo", "0"));
    cfg.sroPpm = atof(opt(argc, argv, "--sro", "0"));
    cfg.modeOpt[0] = atoi(opt(argc, argv, "--service", "1"));
    cfg.modeOpt[1] = atoi(opt(argc, argv, "--fade", "0"));
    cfg.modeOpt[3] = atoi(opt(argc, argv, "--lpm", "0"));
    cfg.modeOpt[4] = atoi(opt(argc, argv, "--ioc", "0"));
    cfg.modeOpt[5] = atoi(opt(argc, argv, "--lines", "0"));
    cfg.modeVal[0] = atof(opt(argc, argv, "--mist", "0"));
    cfg.modeVal[1] = atof(opt(argc, argv, "--idle", "0"));
    cfg.modeVal[2] = atof(opt(argc, argv, "--phasing", "0"));
    cfg.modeOpt[2] = atoi(opt(argc, argv, "--msg", "0"));
    const double off = atof(opt(argc, argv, "--offset", "0"));
    auto syn = makeMarineSynthAt(cfg, rate, off);
    if (!syn) { printf("rate too low\n"); return 2; }
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) { printf("cannot write %s\n", path.c_str()); return 2; }
    const bool cs8 = endsWith(path, ".cs8");
    size_t left = (size_t)(secs * rate);
    std::vector<cf32> b(65536);
    std::vector<int8_t> q(2 * b.size());
    while (left) {
        const size_t n = std::min(left, b.size());
        syn->generate(b.data(), n);
        if (cs8) {
            for (size_t i = 0; i < n; i++) {
                q[2 * i] = (int8_t)std::lrintf(std::max(-1.f, std::min(1.f, b[i].real())) * 127.f);
                q[2 * i + 1] = (int8_t)std::lrintf(std::max(-1.f, std::min(1.f, b[i].imag())) * 127.f);
            }
            fwrite(q.data(), 2, n, f);
        } else fwrite(b.data(), sizeof(cf32), n, f);
        left -= n;
    }
    fclose(f);
    printf("wrote %.1f s at %.0f sps to %s\n", secs, rate, path.c_str());
    return 0;
}

static int rx(int argc, char** argv) {
    if (argc < 3) { printf("marinetool rx <file> --rate <sps> [options]\n"); return 2; }
    const std::string path = argv[2];
    const double rate = atof(opt(argc, argv, "--rate", "0"));
    if (rate <= 0) { printf("--rate is needed\n"); return 2; }
    std::string fmt = opt(argc, argv, "--format", endsWith(path, ".cs8") ? "cs8" : endsWith(path, ".cu8") ? "cu8" : "cf32");
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) { printf("cannot read %s\n", path.c_str()); return 2; }
    MarineReceiver r;
    r.configure(rate);
    r.setSignalOffset(atof(opt(argc, argv, "--offset", "0")));
    r.setService(atoi(opt(argc, argv, "--service", "0")));
    r.setFrequencyHz(atof(opt(argc, argv, "--freq", "0")));
    r.setLogCallback([](const std::string& s) { printf("  [%s]\n", s.c_str()); });
    if (!r.ready()) { printf("sample rate too low\n"); return 2; }
    std::vector<cf32> b(65536);
    std::vector<uint8_t> raw(2 * b.size());
    MarineTelemetry t; uint64_t last = 0; MarineTelemetry fin;
    bool have = false;
    for (;;) {
        size_t n;
        if (fmt == "cf32") n = fread(b.data(), sizeof(cf32), b.size(), f);
        else {
            n = fread(raw.data(), 2, b.size(), f);
            for (size_t i = 0; i < n; i++) {
                const float a = fmt == "cs8" ? (float)(int8_t)raw[2 * i] / 128.f : ((float)raw[2 * i] - 127.5f) / 127.5f;
                const float c = fmt == "cs8" ? (float)(int8_t)raw[2 * i + 1] / 128.f : ((float)raw[2 * i + 1] - 127.5f) / 127.5f;
                b[i] = cf32(a, c);
            }
        }
        if (!n) break;
        r.feed(b.data(), n);
        if (r.telemetry(t, last)) { last = t.seq; fin = t; have = true; }
    }
    fclose(f);
    if (!have) { printf("no report\n"); return 1; }
    printf("%s\n", marineSummary(fin).c_str());
    printf("snr %.1f dB, centre %.1f Hz, tones %.1f / %.1f Hz, baud %.2f\n", fin.snrDb, fin.cfoHz, fin.toneHighHz, fin.toneLowHz, fin.baudEst);
    for (const auto& m : fin.navtex) {
        printf("NAVTEX %s subject %c (%s) number %d%s, %u chars, %u errors, heard %d time(s)\n%s\n", m.header.c_str(), m.subject, m.subjectName.c_str(), m.number, m.complete ? "" : " [incomplete]", m.chars, m.errors, m.repeats, m.text.c_str());
    }
    for (const auto& c : fin.dsc) {
        printf("DSC%s: %s [ECC %s, EOS %d, %d erased]\n", c.vhf ? " VHF" : "", c.text.c_str(), c.eccOk ? "ok" : "bad", c.eos, c.erasures);
        if (!c.telecmdText.empty()) printf("      %s\n", c.telecmdText.c_str());
        if (!c.freqRx.empty()) printf("      frequency %s %s\n", c.freqRx.c_str(), c.freqTx.c_str());
    }
    FaxImage img; uint64_t seq = 0;
    if (r.latestImage(img, seq) && img.width > 0) {
        printf("fax: %d x %d, IOC %d, %d lpm\n", img.width, img.height, fin.fax.ioc, fin.fax.lpm);
        if (const char* pgm = opt(argc, argv, "--pgm")) {
            FILE* o = fopen(pgm, "wb");
            if (o) { fprintf(o, "P5\n%d %d\n255\n", img.width, img.height); fwrite(img.pix.data(), 1, img.pix.size(), o); fclose(o); printf("wrote %s\n", pgm); }
        }
    }
    return 0;
}

int main(int argc, char** argv) {
    if (argc >= 2 && !strcmp(argv[1], "gen")) return gen(argc, argv);
    if (argc >= 2 && !strcmp(argv[1], "rx")) return rx(argc, argv);
    printf("usage: marinetool gen <secs> <file.cs8|.cf32> [--service 1|2|3|4] [--rate sps] [--snr dB] [--cfo Hz] [--sro ppm] [--fade 1] [--mist Hz] [--offset Hz]\n"
           "       marinetool rx <file> --rate <sps> [--format cs8|cu8|cf32] [--service 0..3] [--freq Hz] [--offset Hz] [--pgm out.pgm]\n");
    return 0;
}
