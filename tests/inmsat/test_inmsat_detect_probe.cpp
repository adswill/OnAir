// Carrier detector on a real capture, search by search: what it makes of a band (for the C-channel pool, where neighbouring carriers sit
// 8 to 12 kHz apart). Does nothing (and passes) without DECT2_DETECT_CAPTURE (cf32, DECT2_DETECT_RATE default 10 Msps, DECT2_DETECT_CENTER
// MHz, DECT2_DETECT_LO / HI MHz: the part of the band to print).
#include "dect2/inmsat/inmsat_detect.h"
#include <cstdio>
#include <cstdlib>
#include <vector>
using namespace dect2;
using namespace dect2::inmsat;

#include "../common/test_util.h"

int main() {
    const char* path = getenv("DECT2_DETECT_CAPTURE");
    if (!path) { printf("inmsat_detect_probe: no DECT2_DETECT_CAPTURE, nothing to do\n"); return testSummary("inmsat_detect_probe"); }
    const double rate = getenv("DECT2_DETECT_RATE") ? atof(getenv("DECT2_DETECT_RATE")) : 10e6;
    const double centre = (getenv("DECT2_DETECT_CENTER") ? atof(getenv("DECT2_DETECT_CENTER")) : 1546.153) * 1e6;
    const double lo = (getenv("DECT2_DETECT_LO") ? atof(getenv("DECT2_DETECT_LO")) : 1546.09) * 1e6, hi = (getenv("DECT2_DETECT_HI") ? atof(getenv("DECT2_DETECT_HI")) : 1546.33) * 1e6;
    const double secs = getenv("DECT2_DETECT_SECONDS") ? atof(getenv("DECT2_DETECT_SECONDS")) : 20;
    FILE* f = fopen(path, "rb");
    if (!f) { printf("cannot open %s\n", path); return 1; }
    CarrierDetector det;
    DetectorConfig c; c.rate = rate;
    det.configure(c);
    std::vector<cf32> buf(1 << 18);
    size_t total = (size_t)(secs * rate), done = 0;
    uint64_t last = 0;
    while (done < total) {
        const size_t got = fread(buf.data(), sizeof(cf32), std::min(buf.size(), total - done), f);
        if (!got) break;
        det.feed(buf.data(), got);
        done += got;
        if (det.searches() != last) {
            last = det.searches();
            printf("search %llu at %.1f s, floor %.1f dB:\n", (unsigned long long)last, det.nowSec(), det.floorDb());
            for (const auto& k : det.carriers()) {
                const double fq = centre + k.offsetHz;
                if (fq < lo || fq > hi) continue;
                printf("   id %3d  %.4f MHz  width %5.1f kHz  snr %4.1f dB  %-6s  seen %llu%s\n", k.id, fq / 1e6, k.widthHz / 1e3, k.snrDb, carrierClassName(k.cls),
                       (unsigned long long)k.seen, k.confirmed ? "  confirmed" : "");
            }
            // the spectrum of that part of the band, 1 kHz steps
            std::vector<float> dB; double start = 0, binHz = 0;
            det.spectrum(dB, start, binHz);
            if (binHz > 0) {
                printf("   spectrum:");
                for (double fq = lo; fq <= hi; fq += 1000) {
                    const int j = (int)((fq - centre - start) / binHz);
                    if (j >= 0 && j < (int)dB.size()) printf(" %.0f", dB[(size_t)j]);
                }
                printf("\n");
            }
        }
    }
    fclose(f);
    return testSummary("inmsat_detect_probe");
}
