// ADS-B on a real recording: modes1.bin from the dump1090 test files (2 Msps, unsigned 8 bit IQ, 0.18 s, one aircraft, address 4D2023). Every message the
// receiver reports has passed the CRC; the file holds 57 short and 126 long ones.
#include "dect2/adsb/adsb_rx.h"
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <vector>
using namespace dect2;
#include "../common/test_util.h"

int main() {
    const std::string path = std::string(DECT2_SOURCE_DIR) + "/tests/data/adsb/modes1_2msps_cu8.bin";
    std::ifstream f(path, std::ios::binary);
    CHECK(f.good(), "cannot open %s", path.c_str());
    if (!f.good()) return testSummary("real adsb");
    std::vector<unsigned char> raw((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    CHECK(raw.size() == 713736, "%zu bytes", raw.size());
    std::vector<cf32> x(raw.size() / 2);
    for (size_t i = 0; i < x.size(); i++) x[i] = cf32(((int)raw[2 * i] - 127.5f) / 127.5f, ((int)raw[2 * i + 1] - 127.5f) / 127.5f);
    AdsbReceiver rx;
    rx.configure(2e6);
    CHECK(rx.ready(), "not ready at 2 Msps");
    int shortN = 0, longN = 0, df17 = 0, pos = 0, vel = 0, bad = 0;
    std::set<uint32_t> icaos;
    rx.setFrameCallback([&](const AdsbFrame& fr) {
        if (fr.bits == 56) shortN++; else longN++;
        if (fr.df == 17 || fr.df == 18) df17++;
        if (fr.msg.tc >= 9 && fr.msg.tc <= 18) pos++;
        if (fr.msg.tc == 19) vel++;
        if (fr.icao != 0x4D2023) bad++;
        icaos.insert(fr.icao);
    });
    rx.setLogCallback([](const std::string&) {});
    for (size_t at = 0; at < x.size(); at += 65536) rx.feed(x.data() + at, std::min<size_t>(65536, x.size() - at));
    std::vector<cf32> tail(2000, cf32(0, 0));
    rx.feed(tail.data(), tail.size());
    printf("  %d short, %d long, %d extended squitter, %d position, %d velocity, %zu address(es)\n", shortN, longN, df17, pos, vel, icaos.size());
    CHECK(longN >= 100, "%d long messages (the file holds 126)", longN);
    CHECK(shortN >= 40, "%d short messages (the file holds 57)", shortN);
    CHECK(df17 >= 100, "%d extended squitters", df17);
    CHECK(pos >= 20 && vel >= 5, "%d position and %d velocity messages", pos, vel);
    CHECK(icaos.size() == 1 && icaos.count(0x4D2023), "addresses: %zu", icaos.size());
    CHECK(bad == 0, "%d messages from another address (CRC-checked, so a false decode)", bad);
    return testSummary("real adsb");
}
