// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Horus through the whole dect2core: the registry, the engine's idle snapshot on the Horus active standard, the telemetry JSON
// and the results slot (save, clear, load). No radio, window or results saver is started.
#include "dect2/engine/engine.h"
#include "dect2/horus/horus_gen.h"
#include "dect2/results/rx_json.h"
#include "dect2/automation/json_mini.h"
#include "../common/test_util.h"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <memory>
using namespace dect2;
namespace json = dect2::automation::json;

static std::string savedPackets() {
    horus::Tables t;
    horus::parseIdList(horus::defaultIdList(), t.ids);
    std::string s = "horus1\n";
    const char* h = "0123456789abcdef";
    for (int k = 0; k < 3; k++) {
        s += std::to_string(1700000000 + 4 * k) + "\t2\t";
        for (auto b : horus::buildBinary(horus::simPacket(2, k, 4), 2)) { s += h[b >> 4]; s += h[b & 15]; }
        s += '\n';
    }
    s += "1700000020\t0\t" + horus::buildSentence(horus::simPacket(0, 0, 4)) + "\n";
    return s;
}

static void snapshot(Engine& e, size_t payloads) {
    e.setStandard(horus::kStandard);
    RxTelemetry rx;
    CHECK(e.latestRx(rx, 0) && rx.standard == horus::kActiveStandard, "idle snapshot on the Horus active standard (got %d)", rx.standard);
    json::Value root; std::string error;
    CHECK(json::parse(rxTelemetryJson(rx), root, error), "telemetry JSON: %s", error.c_str());
    const auto* d = root.get("details");
    CHECK(d && d->str("mode") == "Horus", "details.mode");
    const auto* rows = d ? d->get("payloads") : nullptr;
    CHECK(rows && rows->type == json::Value::Arr && rows->a.size() == payloads, "%zu payload rows (want %zu)", rows ? rows->a.size() : 0, payloads);
    CHECK(rx.horus.payloads.size() == payloads, "telemetry member");
}

int main() {
    const auto tu = horusTuning();
    const auto* byId = modeTuningById("horus");
    CHECK(byId && byId == modeTuning(tu.stdMode) && byId->defMhz == tu.defMhz, "registry resolves horus by id and standard");
    CHECK(makeModeSynth(tu.stdMode, SynthConfig(), tu.sampleRate) != nullptr, "test signal registered");
    auto engine = std::make_unique<Engine>();
    CHECK(engine->horus().loadResults(savedPackets()), "seed Horus results");
    const auto names = engine->resultNames();
    CHECK(std::count(names.begin(), names.end(), "horus") == 1, "one results slot");
    snapshot(*engine, 2);   // 4FSKTEST-V2 and the RTTY payload
    engine->clearResults(horus::kStandard);
    snapshot(*engine, 0);
    CHECK(engine->horus().loadResults(savedPackets()), "restore");
    const auto dir = std::filesystem::temp_directory_path() / ("onair-horus-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::error_code ec;
    if (std::filesystem::create_directory(dir, ec)) {
        CHECK(engine->saveResults(dir.string()) >= 1 && std::filesystem::exists(dir / "horus.bin"), "results file");
        engine->clearAllResults();
        CHECK(engine->loadResults(dir.string()) >= 1, "reload");
        snapshot(*engine, 2);
        std::filesystem::remove_all(dir, ec);
    } else CHECK(false, "temporary directory: %s", ec.message().c_str());
    return testSummary("test_horus_engine");
}
