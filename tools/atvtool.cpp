// Analog TV: write the test signal to a file (atvtool gen ...) or run the receiver on a recording (atvtool rx ...).
#include "dect2/atv_card.h"
#include "dect2/atv_gen.h"
#include "dect2/atv_rx.h"
#include "dect2/atv_testkit.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace dect2;

static void usage() {
    printf("atvtool gen <file.cs8|.cu8|.cf32> [options]     write the test signal\n"
           "  --secs N        length in seconds (default 2)\n"
           "  --rate MSPS     sample rate in Msps (default 10)\n"
           "  --system S      g (B/G 8 MHz, default), b (B/G 7 MHz), i, dk, m, n\n"
           "  --colour C      default, pal, ntsc, secam, none\n"
           "  --pattern P     card (default), bars, ramp\n"
           "  --snr DB        carrier-to-noise ratio in 5 MHz (default 40, 100 = none)\n"
           "  --cfo HZ        carrier offset          --sro PPM   sample clock error\n"
           "  --echo DB       ghost level (dB below the signal)   --echo-us US   ghost delay   --echo-phase DEG\n"
           "  --hum PCT       mains hum on the carrier (percent)  --hum-hz HZ\n"
           "  --compress PCT  sync compression (percent)\n"
           "  --sound M       beeps (default), melody, carrier, none, tone\n"
           "  --no-groupdelay --nyquist-tx --no-setup\n"
           "  --frame-png F   also write the reference test card of the first frame as a PPM file\n"
           "atvtool rx <file> --rate MSPS [--format cs8|cu8|cf32] [options]   run the receiver\n"
           "atvtool sweep [name-filter] [--secs N]   run the receiver over a list of impaired signals and print one line each\n");
}


