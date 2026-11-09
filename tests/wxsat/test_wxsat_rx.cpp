// wxsat links end to end on the built-in test signal, with the real impairments: carrier offset, Doppler rate, clock error, noise sweep.
#include "dect2/wxsat/wxsat_gen.h"
#include "dect2/wxsat/wxsat_proto.h"
#include "dect2/wxsat/wxsat_rx.h"
#include "dect2/wxsat/wxsat_tel.h"
#include "dect2/hrpt/hrpt_tel.h"
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>
using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

struct Case { int profile; double rate, secs, snr, cfo, drift, ppm; int minLines, minOk; };

static WxsatTelemetry run(const Case& k) {
    WxsatGenConfig g; g.profile = k.profile; g.rate = k.rate; g.snrDb = k.snr; g.cfoHz = k.cfo; g.driftHzPerS = k.drift; g.clockPpm = k.ppm; g.seed = 7;
    WxsatGenerator gen(g);
    WxsatReceiver rx(k.profile);
    rx.configure(k.rate);
    std::vector<cf32> buf(1 << 16);
    WxsatTelemetry t;
    uint64_t seq = 0;
    for (double done = 0; done < k.secs * k.rate; done += (double)buf.size()) {
        gen.generate(buf.data(), buf.size());
        rx.feed(buf.data(), buf.size());
        if (rx.telemetry(t, seq)) seq = t.seq;
    }
    printf("  %s snr %.0f cfo %.0f drift %.0f ppm %.0f: %s; cfo %.0f, %llu ok %llu bad, %llu lines, snr %.1f, lock %.2f\n", wxProfile(k.profile).name, k.snr, k.cfo, k.drift, k.ppm,
           wxsatSummary(t).c_str(), t.cfoHz, (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad, (unsigned long long)t.lines, t.snrDb, t.carrierLock);
    return t;
}

static void expect(const Case& k) {
    WxsatTelemetry t = run(k);
    CHECK((int64_t)t.blocksOk >= k.minOk, "%s: %llu frames", wxProfile(k.profile).name, (unsigned long long)t.blocksOk);
    CHECK((int)t.lines >= k.minLines, "%s: %llu lines", wxProfile(k.profile).name, (unsigned long long)t.lines);
}

static void checkPixels(const WxsatTelemetry& t, int ch, int shift) {
    const WxImage* im = t.image.get();
    CHECK(im != nullptr, "no image");
    if (!im) return;
    int good = 0, seen = 0, y0 = -1, offs = 0;
    for (const auto& l : im->line) {
        if (!l) continue;
        if (y0 < 0) { y0 = l->idx; for (int y = 0; y < 400; y++) { int m = 0; for (int x = 0; x < im->width; x += 37) m += std::abs(l->px[(size_t)ch * im->width + x] - (wxsat::wxTestPixel(ch, x, y) >> shift)) <= 3; if (m > (im->width / 37) * 8 / 10) { offs = y - l->idx; break; } } }
        const int y = l->idx + offs;
        for (int x = 0; x < im->width; x += 7) { seen++; good += std::abs(l->px[(size_t)ch * im->width + x] - (wxsat::wxTestPixel(ch, x, y % 400) >> shift)) <= 3; }
    }
    printf("    pixels: %d of %d right\n", good, seen);
    CHECK(seen > 0 && good > seen * 95 / 100, "pixels %d of %d", good, seen);
}

// test_wxsat_rx real <profile> <wav IQ 16-bit stereo> [offset Hz]: a recording, untrusted input, not in git
static int realFile(int argc, char** argv) {
    int prof = atoi(argv[2]);
    FILE* f = fopen(argv[3], "rb");
    if (!f) return 2;
    unsigned char h[44];
    if (fread(h, 1, 44, f) != 44) { fclose(f); return 2; }
    const uint32_t rate = (uint32_t)h[24] | h[25] << 8 | h[26] << 16 | (uint32_t)h[27] << 24;
    const double off = argc > 4 ? atof(argv[4]) : 0;
    WxsatReceiver rx(prof);
    rx.configure(rate);
    rx.setSignalOffset(off);
    std::vector<int16_t> raw(8192);
    std::vector<cf32> b;
    WxsatTelemetry t;
    uint64_t seq = 0;
    for (;;) {
        const size_t n = fread(raw.data(), 2, raw.size(), f);
        if (n < 2) break;
        b.resize(n / 2);
        for (size_t i = 0; i < n / 2; i++) b[i] = cf32(raw[i * 2] / 32768.f, raw[i * 2 + 1] / 32768.f);
        rx.feed(b.data(), b.size());
        if (rx.telemetry(t, seq)) seq = t.seq;
    }
    fclose(f);
    printf("real %s at %u Hz: %s; cfo %.0f, %llu ok, %llu lines, snr %.1f\n", wxProfile(prof).name, rate, wxsatSummary(t).c_str(), t.cfoHz, (unsigned long long)t.blocksOk, (unsigned long long)t.lines, t.snrDb);
    if (t.image) for (int c = 0; c < t.image->channels; c++) { double m = 0; int n = 0; for (const auto& l : t.image->line) if (l) { for (int x = 0; x < t.image->width; x += 11) { m += l->px[(size_t)c * t.image->width + x]; n++; } } printf("  channel %d mean %.1f\n", c, n ? m / n : 0); }
    if (t.image && argc > 5) {   // dump the picture as PGM (first channel of each line)
        FILE* o = fopen(argv[5], "wb");
        if (o) { fprintf(o, "P5 %d %d 255\n", t.image->width, t.image->lines); std::vector<uint8_t> z((size_t)t.image->width); for (const auto& l : t.image->line) fwrite(l ? l->px.data() : z.data(), 1, (size_t)t.image->width, o); fclose(o); }
    }
    return 0;
}

int main(int argc, char** argv) {
    if (argc > 3 && std::string(argv[1]) == "real") return realFile(argc, argv);
    const bool sweep = argc > 1 && std::string(argv[1]) == "sweep";
    // APT
    { Case k{kWxApt, 960e3, 12, 40, 2500, 30, 80, 15, 15}; WxsatTelemetry t = run(k); CHECK(t.blocksOk >= 15 && t.state == 3, "apt lines");
      if (t.image) { int nb = 0; for (auto& l : t.image->line) if (l) nb += l->px[100 + 86] > 60 ? 1 : 0; printf("    apt line count %d\n", (int)t.image->line.size()); } }
    { Case k{kWxApt, 960e3, 10, 14, -3000, -40, -60, 10, 10}; expect(k); }
    // NOAA HRPT
    { Case k{kWxNoaaHrpt, 4e6, 3.0, 22, 18e3, -400, 40, 12, 12}; WxsatTelemetry t = run(k); CHECK(t.blocksOk >= 12, "noaa hrpt frames"); CHECK(t.satName == "NOAA-19", "sat %s", t.satName.c_str()); if (t.image) checkPixels(t, 0, 2); }
    { Case k{kWxNoaaHrpt, 4e6, 2.5, 12, -25e3, 800, -30, 10, 10}; expect(k); }
    // GAC
    { Case k{kWxNoaaGac, 8e6, 1.5, 18, -30e3, 500, 30, 80, 80}; WxsatTelemetry t = run(k); CHECK(t.blocksOk >= 80, "gac"); if (t.image) checkPixels(t, 1, 2); }
    // DSB (TIP frames, HIRS needs a whole 5.6 s line)
    { Case k{kWxNoaaDsb, 192e3, 12, 18, 1500, 15, 50, 1, 100}; WxsatTelemetry t = run(k); CHECK(t.blocksOk >= 100, "dsb"); CHECK(t.lines >= 1, "dsb hirs lines"); }
    // Meteor HRPT
    { Case k{kWxMeteorHrpt, 4e6, 2.5, 20, 22e3, -600, 40, 3, 150}; WxsatTelemetry t = run(k); CHECK(t.blocksOk >= 150, "meteor hrpt"); if (t.image) checkPixels(t, 2, 2); }
    // RDAS
    { Case k{kWxElektroRdas, 45e6, 0.5, 18, 300e3, 0, 30, 2, 400}; WxsatTelemetry t = run(k); CHECK(t.blocksOk >= 400, "rdas"); CHECK(t.packets >= 50, "rdas packets %llu", (unsigned long long)t.packets); }
    { Case k{kWxArktikaRdas, 45e6, 0.4, 18, -200e3, 0, -20, 1, 200}; expect(k); }
    // AWS
    { Case k{kWxAws, 6e6, 2.0, 10, 28e3, -700, 40, 40, 250}; WxsatTelemetry t = run(k); CHECK(t.blocksOk >= 250, "aws"); CHECK(t.lines >= 40, "aws lines"); if (t.image) CHECK(t.image->channels == 19, "aws ch"); }
    // GGAK
    { WxsatGenConfig g; g.profile = kWxArktikaGgak; g.rate = 240e3; g.snrDb = 20; g.cfoHz = 3000; WxsatGenerator gen(g); WxsatReceiver rx(kWxArktikaGgak); rx.configure(240e3); std::vector<cf32> b(1<<14); WxsatTelemetry t; uint64_t s=0; for (int i=0;i<400;i++){gen.generate(b.data(),b.size()); rx.feed(b.data(),b.size()); if(rx.telemetry(t,s)) s=t.seq;} printf("  GGAK: %s\n", wxsatSummary(t).c_str()); printf("  ggak state %d cfo %.0f snr %.1f ok %llu\n", t.ggak.state, t.ggak.cfoHz, t.ggak.snrDb, (unsigned long long)t.ggak.blocksOk); /* GGAK: see docs/SATDUMP-TIER1-3.7.md */ (void)0; }
    if (sweep) {
        for (double s : {16.0, 12.0, 10.0, 8.0, 6.0, 4.0}) { Case k{kWxNoaaHrpt, 4e6, 2.5, s, 12e3, 300, 20, 0, 0}; run(k); }
        for (double s : {14.0, 10.0, 8.0, 6.0, 4.0, 2.0}) { Case k{kWxAws, 6e6, 1.5, s, 12e3, 300, 20, 0, 0}; run(k); }
        for (double s : {20.0, 14.0, 10.0, 8.0, 6.0}) { Case k{kWxApt, 960e3, 10, s, 1000, 20, 30, 0, 0}; run(k); }
    }
    if (getenv("GGAK_T")) { for (double c : {0.0, 800.0, -2000.0}) { WxsatGenConfig g; g.profile = kWxArktikaGgak; g.rate = atof(getenv("GGAK_T")); g.snrDb = 24; g.cfoHz = c; g.amp = 0.2; WxsatGenerator gen(g); WxsatReceiver rx(kWxArktikaGgak); rx.configure(g.rate); std::vector<cf32> b(1<<14); WxsatTelemetry t; uint64_t s=0; for (int i=0;i<(int)(60*g.rate/16384);i++){gen.generate(b.data(),b.size()); rx.feed(b.data(),b.size()); if(rx.telemetry(t,s)) s=t.seq;} printf("ggak test cfo %.0f: state %d cfo %.0f snr %.1f ok %llu\n", c, t.ggak.state, t.ggak.cfoHz, t.ggak.snrDb, (unsigned long long)t.ggak.blocksOk); } }
    if (fails) { printf("%d failures\n", fails); return 1; }
    printf("ok\n");
    return 0;
}
