// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Full dect2core integration: registry -> idle telemetry -> persistent result slots.
// No radio, network source, window or background results saver is started.
#include "dect2/engine/engine.h"
#include "dect2/railway/railway_gen.h"
#include "dect2/results/rx_json.h"
#include "dect2/automation/json_mini.h"
#include "../common/test_util.h"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <memory>
using namespace dect2;
namespace json = dect2::automation::json;
static std::string savedFrame(railway::Protocol p) {
    std::string s = "cir1\n1700000000\t0\t"; const char* h = "0123456789abcdef";
    for (auto b : railway::demoFrame(p)) { s += h[b >> 4]; s += h[b & 15]; }
    return s + '\n';
}
static RxTelemetry snapshot(Engine& engine, int standard, int expectedActive, const char* mode, size_t count) {
    engine.setStandard(standard);
    RxTelemetry rx; CHECK(engine.latestRx(rx, 0) && rx.standard == expectedActive, "%s idle snapshot routes to active standard %d", mode, expectedActive);
    json::Value root; std::string error;
    CHECK(json::parse(rxTelemetryJson(rx), root, error), "engine telemetry JSON: %s", error.c_str());
    const auto* d = root.get("details");
    CHECK(d && d->str("mode") == mode, "engine mode selects %s JSON", mode);
    const auto* rows = d ? d->get("messages") : nullptr;
    CHECK(rows && rows->type == json::Value::Arr && rows->a.size() == count, "%s result history has %zu rows", mode, count);
    return rx;
}
int main() {
    for (const auto& tuning : {klwTuning(), tdcsTuning(), lbjTuning()}) {
        const auto* byId = modeTuningById(tuning.id);
        const auto* byStandard = modeTuning(tuning.stdMode);
        CHECK(byId && byStandard && byId == byStandard && byId->stdMode == tuning.stdMode && byId->defMhz == tuning.defMhz, "registry resolves %s by both ID and engine standard", tuning.id);
    }
    auto engine = std::make_unique<Engine>();
    CHECK(engine->klw().loadResults(savedFrame(railway::Protocol::Klw)), "seed KLW");
    CHECK(engine->tdcs().loadResults(savedFrame(railway::Protocol::Tdcs)), "seed TDCS");
    CHECK(engine->lbj().loadResults("lbj2\nK123\t12.3\tHXD3C-0931\t23\tdown\t1700000000\n"), "seed LBJ");
    const auto names = engine->resultNames();
    for (const char* name : {"lbj", "klw", "tdcs"}) CHECK(std::count(names.begin(), names.end(), name) == 1, "one result slot for %s", name);
    auto klw = snapshot(*engine, railway::kKlwStandard, railway::kKlwActiveStandard, "KLW", 1);
    CHECK(klw.klw.messages.size() == 1 && klw.klw.messages[0].tailId == "123456", "KLW telemetry member");
    auto tdcs = snapshot(*engine, railway::kTdcsStandard, railway::kTdcsActiveStandard, "TDCS", 1);
    CHECK(tdcs.tdcs.messages.size() == 1 && tdcs.tdcs.messages[0].train == "T112", "TDCS telemetry member");
    snapshot(*engine, lbjTuning().stdMode, 82, "LBJ", 1);
    engine->clearResults(railway::kKlwStandard);
    snapshot(*engine, railway::kKlwStandard, railway::kKlwActiveStandard, "KLW", 0);
    snapshot(*engine, railway::kTdcsStandard, railway::kTdcsActiveStandard, "TDCS", 1);
    snapshot(*engine, lbjTuning().stdMode, 82, "LBJ", 1);
    CHECK(engine->klw().loadResults(savedFrame(railway::Protocol::Klw)), "restore KLW before disk round trip");

    const auto dir = std::filesystem::temp_directory_path() / ("onair-railway-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::error_code ec;
    const bool owned = std::filesystem::create_directory(dir, ec);
    CHECK(owned, "create owned temporary results directory: %s", ec.message().c_str());
    if (owned) {
        CHECK(engine->saveResults(dir.string()) >= 3, "save registered result slots");
        for (const char* name : {"lbj.bin", "klw.bin", "tdcs.bin"}) CHECK(std::filesystem::exists(dir / name), "results file %s", name);
        engine->clearAllResults();
        CHECK(engine->loadResults(dir.string()) >= 3, "reload registered result slots");
        snapshot(*engine, railway::kKlwStandard, railway::kKlwActiveStandard, "KLW", 1);
        snapshot(*engine, railway::kTdcsStandard, railway::kTdcsActiveStandard, "TDCS", 1);
        snapshot(*engine, lbjTuning().stdMode, 82, "LBJ", 1);
        std::filesystem::remove_all(dir, ec);
        CHECK(!ec, "remove owned test results");
    }
    return testSummary("test_railway_engine");
}
