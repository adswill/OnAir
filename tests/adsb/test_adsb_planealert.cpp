// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// plane-alert-db import (round 4 #31): the real header, rows with quoted commas, lower-case hex, bad addresses, columns in another order,
// one list per category (off at first), matching an ADS-B event through the watchlist, the table index.
#include "dect2/watchlist/plane_alert.h"
#include "../common/test_util.h"
using namespace dect2::watchlist;
namespace au = dect2::automation;

int main() {
    const std::string csv =
        "$ICAO,$Registration,$Operator,$Type,$ICAO Type,#CMPG,$Tag 1,$#Tag 2,$#Tag 3,Category,$#Link\n"
        "000004,FAC1282,Colombian Aerospace Force,CASA C-295 M,C295,Mil,Cargo,Tactical Transport,Marching Powder,Other Air Forces,https://x\n"
        "ae1234,05-5145,United States Air Force,Boeing C-17A Globemaster III,C17,Mil,Cargo,\"Heavy, Strategic\",,USAF,\n"
        "43C5A1,ZZ336,Royal Air Force,Airbus A330 Voyager KC2,A332,Mil,Tanker,,,Other Air Forces,\n"
        "4CA1B2,EI-ABC,Police Aviation,EC135,EC35,Gov,Police,,,Police Forces,\n"
        "XYZ123,BAD,,,,,,,,Bad,\n"
        "12345,SHORT,,,,,,,,Bad,\n"
        "3C6444,D-HELI,,Bell 206,B06,Civ,Historic,,,\n";
    std::vector<PlaneAlertEntry> v;
    std::string err;
    CHECK(parsePlaneAlert(csv, v, err) && v.size() == 5, "5 aircraft, 2 bad addresses skipped (%zu) %s", v.size(), err.c_str());
    if (v.size() == 5) {
        CHECK(v[0].icao == 4 && v[0].registration == "FAC1282" && v[0].tagText() == "Cargo, Tactical Transport, Marching Powder" && v[0].category == "Other Air Forces" && v[0].cmpg == "Mil", "first row");
        CHECK(v[1].icao == 0xAE1234 && v[1].tags.size() == 2 && v[1].tags[1] == "Heavy, Strategic" && v[1].icaoType == "C17", "quoted comma, lower-case hex");
        CHECK(v[4].category.empty() && v[4].tagText() == "Historic", "no category");
    }
    const auto lists = planeAlertLists(v);
    CHECK(lists.size() == 4 && lists[0].name == "plane-alert: Other Air Forces" && lists[0].values.size() == 2 && !lists[0].enabled && isPlaneAlertList(lists[0]) && lists[3].name == "plane-alert: Uncategorised",
          "lists per category (%zu)", lists.size());
    au::Event e("adsb.aircraft"); e.str("hex", "43C5A1").str("callsign", "RRR123");
    std::string matched, ident;
    List on = lists[0]; on.enabled = true;
    CHECK(matchList(on, e, matched, ident) && matched == "43C5A1", "the watchlist matches the address: %s", matched.c_str());
    au::Event o("adsb.aircraft"); o.str("hex", "4840D6");
    CHECK(!matchList(on, o, matched, ident), "not another aircraft");
    const auto idx = planeAlertIndex(v);
    CHECK(idx.count(0xAE1234) && idx.at(0xAE1234).operatorName == "United States Air Force", "index");
    // columns in another order, a BOM
    std::vector<PlaneAlertEntry> w;
    CHECK(parsePlaneAlert("\xEF\xBB\xBF" "Category,$ICAO,$Operator\nPolice Forces,4ca1b2,Police\n", w, err) && w.size() == 1 && w[0].icao == 0x4CA1B2 && w[0].category == "Police Forces", "header order and BOM");
    CHECK(!parsePlaneAlert("a,b\n1,2\n", w, err) && !parsePlaneAlert("", w, err), "not a plane-alert file");
    return testSummary("adsb_planealert");
}
