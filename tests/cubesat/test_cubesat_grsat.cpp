// The gr-satellites satellite table (cubesat_grsat.h) and the pass auto-setup of the CubeSat receiver:
//  - the table: size, known satellites (GOMX-1 U482C AFSK, FUNcube AO-40, BEESAT-9, LilacSat-2 CCSDS concatenated parameters), every row sane
//  - the receiver following a satellite whose baud rate the fixed 9600 / 4800 demodulators do not cover (a USP satellite at 2400 baud, the NGHam
//    FloripaSat-1 at 2400): its frames decode only once the pass planner names it (setTracked), with a carrier offset and noise
// Simulated signals only (GMSK made here, through the FM channel at 2 Msps).
#include "dect2/cubesat/cubesat_grsat.h"
#include "dect2/cubesat/cubesat_more.h"
#include "dect2/cubesat/cubesat_rx.h"
#include "dect2/cubesat/cubesat_tel.h"
#include "dect2/cubesat/cubesat_telem.h"
#include "dect2/dsp/gen_util.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>
using namespace dect2;
using namespace dect2::cubesat;
#include "../common/test_util.h"

static const double kPi = 3.14159265358979323846;

// GMSK (BT 0.5) FM at `rate` around offset Hz: 0.3 s of silence, a 0101 preamble, the symbols, a tail
static std::vector<cf32> gmsk(const std::vector<uint8_t>& sym, int baud, double dev, double rate, double offset, double snrDb) {
    std::vector<uint8_t> lv;
    for (int i = 0; i < 400; i++) lv.push_back((uint8_t)(i & 1));
    lv.insert(lv.end(), sym.begin(), sym.end());
    for (int i = 0; i < 400; i++) lv.push_back((uint8_t)(i & 1));
    const double sps = rate / baud;
    const double sigma = std::sqrt(std::log(2.0)) / (2 * kPi * 0.5 * baud) * rate;
    const int half = (int)std::ceil(3 * sigma);
    std::vector<double> g((size_t)(2 * half + 1));
    double sum = 0;
    for (int i = -half; i <= half; i++) { g[(size_t)(i + half)] = std::exp(-0.5 * i * i / (sigma * sigma)); sum += g[(size_t)(i + half)]; }
    const long lead = (long)(0.3 * rate);
    const long n = (long)(lv.size() * sps);
    std::vector<float> nrz((size_t)n);
    for (long i = 0; i < n; i++) nrz[(size_t)i] = lv[(size_t)(i / sps)] ? 1.f : -1.f;
    std::vector<cf32> out((size_t)(lead + n + lead), cf32(0, 0));
    double th = 0;
    for (size_t i = 0; i < out.size(); i++) {
        double f = offset;
        const long k = (long)i - lead;
        if (k >= 0 && k < n) {
            double acc = 0;
            for (int j = -half; j <= half; j += 4) { const long q = std::max(0L, std::min(n - 1, k + j)); acc += g[(size_t)(j + half)] * nrz[(size_t)q]; }
            f += dev * acc * 4 / sum;
        }
        th += 2 * kPi * f / rate;
        const bool on = k >= -1000 && k < n + 1000;
        out[i] = on ? cf32((float)std::cos(th), (float)std::sin(th)) * 0.3f : cf32(0, 0);
    }
    genutil::NoiseSource ns(5);
    const double pn = 0.09 * rate / 25000.0 / std::pow(10.0, snrDb / 10.0);
    ns.add(out.data(), out.size(), (float)std::sqrt(pn / 2));
    return out;
}

static std::vector<uint8_t> msb(const std::vector<uint8_t>& d) { std::vector<uint8_t> b; for (uint8_t v : d) for (int k = 7; k >= 0; k--) b.push_back((uint8_t)((v >> k) & 1)); return b; }

