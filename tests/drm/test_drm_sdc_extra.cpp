// DRM SDC data entities 2, 3, 4, 6, 7, 14 from hand-built fields (written with the encoder's sdcPutEntity), and that they go through sdcParse.
#include "dect2/drm/drm_msg.h"
#include <cstdio>
using namespace dect2::drm;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)
int main() {
    std::vector<uint8_t> f;
    sdcPutEntity(f, 4, 0, {0x30, 0xD0 >> 0, 0x40, 0x00, 0x3C});                 // schedule 3: day code, start and duration
    sdcPutEntity(f, 7, 0, {0x20, 0x31, 0x08, 0x80 | 10, 20, 45, 46});           // region 2 at 49 N, 17 E, extents 10, 20, CIRAF 45 46
    sdcPutEntity(f, 6, 0, {0xA0, 0x40 | 0x2 << 4, 0x7C, 0x03 << 0});            // announcements
    sdcPutEntity(f, 14, 0, {0x10, 20, 60, 100});                                // packet FEC: stream 0, R 20, C 60, length 100
    sdcPutEntity(f, 2, 0, {0x60, 0x12, 0x34});                                  // CA, short id 1, audio
    sdcPutEntity(f, 3, 0, {0xB0, 0x50, 0x55, 0x1E, 0xA0});                            // sync + flags: region / schedule 5/5, 7840 kHz
    sdcPutEntity(f, 1, 0, {0x00, 'H', 'i'});
    SdcInfo in; int n = sdcParse(f.data(), (int)f.size(), in);
    const auto& e = in.extra;
    CHECK(n >= 7, "entities %d", n);
    CHECK(e.schedules.size() == 1 && e.schedules[0].id == 3 && e.schedules[0].dayCode == (0xD0 >> 1) && e.schedules[0].durationMin == 60, "schedule %zu", e.schedules.size());
    CHECK(e.regions.size() == 1 && e.regions[0].id == 2 && e.regions[0].latDeg == 49 && e.regions[0].lonDeg == 17 && e.regions[0].latExt == 10 && e.regions[0].lonExt == 20 && e.regions[0].ciraf.size() == 2, "region lat %d lon %d", e.regions.empty() ? 0 : e.regions[0].latDeg, e.regions.empty() ? 0 : e.regions[0].lonDeg);
    CHECK(e.announcements.size() == 1 && e.announcements[0].shortFlags == 0xA, "announcement");
    CHECK(e.packetFec.size() == 1 && e.packetFec[0].r == 20 && e.packetFec[0].c == 60 && e.packetFec[0].packetLen == 100, "fec");
    CHECK(e.ca.size() == 1 && e.ca[0].shortId == 1 && e.ca[0].audio && e.ca[0].info.size() == 2, "ca");
    CHECK(e.altFreq.size() == 1 && e.altFreq[0].shortFlags == 5 && e.altFreq[0].region == 5 && e.altFreq[0].schedule == 5 && !e.altFreq[0].freqKhz.empty() && e.altFreq[0].freqKhz[0] == 0x1EA0, "alt freq");
    { std::vector<uint8_t> g; sdcPutEntity(g, 4, 1, {0x30, 0, 0, 0, 0}); SdcExtra x; sdcParseExtra(g.data(), (int)g.size(), x); CHECK(x.schedules.empty(), "next-version entities are not current"); }
    printf(fails ? "drm sdc extra FAILED\n" : "drm sdc extra passed\n");
    return fails ? 1 : 0;
}
