// SatDump tier 3 links: each generator through its receiver, with the impairments that matter. Everything here is simulated signals (docs/SATDUMP-TIER3-3.8.md).
#include "dect2/sd3/sd3_gen.h"
#include "dect2/sd3/sd3_proto.h"
#include "dect2/sd3/sd3_rx.h"
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace dect2;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("  FAIL %s: ", #c); printf(__VA_ARGS__); printf("\n"); } } while (0)

struct Res { Sd3Telemetry t; };

static Res run(int profile, int link, double rate, double seconds, double snr, double cfo, double ppm = 0, double drift = 0, double startOff = 0, bool invert = false) {
    Sd3GenConfig g;
    g.profile = profile; g.link = link; g.rate = rate; g.snrDb = snr; g.cfoHz = cfo; g.clockPpm = ppm; g.driftHzPerS = drift; g.invert = invert; g.phaseDeg = 33;
    Sd3Generator gen(g);
    Sd3Receiver rx(profile);
    rx.setLink(link);
    rx.setBlocking(true);
    if (getenv("SD3_LOG")) rx.setLogCallback([](const std::string& s) { printf("%s\n", s.c_str()); });
    rx.configure(rate);
    if (startOff != 0) rx.setSignalOffset(startOff);
    std::vector<cf32> buf(16384);
    const size_t total = (size_t)(rate * seconds);
    for (size_t at = 0; at < total;) { const size_t n = std::min(buf.size(), total - at); gen.generate(buf.data(), n); rx.feed(buf.data(), n); at += n; }
    if (getenv("SD3_LOG")) printf("fed %zu samples, flushing\n", total);
    rx.flush();
    Res r;
    rx.telemetry(r.t, ~0ull);
    return r;
}

static void show(const char* what, const Res& r) {
    printf("%-32s frames ok %6llu bad %5llu rs %5llu crc %llu pkts %6llu files %4llu lines %5llu snr %5.1f cfo %9.1f state %d %s\n", what, (unsigned long long)r.t.blocksOk, (unsigned long long)r.t.blocksBad, (unsigned long long)r.t.rsCorrected, (unsigned long long)r.t.crcBad, (unsigned long long)r.t.packets, (unsigned long long)r.t.files, (unsigned long long)r.t.lines, r.t.snrDb, r.t.cfoHz, r.t.state, r.t.lastText.empty() ? r.t.hint.c_str() : r.t.lastText.c_str());
    fflush(stdout);
}

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IOLBF, 0);
    const std::string only = argc > 1 ? argv[1] : "";
    auto want = [&](const char* s) { return only.empty() || only == s; };
    if (want("gvar")) {
        auto r = run(kSd3Gvar, 0, 6e6, 2.5, 25, 3000);  show("GVAR 25 dB +3 kHz", r);
        CHECK(r.t.blocksOk >= 12, "gvar frames %llu", (unsigned long long)r.t.blocksOk);
        CHECK(r.t.image2 && r.t.image2->lines >= 1 && r.t.image, "gvar pictures");
    }
    if (want("cda")) {
        auto r = run(kSd3Cda, 0, 1e6, 3, 12, 2000); show("CDA 40k 12 dB", r); CHECK(r.t.blocksOk >= 30, "cda %llu", (unsigned long long)r.t.blocksOk);
        r = run(kSd3Cda, 2, 250000, 12, 15, 200); show("CDA GOES-N PM", r); CHECK(r.t.blocksOk >= 4, "cda-n %llu", (unsigned long long)r.t.blocksOk);
    }
    if (want("mdl")) {
        auto r = run(kSd3Mdl, 0, 3e6, 2, 18, -4000); show("MDL 18 dB -4 kHz", r); CHECK(r.t.blocksOk >= 40, "mdl %llu", (unsigned long long)r.t.blocksOk);
    }
    if (want("cdas")) {
        auto r = run(kSd3Cdas, 0, 8e6, 0.5, 18, 8000); show("CDAS 18 dB", r); CHECK(r.t.blocksOk >= 60, "cdas %llu", (unsigned long long)r.t.blocksOk);
    }
    if (want("geoscan")) {
        auto r = run(kSd3Geoscan, 0, 250000, 3, 22, 3000, 0, 0, 0); show("Geoscan 22 dB", r); CHECK(r.t.blocksOk >= 25, "geoscan %llu", (unsigned long long)r.t.blocksOk);
    }
    if (want("lucky7")) {
        auto r = run(kSd3Lucky7, 0, 250000, 3, 22, -2000); show("Lucky-7 22 dB", r); CHECK(r.t.blocksOk >= 25, "lucky7 %llu", (unsigned long long)r.t.blocksOk);
    }
    if (want("spino")) {
        auto r = run(kSd3Spino, 0, 250000, 3, 22, 1200); show("Spino 22 dB", r); CHECK(r.t.blocksOk >= 10, "spino %llu", (unsigned long long)r.t.blocksOk);
    }
    if (want("erminaz")) {
        auto r = run(kSd3Erminaz, 0, 250000, 3, 22, 1500); show("Erminaz 22 dB", r); CHECK(r.t.blocksOk >= 10, "erminaz %llu", (unsigned long long)r.t.blocksOk);
    }
    if (want("sband")) {
        int ok = 0, n = sd3LinkCount(kSd3Sband);
        for (int i = 0; i < n; i++) {
            const Sd3Link& l = sd3LinkAt(kSd3Sband, i);
            const double rate = std::max(4 * l.sym, 500000.0);
            const double frameSec = l.frameBits * (l.conv ? 2.0 : 1.0) / (l.mod == kSd3Qpsk || l.mod == kSd3Oqpsk ? 2.0 : 1.0) / l.sym;
            const double sec = std::min(2.0, 14 * frameSec) + 0.05;
            auto r = run(kSd3Sband, i, rate, sec, l.conv ? 10 : 15, 0.002 * l.sym);
            char b[80]; snprintf(b, sizeof b, "%s", l.name);
            show(b, r);
            CHECK(r.t.blocksOk >= 3, "link %s frames %llu bad %llu", l.id, (unsigned long long)r.t.blocksOk, (unsigned long long)r.t.blocksBad);
            if (r.t.blocksOk >= 3) ok++;
        }
        printf("S-band links decoded: %d of %d\n", ok, n);
    }
    if (want("grb")) {
        auto r = run(kSd3Grb, 0, 20e6, 1.0, 14, 10000); show("GRB 14 dB", r);
        CHECK(r.t.packets >= 50, "grb packets %llu", (unsigned long long)r.t.packets);
        CHECK(!r.t.products.empty(), "grb products");
        CHECK(r.t.image && r.t.image->lines >= 3, "grb picture");
    }
    if (want("uhrit")) {
        auto r = run(kSd3Uhrit, 0, 30e6, 0.4, 16, 10000); show("UHRIT 14 dB", r);
        CHECK(r.t.files >= 2, "uhrit files %llu", (unsigned long long)r.t.files);
        CHECK(r.t.lastText.find("IMG_TEST") != std::string::npos, "uhrit name %s", r.t.lastText.c_str());
    }
    printf("%s\n", fails ? "FAILED" : "PASS");
    return fails ? 1 : 0;
}
