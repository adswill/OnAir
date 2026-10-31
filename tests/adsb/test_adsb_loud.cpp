// ADS-B loud-message counter and the coverage by altitude band. Synthetic Mode S extended squitters (AdsbMixer) at different pulse levels, with the radio's
// noise, a carrier offset and the 2.4 Msps rate of an RTL-SDR, go through the real receiver: the loud counter (level above -7 dBFS, a peak within about 3 dB of full scale, what the automatic
// gain reads) must follow the level, whatever the offset. The coverage part feeds positions at known ranges, bearings and heights.
#include "dect2/adsb/adsb_encode.h"
#include "dect2/adsb/adsb_gen.h"
#include "dect2/adsb/adsb_rx.h"
#include "dect2/adsb/adsb_track.h"
#include "../common/impair.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
using namespace dect2;
using V = std::vector<cf32>;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

struct Out { uint64_t ok = 0, loud = 0; };
static Out run(double rate, float amp, int n, double cfoHz, double snrDb) {
    AdsbMixer mx(rate);
    for (int k = 0; k < n; k++) {
        AdsbTx t;
        t.t = 0.001 + 0.001 * k + 1e-7 * (k % 7);
        t.frame = adsb::encodeIdentification(0x400000 + (uint32_t)(k % 24), 5, 4, 3, "TST" + std::to_string(100 + k % 900));
        t.amp = amp; t.phase = 0.37f * (float)k;
        mx.add(t);
    }
    V x((size_t)(rate * (0.003 + 0.001 * n)));
    mx.render(x.data(), x.size());
    if (cfoHz != 0) impair::shift(x, cfoHz, rate);
    impair::noise(x, snrDb, 7);
    AdsbReceiver rx;
    rx.configure(rate);
    if (getenv("LV")) rx.setFrameCallback([](const AdsbFrame& f) { static int k = 0; if (k++ % 100 == 0) printf("level %.1f\n", f.levelDbfs); });
    AdsbTelemetry t; uint64_t seq = 0;
    for (size_t i = 0; i < x.size(); i += 65536) {
        rx.feed(x.data() + i, std::min<size_t>(65536, x.size() - i));
        AdsbTelemetry m; if (rx.telemetry(m, seq)) { seq = m.seq; t = m; }
    }
    V z((size_t)(rate * 0.05));
    for (int k = 0; k < 20; k++) { rx.feed(z.data(), z.size()); AdsbTelemetry m; if (rx.telemetry(m, seq)) { seq = m.seq; t = m; } }
    return {t.blocksOk, t.loudMsgs};
}

static void testLoud() {
    const int n = 300;
    for (double rate : {2.0e6, 2.4e6})
        for (double cfo : {0.0, 55e3, -55e3}) {
            const Out hot = run(rate, 0.9f, n, cfo, 35), mid = run(rate, 0.5f, n, cfo, 35), quiet = run(rate, 0.1f, n, cfo, 30);
            printf("%.1f Msps cfo %+.0f kHz: amp 0.9 %llu/%llu loud, 0.5 %llu/%llu loud, 0.1 %llu/%llu loud\n", rate / 1e6, cfo / 1e3, (unsigned long long)hot.loud, (unsigned long long)hot.ok,
                   (unsigned long long)mid.loud, (unsigned long long)mid.ok, (unsigned long long)quiet.loud, (unsigned long long)quiet.ok);
            CHECK(hot.ok >= (uint64_t)(n * 0.9) && hot.loud >= hot.ok * 9 / 10, "hot: %llu of %llu loud", (unsigned long long)hot.loud, (unsigned long long)hot.ok);   // 0.9 amplitude = -0.9 dBFS
            CHECK(mid.ok >= (uint64_t)(n * 0.9) && mid.loud == 0, "0.5 amplitude (-6 dBFS) is not loud: %llu", (unsigned long long)mid.loud);
            CHECK(quiet.ok >= (uint64_t)(n * 0.8) && quiet.loud == 0, "quiet: %llu loud", (unsigned long long)quiet.loud);
            CHECK(hot.loud <= hot.ok, "loud <= good");
        }
}

static AdsbExtUpdate at(uint32_t icao, double lat, double lon, int alt, bool ground = false) {
    AdsbExtUpdate u; u.icao = icao; u.hasPos = true; u.lat = lat; u.lon = lon;
    if (ground) { u.hasGround = true; u.ground = true; } else { u.hasAlt = true; u.altFt = alt; }
    return u;
}

