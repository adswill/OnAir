// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/results/rx_json.h"
#include "dect2/automation/json_mini.h"
#include "dect2/railway/railway_gen.h"
#include "../common/test_util.h"
using namespace dect2;
namespace json = dect2::automation::json;
static json::Value details(const RxTelemetry& rx) {
    json::Value root; std::string error;
    CHECK(json::parse(rxTelemetryJson(rx), root, error), "valid telemetry JSON: %s", error.c_str());
    CHECK(root.num("standard") == rx.standard, "preserve active standard");
    const auto* d = root.get("details"); CHECK(d && d->type == json::Value::Obj, "mode details object");
    return d ? *d : json::Value::object();
}
static json::Value message(const json::Value& d, size_t index = 0) {
    const auto* rows = d.get("messages");
    CHECK(rows && rows->type == json::Value::Arr && rows->a.size() > index, "message array at %zu", index);
    return rows && rows->a.size() > index ? rows->a[index] : json::Value::object();
}
static bool null(const json::Value& v, const char* field) { const auto* x = v.get(field); return x && x->type == json::Value::Null; }
int main() {
    RxTelemetry rx;
    railway::Message klw, tdcs;
    CHECK(railway::parseFrame(railway::demoFrame(railway::Protocol::Klw), klw) && railway::parseFrame(railway::demoFrame(railway::Protocol::Tdcs), tdcs), "seed protocol messages");
    klw.wallTime = 1700000000; klw.correctedBits = 2;
    rx.klw.messages = {klw}; rx.tdcs.messages = {tdcs};
    rx.standard = railway::kKlwActiveStandard;
    auto d = details(rx), m = message(d);
    CHECK(d.str("mode") == "KLW" && m.str("action") == "Pressure response" && m.num("actionCode") == 0x92 && m.str("tailId") == "123456" && m.num("pressureKpa") == 486, "KLW JSON selects the KLW receiver");
    CHECK(m.num("correctedBits") == 2 && m.num("wallTime") == 1700000000 && null(m, "kilometer"), "metadata and absent mileage");
    rx.klw.messages[0].pressureKpa = -1;
    CHECK(null(message(details(rx)), "pressureKpa"), "unknown pressure is null");
    rx.klw.messages[0].pressureKpa = 0;
    CHECK(message(details(rx)).num("pressureKpa", -1) == 0, "zero pressure survives JSON");
    rx.standard = railway::kTdcsActiveStandard; d = details(rx); m = message(d);
    CHECK(d.str("mode") == "TDCS" && m.str("train") == "T112" && m.str("locomotive") == "HXD3D-0521" && m.num("speedKmh") == 124 && m.num("kilometer") == 1161.011, "TDCS JSON selects the TDCS receiver");
    CHECK(m.num("station") == 130 && m.num("track") == 4 && m.num("actualTrack") == 5 && m.num("carCount") == 18 && m.num("driver") == 123456, "TDCS consist and station fields");
    rx.tdcs.messages[0].kilometerValid = false; rx.tdcs.messages[0].testFrame = true;
    m = message(details(rx)); CHECK(null(m, "kilometer") && m.flag("testFrame"), "TDCS test mileage is absent");
    LbjMessage lbj; lbj.train = "K123"; lbj.loco = "HXD3C-0931"; lbj.route = "津山线\t\"A\"\\B\n";
    lbj.latitude = 39.125; lbj.longitude = 117.25; lbj.positionValid = true; lbj.paired = true;
    rx.standard = 82; rx.lbj.messages = {lbj}; d = details(rx); m = message(d);
    CHECK(d.str("mode") == "LBJ" && m.str("route") == lbj.route && m.flag("paired") && m.num("latitude") == 39.125 && m.num("longitude") == 117.25, "LBJ UTF-8/JSON escaping, pairing and position");
    rx.lbj.messages[0].positionValid = false; rx.lbj.messages[0].paired = false;
    rx.lbj.messages[0].train.clear(); rx.lbj.messages[0].timeSync = true; rx.lbj.messages[0].networkTime = "12:34";
    m = message(details(rx)); CHECK(null(m, "latitude") && null(m, "longitude") && !m.flag("paired") && m.flag("timeSync") && m.str("train").empty() && m.str("networkTime") == "12:34", "LBJ time sync has no invented train or position");
    rx.klw.messages.clear(); rx.tdcs.messages.clear(); rx.lbj.messages.clear();
    for (int active : {82, railway::kKlwActiveStandard, railway::kTdcsActiveStandard}) {
        rx.standard = active; d = details(rx); const auto* rows = d.get("messages");
        CHECK(rows && rows->type == json::Value::Arr && rows->a.empty(), "empty history remains valid JSON for mode %d", active);
    }
    return testSummary("test_railway_telemetry");
}
