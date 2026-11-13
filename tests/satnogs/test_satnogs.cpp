// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// SatNOGS DB: the parsers (a small hand-written sample in the shape of the API answers), the mode mapping, the filters and the cache age.
#include "dect2/net/satnogs.h"
#include <cstdio>
using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static const char* kSats = R"([
 {"sat_id":"AAAA-0001-0001-0001-0001","norad_cat_id":99001,"name":"TESTSAT-1","status":"alive"},
 {"sat_id":"AAAA-0002","norad_cat_id":99002,"name":"Café \"Two\"","status":"alive"},
 {"norad_cat_id":null,"name":"No Number"}
])";
static const char* kTx = R"([
 {"uuid":"u1","description":"Mode V 1k2 AFSK","alive":true,"type":"Transmitter","uplink_low":145850000,"uplink_high":null,
  "downlink_low":437800000,"downlink_high":null,"mode":"AFSK1k2","mode_id":3,"baud":1200.0,"sat_id":"AAAA","norad_cat_id":99001,
  "status":"active","service":"Amateur","params":{"nested":[1,2,{"x":"y"}]}},
 {"uuid":"u2","description":"CW beacon","alive":true,"downlink_low":145900000,"mode":"CW","baud":null,"norad_cat_id":99002,"status":"active"},
 {"uuid":"u3","description":"Old GMSK","alive":false,"downlink_low":435325000,"mode":"GMSK9k6","baud":9600,"norad_cat_id":99001,"status":"invalid"},
 {"uuid":"u4","description":"Unknown sat","alive":true,"downlink_low":137100000,"mode":"LRPT","norad_cat_id":99777,"status":"active"},
 {"uuid":"u5","description":"No status field","alive":true,"downlink_low":None}
])";

int main() {
    std::map<int, std::string> names;
    CHECK(parseSatnogsSatellites(kSats, names) == 2, "satellites");
    CHECK(names[99001] == "TESTSAT-1", "name");
    CHECK(names[99002] == "Caf\xC3\xA9 \"Two\"", "escapes: %s", names[99002].c_str());
    CHECK(parseSatnogsSatellites("not json", names) == 0 && parseSatnogsSatellites("{\"a\":1}", names) == 0 && parseSatnogsSatellites("[{", names) == 0, "garbage");

    std::vector<SatnogsTx> tx;
    // the sample's last entry is deliberately invalid JSON (None): the whole answer is refused, not half read
    CHECK(parseSatnogsTransmitters(kTx, names, tx) == 0 && tx.empty(), "invalid refused");
    std::string good = kTx;
    good.replace(good.find("None"), 4, "null");
    CHECK(parseSatnogsTransmitters(good, names, tx) == 5 && tx.size() == 5, "five read: %zu", tx.size());
    if (tx.size() == 5) {
        CHECK(tx[0].satName == "TESTSAT-1" && tx[0].norad == 99001 && tx[0].downHz == 437800000 && tx[0].upHz == 145850000 && tx[0].baud == 1200 && tx[0].alive && tx[0].mode == "AFSK1k2", "tx0");
        CHECK(tx[1].baud == 0 && tx[1].upHz == 0 && tx[1].alive, "tx1 nulls");
        CHECK(!tx[2].alive && tx[2].status == "invalid", "tx2 dead");
        CHECK(tx[3].satName == "NORAD 99777", "unknown sat: %s", tx[3].satName.c_str());
        CHECK(tx[4].downHz == 0 && tx[4].norad == 0, "tx4 empty");
    }

    // the mode map
    struct { const char* m; int fam; } map[] = {{"FM", 5}, {"FMN", 5}, {"AFSK1k2", 24}, {"AFSK", 24}, {"BPSK1k2", 61}, {"BPSK PMT-A3", 61}, {"GMSK9k6", 61}, {"GFSK9k6", 61},
        {"FSK9k6", 61}, {"MSK2k4", 61}, {"CW", 34}, {"LRPT", 59}, {"APT", 41}, {"SSTV", 0}, {"LoRa", 0}, {"USB", 0}, {"", 0}, {"cw", 34}};
    for (const auto& e : map) CHECK(satnogsFamily(e.m, 0) == e.fam, "mode %s -> %d", e.m, satnogsFamily(e.m, 0));

    // filters
    SatnogsFilter f;   // alive only
    int n = 0;
    for (const auto& t : tx) n += satnogsMatches(t, f);
    CHECK(n == 4, "alive only (u5 has no status, counts as alive): %d", n);
    f.aliveOnly = false; n = 0;
    for (const auto& t : tx) n += satnogsMatches(t, f);
    CHECK(n == 5, "all: %d", n);
    f.aliveOnly = true; f.minMhz = 144; f.maxMhz = 146;
    n = 0; for (const auto& t : tx) n += satnogsMatches(t, f);
    CHECK(n == 1, "2 m band: %d", n);
    f = SatnogsFilter(); f.modeText = "afsk";
    n = 0; for (const auto& t : tx) n += satnogsMatches(t, f);
    CHECK(n == 1, "mode text: %d", n);
    f = SatnogsFilter(); f.text = "testsat";
    n = 0; for (const auto& t : tx) n += satnogsMatches(t, f);
    CHECK(n == 1, "name text (the dead GMSK one is hidden): %d", n);

    // cache age
    CHECK(satnogsCacheStale(1000, 0) && !satnogsCacheStale(100000, 99000) && satnogsCacheStale(200000, 99000) && satnogsCacheStale(1000, 5000), "cache age");
    if (!fails) printf("satnogs: ok\n");
    return fails ? 1 : 0;
}