// ---- sweep: generator -> impairment -> 8 bit -> receiver, one line per case
namespace {
struct SweepCase {
    std::string name;
    AtvGenConfig cfg;
    atvkit::Options opt;
    double secs = 3;
};

double toneSnrDb(const std::vector<float>& a, double hz, size_t from, size_t to) {
    if (to > a.size()) to = a.size();
    if (to <= from + 4800) return -99;
    const double amp = atvkit::toneAmp(a, hz, from, to);
    double e = 0;
    for (size_t i = from; i < to; i++) e += (double)a[i] * a[i];
    e /= (double)(to - from);
    const double sig = amp * amp / 2, noise = std::max(1e-12, e - sig);
    return 10 * std::log10(sig / noise);
}

int sweepMain(int argc, char** argv) {
    std::string filter;
    double secsOverride = 0;
    for (int i = 2; i < argc; i++) {
        const std::string a = argv[i];
        if (a == "--secs" && i + 1 < argc) secsOverride = atof(argv[++i]);
        else filter = a;
    }
    std::vector<SweepCase> cases;
    auto base = [](int sys = kAtvG, int col = kAtvPal) { AtvGenConfig c; c.sys = sys; c.colour = col; c.cnrDb = 40; c.rate = 10e6; c.sound = 4; return c; };
    auto add = [&](const std::string& n, const AtvGenConfig& c, const atvkit::Options& o = atvkit::Options(), double secs = 3) { SweepCase k; k.name = n; k.cfg = c; k.opt = o; k.secs = secs; cases.push_back(k); };
    add("clean 10 Msps", base());
    for (double snr : {35.0, 30.0, 25.0, 22.0, 20.0, 18.0, 16.0, 14.0, 12.0, 10.0, 8.0}) { auto c = base(); c.cnrDb = snr; char n[64]; snprintf(n, sizeof n, "C/N %.0f dB", snr); add(n, c); }
    for (double cfo : {-1.5e6, -600e3, -150e3, -40e3, -12e3, 12e3, 40e3, 150e3, 600e3, 1.2e6}) { auto c = base(); c.cfoHz = cfo; char n[64]; snprintf(n, sizeof n, "carrier offset %+.0f kHz", cfo / 1e3); add(n, c); }
    for (double ppm : {-100.0, -40.0, 40.0, 100.0}) { auto c = base(); c.sroPpm = ppm; char n[64]; snprintf(n, sizeof n, "clock error %+.0f ppm", ppm); add(n, c); }
    for (double rate : {8e6, 12.5e6, 16e6, 20e6}) { auto c = base(); c.rate = rate; char n[64]; snprintf(n, sizeof n, "rate %.1f Msps", rate / 1e6); add(n, c); }
    for (size_t ch : {(size_t)1, (size_t)7, (size_t)4096, (size_t)65536}) { atvkit::Options o; o.chunk = ch; char n[64]; snprintf(n, sizeof n, "chunks of %zu", ch); add(n, base(), o, ch == 1 ? 0.8 : 2.5); }
    { atvkit::Options o; o.quantise = false; add("no 8-bit quantisation", base(), o); }
    for (double sc : {0.5, 0.2, 0.1, 0.05}) { atvkit::Options o; o.scale = sc; char n[64]; snprintf(n, sizeof n, "signal at %.0f%% of the nominal level", sc * 100); add(n, base(), o); }
    for (double hz : {5e3, 40e3, -200e3}) { atvkit::Options o; o.impair = atvkit::frequencyStep(1.5, hz, 10e6); char nm[64]; snprintf(nm, sizeof nm, "carrier jumps by %+.0f kHz at 1.5 s", hz / 1e3); add(nm, base(), o, 4.0); }
    { atvkit::Options o; o.impair = atvkit::dcOffset(0.06f, -0.04f); add("DC offset 0.06 / -0.04", base(), o); }
    { atvkit::Options o; o.impair = atvkit::iqImbalance(1.15, 6); add("IQ imbalance 1.2 dB, 6 deg", base(), o); }
    for (double ms : {2.0, 5.0, 20.0, 60.0}) {
        atvkit::Options o;
        o.impair = atvkit::dropout(1.5, ms, 10e6);
        char nm[64]; snprintf(nm, sizeof nm, "dropout of %.0f ms (zeros)", ms); add(nm, base(), o, 3.0);
    }
    { atvkit::Options o; o.impair = atvkit::impulses(200000, 50, 1.5f); add("impulse noise: 5 us bursts, 50 per second", base(), o); }
    for (double db : {-30.0, -15.0, -6.0}) {      // a carrier of the adjacent channels that aliases into the picture band
        atvkit::Options o;
        o.impair = atvkit::tone(4.75e6, 0.4 * std::pow(10.0, db / 20), 10e6);
        char nm[64]; snprintf(nm, sizeof nm, "CW at +4.75 MHz, %.0f dB re the carrier", db); add(nm, base(), o, 6.0);
    }
    for (double db : {-30.0, -20.0}) {
        atvkit::Options o;
        o.impair = atvkit::tone(-1.0e6, 0.4 * std::pow(10.0, db / 20), 10e6);
        char nm[64]; snprintf(nm, sizeof nm, "CW in the picture band (-1.0 MHz), %.0f dB", db); add(nm, base(), o);
    }
    for (double db : {-25.0, -15.0, -9.0, -6.0}) for (double ph : {0.0, 90.0, 180.0}) { auto c = base(); c.echoDb = -db; c.echoDelayUs = 1.5; c.echoPhaseDeg = ph; char nm[64]; snprintf(nm, sizeof nm, "ghost %.0f dB, 1.5 us, %.0f deg", db, ph); add(nm, c); }
    for (double h : {4.0, 10.0}) { auto c = base(); c.humPct = h; char nm[64]; snprintf(nm, sizeof nm, "50 Hz hum %.0f%%", h); add(nm, c); }
    for (double comp : {0.2, 0.4, 0.6}) { auto c = base(); c.syncCompression = comp; char nm[64]; snprintf(nm, sizeof nm, "sync compression %.0f%%", comp * 100); add(nm, c); }
    {
        SweepCase k; k.name = "level step: x1, then x0.2, then x1"; k.cfg = base(); k.secs = 4.5;
        k.opt.impair = atvkit::levelStep(1.5, 3.0, 0.2f, 10e6);
        cases.push_back(k);
    }
    // the standards
    struct S { int sys, col; const char* n; };
    for (S s : {S{kAtvB, kAtvPal, "PAL B 7 MHz"}, S{kAtvI, kAtvPal, "PAL I"}, S{kAtvDK, kAtvPal, "PAL D/K"}, S{kAtvM, kAtvNtsc, "NTSC M"}, S{kAtvM, kAtvPal, "PAL-M"}, S{kAtvN, kAtvPal, "PAL-N"}, S{kAtvG, kAtvMono, "B/G monochrome"}}) {
        for (double snr : {40.0, 20.0, 14.0}) { auto c = base(s.sys, s.col); c.cnrDb = snr; char nm[96]; snprintf(nm, sizeof nm, "%s, C/N %.0f dB", s.n, snr); add(nm, c); }
    }
    for (S s : {S{kAtvG, kAtvSecam, "SECAM B/G"}, S{kAtvDK, kAtvSecam, "SECAM D/K"}}) {
        for (double snr : {40.0, 30.0, 25.0, 22.0, 20.0, 18.0, 16.0}) { auto c = base(s.sys, s.col); c.cnrDb = snr; char nm[96]; snprintf(nm, sizeof nm, "%s, C/N %.0f dB", s.n, snr); add(nm, c); }
    }
    for (int id : {1, 2}) { auto c = base(kAtvG, kAtvSecam); c.secamIdent = id; add(id == 1 ? "SECAM lead-in only" : "SECAM field identification only", c); }
    for (double cfo : {-300e3, 150e3}) { auto c = base(kAtvG, kAtvSecam); c.cfoHz = cfo; char nm[64]; snprintf(nm, sizeof nm, "SECAM carrier offset %+.0f kHz", cfo / 1e3); add(nm, c); }
    { auto c = base(kAtvG, kAtvSecam); c.sroPpm = 80; add("SECAM clock error +80 ppm", c); }
    for (double r : {12.5e6, 16e6, 20e6}) { auto c = base(kAtvG, kAtvSecam); c.rate = r; char nm[64]; snprintf(nm, sizeof nm, "SECAM rate %.1f Msps", r / 1e6); add(nm, c); }
    printf("%-46s %-3s %-18s %-5s %6s %6s %6s %5s %7s %6s %5s %4s %7s %6s %6s %5s\n", "case", "st", "system", "colr", "lock", "bars", "luma", "bad", "ppm", "vSNR", "C/N", "cmp", "cfo", "toneHz", "tSNR", "xRT");
    int shown = 0;
    for (auto& k : cases) {
        if (!filter.empty() && k.name.find(filter) == std::string::npos) continue;
        if (secsOverride > 0) k.secs = secsOverride;
        AtvGenerator probe(k.cfg);
        if (!probe.ok()) { printf("%-46s generator does not support this\n", k.name.c_str()); continue; }
        const atvkit::Run r = atvkit::run(k.cfg, k.secs, k.opt);
        const AtvTelemetry& t = r.tel;
        AtvFormat f = probe.format();
        char bars[16] = "-", luma[16] = "-";
        if (r.frame && r.frame->width == f.picW && r.frame->height == f.picH) {
            AtvCard card(f);
            double c8[8][3], worst = 0;
            atvkit::barColours(*r.frame, card, c8);
            for (int i = 0; i < 8; i++) { float rgb[3]; atvEbuBar(i, rgb); for (int q = 0; q < 3; q++) worst = std::max(worst, std::fabs(c8[i][q] - rgb[q])); }
            if (f.colour == kAtvMono) { worst = 0; for (int i = 0; i < 8; i++) { float rgb[3], y, u, v; atvEbuBar(i, rgb); atvRgbToYuv(rgb[0], rgb[1], rgb[2], y, u, v); worst = std::max(worst, std::fabs(0.299 * c8[i][0] + 0.587 * c8[i][1] + 0.114 * c8[i][2] - y)); } }
            snprintf(bars, sizeof bars, "%.3f", worst);
            snprintf(luma, sizeof luma, "%.1f", atvkit::lumaDifference(*r.frame, card, 2));
        }
        const size_t na = r.audio.size();
        const double hz = na > 48000 * 2 ? atvkit::peakTone(r.audio, na - 24000, na - 4800, 500, 2000) : 0;
        const double tsnr = na > 48000 * 2 ? toneSnrDb(r.audio, 1000, na - 24000, na - 4800) : -99;
        char lock[16];
        if (r.lockedAfterMs >= 0) snprintf(lock, sizeof lock, "%d", r.lockedAfterMs); else snprintf(lock, sizeof lock, "-");
        printf("%-46s %-3d %-18s %-5s %6s %6s %6s %5llu %+7.1f %6.1f %5.1f %4.0f %+7.0f %6.0f %6.1f %5.1f\n", k.name.c_str(), t.state, t.system.empty() ? "-" : t.system.c_str(), t.colour ? "yes" : "no", lock, bars, luma,
               (unsigned long long)t.blocksBad, t.lineErrPpm, t.snrDb, t.carrierToNoiseDb, t.syncCompressionPct, t.cfoHz, hz, tsnr, r.cpuSecs > 0 ? r.signalSecs / r.cpuSecs : 0.0);
        fflush(stdout);
        shown++;
    }
    return 0;
}
} // namespace

