// Telemetry parsing: the real FUNcube-1 frame of the gr-satellites test vector (WO10 of AO-73: battery 8140 mV), frame type names, text beacons,
// and the frames that must not parse.
#include "dect2/cubesat/cubesat_telem.h"
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>
using namespace dect2::cubesat;
#include "../common/test_util.h"

#ifndef DECT2_SOURCE_DIR
#define DECT2_SOURCE_DIR "."
#endif

static const TelField* find(const TelResult& r, const std::string& g, const std::string& n) {
    for (const TelField& f : r.fields) if (f.group == g && f.name == n) return &f;
    return nullptr;
}

int main() {
    std::ifstream fi(std::string(DECT2_SOURCE_DIR) + "/tests/data/ao40/frame.bin", std::ios::binary);
    const std::vector<uint8_t> fr((std::istreambuf_iterator<char>(fi)), std::istreambuf_iterator<char>());
    CHECK(fr.size() == 256, "frame present");
    TelResult r;
    CHECK(parseTelemetry(kFramingAo40, fr, {}, r), "real frame parses");
    CHECK(r.satellite == "FUNcube-1 (AO-73)", "satellite %s", r.satellite.c_str());
    CHECK(r.kind.find("WO10") != std::string::npos, "kind %s", r.kind.c_str());
    const TelField* bv = find(r, "EPS", "Battery voltage");
    CHECK(bv && bv->numeric && bv->num == 8140, "battery voltage %.0f", bv ? bv->num : -1.0);
    const TelField* seq = find(r, "Software", "Sequence number");
    CHECK(seq != nullptr, "sequence number");
    CHECK(find(r, "RF", "RF temperature") && find(r, "PA", "Forward power") && find(r, "Antennas", "Antenna 1 deployed"), "fields of RF, PA, antennas");
    printf("%s, %zu fields, sequence %.0f, battery %s\n", r.kind.c_str(), r.fields.size(), seq ? seq->num : -1.0, bv ? bv->text.c_str() : "?");
    // header variants: every frame type name
    for (int satid = 0; satid < 3; satid++)
        for (int t = 0; t < 24; t++) {
            std::vector<uint8_t> f(256, 0);
            f[0] = (uint8_t)(satid << 6 | t);
            TelResult x;
            CHECK(parseFuncube(f, x) && !x.fields.empty(), "header sat %d type %d", satid, t);
        }
    { std::vector<uint8_t> f(256, 0); f[0] = (uint8_t)(3 << 6); f[1] = 0x08; TelResult x; CHECK(parseFuncube(f, x) && x.satellite == "extended", "Nayif-1 header"); }
    { std::vector<uint8_t> f(256, 0); f[0] = (uint8_t)(3 << 6); f[1] = 0x09; TelResult x; CHECK(!parseFuncube(f, x), "unknown extended id"); }
    { std::vector<uint8_t> f(255, 0); TelResult x; CHECK(!parseFuncube(f, x), "wrong length"); }
    { std::vector<uint8_t> f(256, 0); f[0] = 63; TelResult x; CHECK(!parseFuncube(f, x), "frame type out of range"); }
    // text beacons
    CHECK(beaconText({'h', 'i', ' ', 'h', 'i'}) == "", "short text is no beacon");
    const std::string s = "ES5E  hello from space 12.3V 21C";
    CHECK(beaconText(std::vector<uint8_t>(s.begin(), s.end())) == s, "plain text");
    std::vector<uint8_t> mix = {1, 2, 3, 200};
    mix.insert(mix.end(), s.begin(), s.end());
    mix.push_back(0xFF);
    CHECK(beaconText(mix) == s, "text inside binary");
    std::vector<uint8_t> sym(40, '#');
    CHECK(beaconText(sym) == "", "symbols only are no text");
    TelResult t;
    CHECK(parseTelemetry(0, {}, std::vector<uint8_t>(s.begin(), s.end()), t) && t.kind == "text beacon", "text beacon through parseTelemetry");
    return testSummary("cubesat telem");
}
