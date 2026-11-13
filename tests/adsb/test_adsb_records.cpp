// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Records board (round 4 #18): new records per category and band, a nearer one does not count, a tie within 0.1 km neither, nonsense distances,
// clearing one category, the saved text with tabs in the fields.
#include "dect2/common/records.h"
#include "../common/test_util.h"
using namespace dect2;

int main() {
    RecordsBoard b;
    CHECK(b.offer("ADS-B", "", 312.4, "4840D6", "KLM1023 FL360", 100) && !b.offer("ADS-B", "", 300, "X", "", 101) && !b.offer("ADS-B", "", 312.45, "Y", "", 102) &&
          b.offer("ADS-B", "", 330, "AE1234", "RCH401", 103), "ADS-B records");
    CHECK(b.offer("FT8", "20m", 2500, "VK2AB", "QF56 -18 dB", 104) && b.offer("FT8", "40m", 900, "DL1ABC", "", 105) && !b.offer("FT8", "20m", 2400, "Z", "", 106), "per band");
    CHECK(!b.offer("AIS", "", -1, "1", "", 1) && !b.offer("AIS", "", 0, "1", "", 1) && !b.offer("AIS", "", 30000, "1", "", 1) && !b.offer("", "", 5, "1", "", 1), "nonsense refused");
    const Record* r = b.get("ADS-B", "");
    CHECK(r && r->id == "AE1234" && r->km == 330 && r->utc == 103 && b.all().size() == 3 && b.dirty(), "kept the best");
    b.offer("APRS", "", 120, "LY1\tABC", "with\ttab", 107);
    RecordsBoard c;
    c.parse(b.serialize() + "bad line\n\t\t\t\t\t\n");
    CHECK(c.all().size() == 4 && c.get("APRS", "")->id == "LY1 ABC" && c.get("FT8", "20m")->detail == "QF56 -18 dB" && !c.dirty(), "saved and read back");
    c.clear("FT8");
    CHECK(c.all().size() == 2 && !c.get("FT8", "40m"), "clear one category");
    return testSummary("adsb_records");
}