int main(int argc, char** argv) {
    if (argc >= 2 && std::string(argv[1]) == "sweep") return sweepMain(argc, argv);
    if (argc < 3) { usage(); return 1; }
    const std::string cmd = argv[1];
    if (cmd == "gen") {
        const std::string path = argv[2];
        AtvGenConfig c;
        double secs = 2;
        std::string refPpm;
        int sysSel = kAtvG, colSel = -1;
        for (int i = 3; i < argc; i++) {
            const std::string a = argv[i];
            auto next = [&]() { return i + 1 < argc ? argv[++i] : (char*)"0"; };
            if (a == "--secs") secs = atof(next());
            else if (a == "--rate") c.rate = atof(next()) * 1e6;
            else if (a == "--system") { const std::string v = next(); sysSel = v == "b" ? kAtvB : v == "i" ? kAtvI : v == "dk" ? kAtvDK : v == "m" ? kAtvM : v == "n" ? kAtvN : kAtvG; }
            else if (a == "--colour") { const std::string v = next(); colSel = v == "pal" ? kAtvPal : v == "ntsc" ? kAtvNtsc : v == "secam" ? kAtvSecam : v == "none" ? kAtvMono : -1; }
            else if (a == "--pattern") { const std::string v = next(); c.pattern = v == "bars" ? 1 : v == "ramp" ? 2 : 0; }
            else if (a == "--snr") c.cnrDb = atof(next());
            else if (a == "--cfo") c.cfoHz = atof(next());
            else if (a == "--sro") c.sroPpm = atof(next());
            else if (a == "--echo") c.echoDb = atof(next());
            else if (a == "--echo-us") c.echoDelayUs = atof(next());
            else if (a == "--echo-phase") c.echoPhaseDeg = atof(next());
            else if (a == "--hum") c.humPct = atof(next());
            else if (a == "--hum-hz") c.humHz = atof(next());
            else if (a == "--compress") c.syncCompression = atof(next()) / 100;
            else if (a == "--sound") { const std::string v = next(); c.sound = v == "melody" ? 1 : v == "carrier" ? 2 : v == "none" ? 3 : v == "tone" ? 4 : 0; }
            else if (a == "--no-groupdelay") c.groupDelay = false;
            else if (a == "--nyquist-tx") c.nyquistTx = true;
            else if (a == "--no-setup") c.setup = false;
            else if (a == "--frame-png") refPpm = next();
        }
        c.sys = sysSel;
        c.colour = colSel >= 0 ? colSel : (sysSel == kAtvM ? kAtvNtsc : kAtvPal);
        AtvGenerator gen(c);
        if (!gen.ok()) { fprintf(stderr, "that combination of system and colour does not exist (or is not supported yet)\n"); return 1; }
        FILE* f = fopen(path.c_str(), "wb");
        if (!f) { perror("open"); return 1; }
        const bool cf32f = path.size() > 5 && path.substr(path.size() - 5) == ".cf32";
        const bool cu8 = path.size() > 4 && path.substr(path.size() - 4) == ".cu8";
        const size_t total = (size_t)(secs * c.rate);
        std::vector<cf32> buf;
        for (size_t done = 0; done < total;) {
            const size_t n = std::min<size_t>(1 << 16, total - done);
            buf.clear();
            gen.generate(n, buf);
            if (cf32f) fwrite(buf.data(), sizeof(cf32), n, f);
            else {
                std::vector<uint8_t> b(2 * n);
                for (size_t i = 0; i < n; i++) {
                    const float re = std::min(1.f, std::max(-1.f, buf[i].real())), im = std::min(1.f, std::max(-1.f, buf[i].imag()));
                    if (cu8) { b[2 * i] = (uint8_t)std::lround(re * 127.5f + 127.5f); b[2 * i + 1] = (uint8_t)std::lround(im * 127.5f + 127.5f); }
                    else { b[2 * i] = (uint8_t)(int8_t)std::lround(re * 127.f); b[2 * i + 1] = (uint8_t)(int8_t)std::lround(im * 127.f); }
                }
                fwrite(b.data(), 1, b.size(), f);
            }
            done += n;
        }
        fclose(f);
        printf("%s: %.2f s of %s at %.3f Msps, vision carrier %.3f MHz, sound %.3f MHz from the channel centre\n", path.c_str(), secs, gen.format().name.c_str(), c.rate / 1e6,
               (atvVisionOffsetHz(gen.format()) + c.cfoHz) / 1e6, (atvSoundOffsetHz(gen.format()) + c.cfoHz) / 1e6);
        if (!refPpm.empty()) {
            std::vector<uint8_t> rgba;
            gen.card().referenceFrame(c.startSec, 0, rgba);
            FILE* p = fopen(refPpm.c_str(), "wb");
            if (p) {
                fprintf(p, "P6\n%d %d\n255\n", gen.card().width(), gen.card().height());
                for (size_t i = 0; i < rgba.size(); i += 4) fwrite(&rgba[i], 1, 3, p);
                fclose(p);
            }
        }
        return 0;
    }
    if (cmd == "rx") {
        const std::string path = argv[2];
        double rate = 10e6;
        std::string fmtName, ppm, wav;
        double ppmEvery = 0;
        bool envelope = false, verbose = false, bob = false;
        int sysSel = -1, colSel = -1;
        double secs = 0;
        for (int i = 3; i < argc; i++) {
            const std::string a = argv[i];
            auto next = [&]() { return i + 1 < argc ? argv[++i] : (char*)"0"; };
            if (a == "--rate") rate = atof(next()) * 1e6;
            else if (a == "--format") fmtName = next();
            else if (a == "--ppm") ppm = next();
            else if (a == "--ppm-every") ppmEvery = atof(next());
            else if (a == "--wav") wav = next();
            else if (a == "--envelope") envelope = true;
            else if (a == "--bob") bob = true;
            else if (a == "--verbose") verbose = true;
            else if (a == "--secs") secs = atof(next());
            else if (a == "--system") { const std::string v = next(); sysSel = v == "b" ? kAtvB : v == "i" ? kAtvI : v == "dk" ? kAtvDK : v == "m" ? kAtvM : v == "n" ? kAtvN : kAtvG; }
            else if (a == "--colour") { const std::string v = next(); colSel = v == "pal" ? kAtvPal : v == "ntsc" ? kAtvNtsc : v == "secam" ? kAtvSecam : v == "none" ? kAtvMono : -1; }
        }
        const bool cf32f = fmtName == "cf32" || (fmtName.empty() && path.size() > 5 && path.substr(path.size() - 5) == ".cf32");
        const bool cu8 = fmtName == "cu8" || (fmtName.empty() && path.size() > 4 && path.substr(path.size() - 4) == ".cu8");
        FILE* f = fopen(path.c_str(), "rb");
        if (!f) { perror("open"); return 1; }
        AtvReceiver rx;
        rx.setSilent(true);
        std::vector<float> audio;
        rx.setAudioTap([&](const float* l, const float*, size_t n) { audio.insert(audio.end(), l, l + n); });
        rx.setLogCallback([&](const std::string& m) { printf("log: %s\n", m.c_str()); });
        rx.configure(rate);
        if (!rx.ready()) { fprintf(stderr, "the sample rate is too low for analog TV (8 Msps at least)\n"); return 1; }
        if (envelope) rx.setDetector(1);
        if (bob) rx.setDeinterlace(1);
        if (sysSel >= 0 || colSel >= 0) rx.setStandard(sysSel, colSel);
        auto writePpm = [&](const std::string& name, const AtvFrame& fr) {
            FILE* p = fopen(name.c_str(), "wb");
            if (!p) return;
            fprintf(p, "P6\n%d %d\n255\n", fr.width, fr.height);
            for (size_t i = 0; i < fr.rgba.size(); i += 4) fwrite(&fr.rgba[i], 1, 3, p);
            fclose(p);
        };
        const size_t bps = cf32f ? 8 : 2;
        std::vector<uint8_t> raw(65536 * bps);
        std::vector<cf32> buf(65536);
        uint64_t seq = 0, fseq = 0, total = 0;
        double nextPrint = 1, nextPpm = ppmEvery;
        AtvTelemetry t;
        std::shared_ptr<const AtvFrame> last;
        int shots = 0;
        for (;;) {
            const size_t got = fread(raw.data(), bps, 65536, f);
            if (!got) break;
            for (size_t i = 0; i < got; i++) {
                if (cf32f) { float v[2]; memcpy(v, &raw[8 * i], 8); buf[i] = cf32(v[0], v[1]); }
                else if (cu8) buf[i] = cf32((raw[2 * i] - 127.5f) / 127.5f, (raw[2 * i + 1] - 127.5f) / 127.5f);
                else buf[i] = cf32((int8_t)raw[2 * i] / 128.0f, (int8_t)raw[2 * i + 1] / 128.0f);
            }
            rx.feed(buf.data(), got);
            total += got;
            const double now = (double)total / rate;
            if (rx.telemetry(t, seq)) seq = t.seq;
            if (auto fr = rx.frame(fseq)) last = fr;
            if (now >= nextPrint) {
                nextPrint += 1;
                printf("[%5.1f s] %s%s\n", now, atvSummary(t).c_str(), verbose ? "" : "");
                if (verbose) printf("        state %d  lines %llu  fields %llu  frames %llu  line %.2f Hz (%+.1f ppm)  field %.3f Hz  carrier %+.4f MHz  cfo %+.0f Hz  detector %s  burst %.0f%%  phase err %.1f deg  sync depth %.1f%%  white %.2f  sound %s dev %.1f kHz level %.1f dB\n",
                                    t.state, (unsigned long long)t.lineCount, (unsigned long long)t.fieldCount, (unsigned long long)t.frameCount, t.lineHz, t.lineErrPpm, t.fieldHz, t.visionHz / 1e6, t.cfoHz,
                                    t.syncDetector ? "sync" : "envelope", t.burstLevel * 100, t.chromaPhaseErrDeg, t.syncDepthPct, t.whitePeak, t.soundPresent ? "yes" : "no", t.soundDevKhz, t.soundLevelDb);
            }
            if (!ppm.empty() && ppmEvery > 0 && now >= nextPpm && last) {
                nextPpm += ppmEvery;
                char nm[512];
                snprintf(nm, sizeof nm, "%s_%03d.ppm", ppm.c_str(), shots++);
                writePpm(nm, *last);
            }
            if (secs > 0 && now >= secs) break;
        }
        fclose(f);
        if (!ppm.empty() && last) { writePpm(ppm + "_last.ppm", *last); printf("last picture: %s_last.ppm (%dx%d, %s, picture %llu)\n", ppm.c_str(), last->width, last->height, last->colour ? "colour" : "grey", (unsigned long long)last->seq); }
        if (!wav.empty()) {
            FILE* w = fopen(wav.c_str(), "wb");
            if (w) {
                const uint32_t n = (uint32_t)audio.size(), dataBytes = n * 2, rateHz = 48000, byteRate = 96000;
                const uint16_t one = 1, bits = 16, align = 2, pcm = 1;
                fwrite("RIFF", 1, 4, w); const uint32_t riff = 36 + dataBytes; fwrite(&riff, 4, 1, w); fwrite("WAVEfmt ", 1, 8, w);
                const uint32_t fmtLen = 16; fwrite(&fmtLen, 4, 1, w); fwrite(&pcm, 2, 1, w); fwrite(&one, 2, 1, w); fwrite(&rateHz, 4, 1, w); fwrite(&byteRate, 4, 1, w);
                fwrite(&align, 2, 1, w); fwrite(&bits, 2, 1, w); fwrite("data", 1, 4, w); fwrite(&dataBytes, 4, 1, w);
                for (float v : audio) { const int16_t s16 = (int16_t)std::lround(std::max(-1.f, std::min(1.f, v)) * 32767); fwrite(&s16, 2, 1, w); }
                fclose(w);
                printf("sound: %s (%u samples at 48 kHz)\n", wav.c_str(), n);
            }
        }
        printf("done: %s\n", atvSummary(t).c_str());
        return 0;
    }
    usage();
    return 1;
}