static void testCoverage() {
    CHECK(adsbAltBand(true, 0, false) == 0 && adsbAltBand(true, 9999, false) == 0 && adsbAltBand(true, 10000, false) == 1 && adsbAltBand(true, 35000, false) == 3 &&
          adsbAltBand(true, 40000, false) == 4 && adsbAltBand(true, 99000, false) == 4 && adsbAltBand(true, -300, false) == 0, "band edges");
    CHECK(adsbAltBand(false, 0, false) == -1 && adsbAltBand(true, 30000, true) == 0, "no altitude, ground");
    AdsbTracker tr;
    tr.setReference(52.0, 4.0);
    tr.applyExternal(at(0x111111, 53.0, 4.0, 41000), 1, 1.0);     // 60 nm north, 41000 ft: band 4
    tr.applyExternal(at(0x222222, 52.0, 5.0, 12000), 1, 2.0);     // ~37 nm east, band 1
    tr.applyExternal(at(0x333333, 52.5, 4.0, 3000), 1, 3.0);      // 30 nm north, band 0
    tr.applyExternal(at(0x444444, 51.0, 4.0, 33000), 1, 4.0);     // 60 nm south, band 3
    const AdsbRangeStats& s = tr.stats();
    CHECK(std::fabs(s.sectorMaxNm[0] - 60.0) < 1.0, "all heights north %.1f", s.sectorMaxNm[0]);
    CHECK(std::fabs(s.sectorBandMaxNm[4][0] - 60.0) < 1.0 && s.sectorBandMaxNm[0][0] > 29 && s.sectorBandMaxNm[0][0] < 31, "north: high band %.1f low band %.1f", s.sectorBandMaxNm[4][0], s.sectorBandMaxNm[0][0]);
    CHECK(std::fabs(s.sectorBandMaxNm[1][8] - 36.9) < 1.0 && s.sectorBandMaxNm[0][8] == 0 && s.sectorBandMaxNm[4][8] == 0, "east in band 1 only");
    CHECK(std::fabs(s.sectorBandMaxNm[3][18] - 60.0) < 1.0, "south in band 3: %.1f", s.sectorBandMaxNm[3][18]);
    // the all-heights figure is the maximum over the bands plus the positions with no altitude
    for (int i = 0; i < kAdsbSectors; i++) {
        float mx = 0;
        for (int b = 0; b < kAdsbAltBands; b++) mx = std::max(mx, s.sectorBandMaxNm[b][i]);
        CHECK(mx <= s.sectorMaxNm[i] + 1e-3f, "sector %d: band %.1f > all %.1f", i, mx, s.sectorMaxNm[i]);
    }
    // a position with no known altitude counts for the all-heights plot only
    AdsbExtUpdate u; u.icao = 0x555555; u.hasPos = true; u.lat = 52.0; u.lon = 3.0;   // ~37 nm west
    tr.applyExternal(u, 1, 5.0);
    const AdsbRangeStats& s2 = tr.stats();
    CHECK(s2.sectorMaxNm[27] > 30, "west, no altitude: all %.1f", s2.sectorMaxNm[27]);
    float bandsW = 0; for (int b = 0; b < kAdsbAltBands; b++) bandsW += s2.sectorBandMaxNm[b][27];
    CHECK(bandsW == 0, "no altitude is in no band: %.1f", bandsW);
    // survives a save, empties on reset
    AdsbTracker tr2;
    CHECK(tr2.loadResults(tr.saveResults(), 10.0), "load");
    CHECK(std::fabs(tr2.stats().sectorBandMaxNm[4][0] - 60.0) < 1.0 && std::fabs(tr2.stats().sectorBandMaxNm[3][18] - 60.0) < 1.0, "bands survive a save");
    tr2.resetStats();
    CHECK(tr2.stats().sectorBandMaxNm[4][0] == 0, "reset");
    // the export
    const std::string csv = adsbCoverageCsv(tr.stats());
    int lines = 0; for (char c : csv) if (c == '\n') lines++;
    CHECK(lines == 1 + (kAdsbAltBands + 1) * kAdsbSectors, "csv lines %d", lines);
    CHECK(csv.rfind("band,from_deg,to_deg,max_range_nm,max_range_km\n", 0) == 0, "csv header");
    CHECK(csv.find("40000 ft and above,0,10,60.") != std::string::npos, "csv north high band row");   // %s
    CHECK(csv.find("all heights,0,10,60.") != std::string::npos, "csv all-heights row");
    // no reference: no ranges
    AdsbTracker tr3;
    tr3.applyExternal(at(0x666666, 53.0, 4.0, 20000), 1, 1.0);
    float sum = 0; for (int b = 0; b < kAdsbAltBands; b++) for (int i = 0; i < kAdsbSectors; i++) sum += tr3.stats().sectorBandMaxNm[b][i];
    CHECK(sum == 0, "without a reference position there are no ranges");
}

int main() {
    testCoverage();
    testLoud();
    printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return fails ? 1 : 0;
}