static int run(const std::vector<cf32>& x, double rate, int norad, const char* name, int framing, const std::vector<uint8_t>& want, std::string* setup, double dial = 0) {
    CubesatReceiver r;
    r.configure(rate);
    r.setDialHz(dial);
    r.setSignalOffset(-cubesatTuning().tuneOffsetHz);
    if (norad) r.setTracked(norad, name);
    for (size_t i = 0; i < x.size(); i += 16384) r.feed(x.data() + i, std::min((size_t)16384, x.size() - i));
    CubeTelemetry t;
    r.telemetry(t, 0);
    if (setup) *setup = t.trackedSetup;
    int n = 0;
    for (const CubeFrame& f : t.frames) n += f.framing == framing && f.data == want;
    return n;
}

int main() {
    const std::vector<GrTx>& T = grsatTable();
    printf("gr-satellites table: %zu transmitters\n", T.size());
    CHECK(T.size() > 600, "table size %zu", T.size());
    int bad = 0, rx = 0;
    for (const GrTx& t : T) { bad += t.norad < 0 || t.hz < 0 || t.baud < 0 || !t.sat || !*t.sat; rx += grsatReceiverDecodes(t); }
    printf("  receiver decodes %d of them\n", rx);
    CHECK(bad == 0 && rx > 400, "rows: %d bad, %d decodable", bad, rx);
    const GrTx* g = grsatChoose(39430, 0);
    CHECK(g && grsatFraming(*g) == U482c && g->afCarrier == 3600 && grsatFieldDecoder(*g) == "GOMX-1", "GOMX-1: U482C AFSK 3600, GOMX-1 fields");
    bool fc = false;
    for (const GrTx* t : grsatFor(39444)) fc |= grsatFraming(*t) == kFramingAo40 && grsatFieldDecoder(*t) == "FUNcube";
    CHECK(fc, "FUNcube-1 (AO-73): AO-40 FEC with FUNcube fields");
    const GrTx* b9 = grsatChoose(44412, 0);
    CHECK(b9 && grsatMobitexVariant(*b9) == MobitexBeesat9, "BEESAT-9: Mobitex variant");
    const GrTx* l2 = grsatChoose(40908, 437.225e6);
    CHECK(l2 && grsatFraming(*l2) == CcsdsConcat && grsatConcat(*l2).frameSize == 114 && !grsatConcat(*l2).dualBasis, "LilacSat-2 4k8 FSK: CCSDS concatenated, 114 bytes, conventional basis");

    const double R = cubesatTuning().sampleRate;
    {   // a USP satellite at 2400 baud (ARCCUBE-1): decoded only when the pass planner names it
        std::vector<uint8_t> ax = {0x82, 0xA0, 0xA4, 0xA6, 0x40, 0x40, 0x60, 0xA4, 0xA6, 0x60, 0x82, 0x86, 0x40, 0x61, 0x03, 0xF0};
        const char* txt = "USP at 2400 baud, the table sets the receiver up";
        ax.insert(ax.end(), txt, txt + strlen(txt));
        const std::vector<cf32> x = gmsk(uspEncode(ax), 2400, 600, R, -cubesatTuning().tuneOffsetHz + 1800, 24);
        std::string setup;
        const int with = run(x, R, 57205, "ARCCUBE-1", Usp, ax, &setup), without = run(x, R, 0, "", Usp, ax, nullptr);
        printf("  USP 2400 baud: tracked %d, untracked %d; setup \"%s\"\n", with, without, setup.c_str());
        CHECK(with == 1 && without == 0 && setup.find("USP") != std::string::npos, "USP 2400: tracked %d untracked %d", with, without);
    }
    {   // NGHam, FloripaSat-1 2k4
        const char* txt = "FloripaSat-1 NGHam beacon";
        const std::vector<uint8_t> pl(txt, txt + strlen(txt));
        const std::vector<cf32> x = gmsk(msb(nghamEncode(pl, 0, false)), 2400, 1200, R, -cubesatTuning().tuneOffsetHz - 2500, 22);
        const int with = run(x, R, 44885, "FloripaSat-1", Ngham, pl, nullptr, 436.1e6);
        printf("  NGHam 2400 baud: tracked %d\n", with);
        CHECK(with == 1, "NGHam 2400 tracked: %d", with);
    }
    return testSummary("cubesat_grsat");
}
