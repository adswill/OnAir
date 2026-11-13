// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Custom bandwidth and sample rate in EVERY registered mode (3.6; docs/BANDWIDTH-AUDIT.md). The owner's rule: the default of a mode stays
// its default, and the user can set any bandwidth and any rate in every mode, with nothing clamped (a limit only warns).
//
// The universe is read from the sources, so a mode added later is covered without touching this file: every row of the mode table
// (app/ui/mode_table.cpp, built-in families and protocol entries), every engine standard of the tuning registry (modeTunings()), every protocol
// entry (subModes()) and a plugin mode. For each of them it proves:
//   1. the default is untouched when nothing is customised (the same rate, filter and bandwidth as the mode's own);
//   2. a custom bandwidth reaches the receiver's configuration through the shared path (rxApplyWidth, the call applyBandwidth() makes): the
//      rate, the radio filter and the bandwidth change with it; a custom rate does too;
//   3. nothing is clamped: 1 kHz, 6 MHz and 100 MHz (and a rate of 1 Hz to 1 Gsps) all give exactly what was asked for, and a value a radio
//      cannot do only produces a warning, never a refusal;
//   4. the choice persists per family and per protocol entry (save, load, another mode unaffected, tick off gives the default back);
//   5. the engine starts on the test signal at the planned rate (default, wide, typed rate) and is processing signal;
//   6. the control is in every layout (the source places the Advanced row and the Simple view's chip) and the chip is wired.
// `--csv` prints the table docs/BANDWIDTH-AUDIT.md is made from.
#include "../../app/state/rx_width.h"
#include "dect2/engine/engine.h"
#include "dect2/engine/engine_testkit.h"
#include "dect2/engine/mode_registry.h"
#include "dect2/engine/mode_tuning.h"
#include "dect2/engine/rate_choice.h"
#include "dect2/t2/t2rx.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
using namespace dect2;

static int fails = 0;
#define CHECK(c, fmt, ...) do { if (!(c)) { std::printf("FAIL line %d: " fmt "\n", __LINE__ __VA_OPT__(,) __VA_ARGS__); fails++; } } while (0)   // Flawfinder: ignore (the format is a literal)

struct MapPrefs {   // the settings store of the app, in memory
    std::map<std::string, double> d;
    double getD(const char* k, double def) { auto i = d.find(k); return i == d.end() ? def : i->second; }
    long getI(const char* k, long def) { auto i = d.find(k); return i == d.end() ? def : (long)i->second; }
    void setD(const char* k, double v) { d[k] = v; }
    void setI(const char* k, long v) { d[k] = (double)v; }
};

static std::string readFile(const std::string& rel) {
    std::ifstream in(std::string(DECT2_SOURCE_DIR) + "/" + rel);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}


// named constants used as family numbers ("inline constexpr int kName = 146;"), as tests/app/test_mode_numbers.cpp reads them
static std::map<std::string, std::string> gConstText;
static void scanConstants(const std::string& text) {
    static const std::regex re(R"(constexpr\s+int\s+(k[A-Za-z0-9_]+)\s*=\s*([^;]+);)");
    for (auto it = std::sregex_iterator(text.begin(), text.end(), re); it != std::sregex_iterator(); ++it) gConstText[(*it)[1].str()] = (*it)[2].str();
}
static bool evalExpr(std::string e, int& out, int depth = 0) {
    if (depth > 8) return false;
    while (!e.empty() && (e.back() == ' ' || e.back() == '\t')) e.pop_back();
    while (!e.empty() && (e.front() == ' ' || e.front() == '\t')) e.erase(e.begin());
    const size_t colons = e.rfind("::");
    if (colons != std::string::npos) e = e.substr(colons + 2);
    const size_t op = e.find_last_of("+-");
    if (op != std::string::npos && op > 0) {
        int a = 0, b = 0;
        if (!evalExpr(e.substr(0, op), a, depth + 1) || !evalExpr(e.substr(op + 1), b, depth + 1)) return false;
        out = e[op] == '+' ? a + b : a - b;
        return true;
    }
    if (!e.empty() && std::isdigit((unsigned char)e[0])) { out = std::atoi(e.c_str()); return true; }
    auto it = gConstText.find(e);
    return it != gConstText.end() && evalExpr(it->second, out, depth + 1);
}

// The engine standard of a family, as engineStd() in app_state.cpp: families 6 and up are the family + 2; the six classic ones are fixed
static int stdOfFamily(int f) { return f >= 6 ? f + 2 : f == 1 ? 3 : f == 2 ? 4 : f == 3 ? 5 : f == 4 ? 6 : f == 5 ? 7 : 0; }

// Modes of the table that have no radio capture of their own (so no tuning row, nothing to widen); each with the reason
static const std::map<int, const char*> kNoCapture = {
    {209, "IPTV: streams over the internet, no radio"},
    {19999, "Mode Builder: a designer; its modes register their own tuning as plugin modes"},
    {999, "Plugins: a manager; each installed plugin mode registers its own tuning"},
};

struct Row { int family; std::string name, sub; };

static std::vector<Row> readModeTable() {
    const std::string table = readFile("app/ui/mode_table.cpp");
    auto subIdOf = [&](size_t brace) -> std::string {
        int depth = 0, field = 0; bool str = false; std::string cur;
        for (size_t p = brace + 1; p < table.size(); p++) {
            const char ch = table[p];
            if (str) { if (ch == '\\') { cur += ch; cur += table[++p]; continue; } if (ch == '"') str = false; cur += ch; continue; }
            if (ch == '"') { str = true; cur += ch; continue; }
            if (ch == '(' || ch == '{') depth++;
            if ((ch == ')' || ch == '}') && depth > 0) { depth--; cur += ch; continue; }
            if (ch == '}' || (ch == ',' && depth == 0)) {
                if (field == 8) { const size_t a = cur.find('"'), b = cur.rfind('"'); return a != std::string::npos && b > a ? cur.substr(a + 1, b - a - 1) : std::string(); }
                if (ch == '}') return std::string();
                field++; cur.clear(); continue;
            }
            cur += ch;
        }
        return std::string();
    };
    {   // the headers that hold the named family numbers (core/include/dect2/<mode>/<x>_modes.h) and the app's own
        std::error_code ec;
        for (auto it = std::filesystem::recursive_directory_iterator(std::string(DECT2_SOURCE_DIR) + "/core/include/dect2", ec); !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
            const std::string fn = it->path().filename().string();
            if (fn.size() > 8 && fn.compare(fn.size() - 8, 8, "_modes.h") == 0) { std::ifstream in(it->path()); std::stringstream ss; ss << in.rdbuf(); scanConstants(ss.str()); }
        }
        scanConstants(readFile("app/app.h"));
    }
    std::vector<Row> rows;
    static const std::regex row(R"(\n\s*\{\s*([A-Za-z0-9_:]+(?:\s*[+-]\s*[0-9]+)?)\s*,\s*\"([^\"]*)\")");
    for (auto it = std::sregex_iterator(table.begin(), table.end(), row); it != std::sregex_iterator(); ++it) {
        int f = 0;
        const std::string expr = (*it)[1].str(), name = (*it)[2].str();
        if (!evalExpr(expr, f)) { CHECK(false, "mode row \"%s\": cannot read the family '%s'", name.c_str(), expr.c_str()); continue; }
        rows.push_back({f, name, subIdOf(table.find('{', (size_t)it->position()))});
    }
    return rows;
}

// the ModeTuning of a tuning plan for a mode: what applyBandwidth() starts from
static TuneSettings ownTune(const ModeTuning& mt) {
    TuneSettings t;
    t.centerHz = mt.defMhz * 1e6;
    t.bandwidthMhz = mt.bandwidthMhz; t.sampleRate = mt.sampleRate; t.basebandFilterHz = mt.basebandHz;
    t.synth.mode = mt.stdMode;
    return t;
}

static bool nearEq(double a, double b) { return std::fabs(a - b) <= 1e-9 * std::max(1.0, std::fabs(b)); }

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IOLBF, 0);   // line by line: a run stopped at its time limit still shows every FAIL and where it was
    const bool csv = argc > 1 && !std::strcmp(argv[1], "--csv");
    const bool rowsOut = argc > 1 && !std::strcmp(argv[1], "--rows");   // one line per mode-table row: family, engine standard, tuning id
    const bool quick = argc > 1 && !std::strcmp(argv[1], "--quick");   // the plan checks only, no engine starts
    // ---- the universe
    std::vector<Row> rows = readModeTable();
    CHECK(rows.size() > 120, "only %zu rows read from the mode table", rows.size());
    std::vector<const ModeTuning*> tunings;
    for (const auto& m : modeTunings()) tunings.push_back(&m);
    // the six classic families have no registry row (applyBandwidth() holds their values): DVB-T2 / DVB-T, ATSC, DAB, ATSC 3.0, ISDB-T, FM
    static std::vector<ModeTuning> classic;
    {
        struct C { int std; const char* id; const char* name; double bw, rate, bbf; };
        const C cs[] = {{0, "dvb", "DVB-T2 / DVB-T", 8, 10e6, 0}, {3, "atsc", "ATSC", 6, 8e6, 0}, {4, "dab", "DAB / DAB+", 1.7, 2.048e6, 1.75e6}, {5, "atsc3", "ATSC 3.0", 6, 8e6, 0},
                        {6, "isdbt", "ISDB-T", 8, 10e6, 0}, {7, "fm", "FM", 0.25, 4e6, 2.5e6}};
        for (const C& c : cs) {
            ModeTuning t; t.stdMode = c.std; t.id = c.id; t.name = c.name; t.bandwidthMhz = c.bw; t.sampleRate = c.rate; t.basebandHz = c.bbf; t.defMhz = 100;
            t.minSampleRate = minSampleRateFor(c.std, c.bw);
            classic.push_back(t);
        }
        for (const ModeTuning& t : classic) tunings.push_back(&t);
    }
    registerPluginTuning(kPluginStdBase + 5, "bwtest-plugin", "BW test plugin", 1, 6000, 100, 2e6, 0.2);   // a plugin mode: one more row of the registry
    const ModeTuning* plug = modeTuning(kPluginStdBase + 5);
    CHECK(plug != nullptr, "the plugin mode did not register");
    if (plug) tunings.push_back(plug);
    CHECK(tunings.size() > 120, "only %zu engine standards", tunings.size());
    // every mode-table row maps to a registered tuning (or is one of the listed non-capture rows)
    std::set<int> rowStd;
    int rowsWithTuning = 0;
    for (const Row& r : rows) {
        const int s = stdOfFamily(r.family);
        if (r.family >= 6 && !modeTuning(s)) {
            CHECK(kNoCapture.count(r.family), "mode row \"%s\" (family %d, engine standard %d) has no tuning, so no custom bandwidth: add its tuning or list the reason in kNoCapture", r.name.c_str(), r.family, s);
            continue;
        }
        rowStd.insert(s); rowsWithTuning++;
    }
    // the protocol entries: each has its own saved key (the app's rateModeId() returns the entry's id), so each saves and loads on its own
    {
        std::set<std::string> seen;
        int nsub = 0;
        const SubMode* sms = subModes(nsub);
        CHECK(nsub > 20, "only %d protocol entries", nsub);
        for (int i = 0; i < nsub; i++) {
            const SubMode& sm = sms[i];
            CHECK(sm.id && *sm.id, "a protocol entry without an id");
            CHECK(seen.insert(sm.id).second, "protocol entry id %s is used twice", sm.id);
            CHECK(modeTuning(sm.stdMode) != nullptr, "protocol entry %s: engine standard %d has no tuning", sm.id, sm.stdMode);
        }
        for (const Row& r : rows) if (!r.sub.empty() && r.sub.rfind("auto:", 0) != 0) CHECK(subModeById(r.sub) != nullptr, "mode row \"%s\": protocol entry %s is not registered (it would share its family's saved bandwidth; the auto: rows detect, then switch to a real mode)", r.name.c_str(), r.sub.c_str());
    }

    if (rowsOut) {
        for (const Row& r : rows) {
            const ModeTuning* t = r.family >= 6 || r.family == 0 ? modeTuning(stdOfFamily(r.family)) : nullptr;
            std::printf("%d\t%s\t%s\t%d\t%s\n", r.family, r.name.c_str(), r.sub.c_str(), stdOfFamily(r.family), t ? t->id : "-");
        }
        return 0;
    }
    if (csv) std::printf("std,id,name,minMhz,maxMhz,rate_msps,basebandHz,bw_mhz,minrate_msps,wide6_rate,wide6_bbf,tiny_rate,huge_rate\n");
    DeviceInfo syn;   // the test signal
    int engines = 0, planned = 0;
    for (const ModeTuning* mt : tunings) {
        if (!mt->id || !*mt->id) { CHECK(false, "engine standard %d has no id", mt->stdMode); continue; }
        // 1. nothing saved: the standard width and rate, untouched
        MapPrefs p;
        CHECK(rxWidthLoad(p, mt->id) == 0, "%s: nothing saved must mean the mode's own bandwidth", mt->id);
        TuneSettings def = ownTune(*mt);
        const TuneSettings ref = def;
        rxApplyWidth(def, rxWidthLoad(p, mt->id), mt->minSampleRate);
        CHECK(def.sampleRate == ref.sampleRate && def.bandwidthMhz == ref.bandwidthMhz && def.basebandFilterHz == ref.basebandFilterHz && !def.rateChosen, "%s: the default changed", mt->id);
        // 2. persistence: per mode, tick off gives the default back, another mode is unaffected
        rxWidthSave(p, mt->id, true, 6.0);
        rxWidthSave(p, "other_mode", true, 1.0);
        CHECK(rxWidthLoad(p, mt->id) == 6.0 && rxWidthLoad(p, "other_mode") == 1.0, "%s: width not saved per mode", mt->id);
        rxWidthSave(p, mt->id, false, 6.0);
        CHECK(rxWidthLoad(p, mt->id) == 0, "%s: tick off must give the default back", mt->id);
        rxWidthSave(p, mt->id, true, 2.5e-3);   // 2.5 kHz
        CHECK(rxWidthLoad(p, mt->id) == 2.5e-3, "%s: a tiny width was not kept", mt->id);
        // 3. the shared receive path: a custom width reaches the configuration, whatever its size
        struct W { double mhz; const char* what; };
        const W widths[] = {{1e-3, "1 kHz"}, {0.01, "10 kHz"}, {6.0, "6 MHz"}, {30.0, "30 MHz"}, {100.0, "100 MHz"}, {500.0, "500 MHz"}};
        double rate6 = 0, bbf6 = 0, rateTiny = 0, rateHuge = 0;
        for (const W& w : widths) {
            TuneSettings t = ownTune(*mt);
            rxApplyWidth(t, w.mhz, mt->minSampleRate);
            CHECK(t.bandwidthMhz == w.mhz, "%s, %s: the bandwidth became %g MHz (clamped)", mt->id, w.what, t.bandwidthMhz);
            CHECK(t.sampleRate >= w.mhz / 0.9 * 1e6 - 1 && t.sampleRate >= mt->minSampleRate && t.rateChosen, "%s, %s: the rate %.4f Msps does not carry the width", mt->id, w.what, t.sampleRate / 1e6);
            CHECK(nearEq(t.basebandFilterHz, 0.9 * t.sampleRate), "%s, %s: the radio filter %.0f Hz did not follow", mt->id, w.what, t.basebandFilterHz);
            CHECK(std::isfinite(t.sampleRate) && t.sampleRate > 0, "%s, %s: no usable rate", mt->id, w.what);
            CHECK(t.channelBwMhz == mt->bandwidthMhz, "%s, %s: the signal's own channel width became %g MHz (a custom capture bandwidth must not change it)", mt->id, w.what, t.channelBwMhz);
            if (w.mhz == 6.0) { rate6 = t.sampleRate; bbf6 = t.basebandFilterHz; }
            if (w.mhz == 1e-3) rateTiny = t.sampleRate;
            if (w.mhz == 500.0) rateHuge = t.sampleRate;
        }
        // a width of zero, negative or not a number is "not a width": the mode's own values stay
        for (double bad : {0.0, -3.0, (double)std::nan(""), (double)INFINITY}) {
            TuneSettings t = ownTune(*mt);
            rxApplyWidth(t, bad, mt->minSampleRate);
            CHECK(t.sampleRate == ref.sampleRate && t.bandwidthMhz == ref.bandwidthMhz, "%s: %g changed the tune", mt->id, bad);
        }
        // a custom rate: any positive value goes through, and only warns (never refused) when the radio does not list it
        {
            DeviceInfo hackrf; hackrf.kind = DeviceInfo::HackRF;
            const RateLimits L = rateLimitsOf(hackrf);
            for (double hz : {1.0, 1e3, 48e3, 3.5e6, 20e6, 61.44e6, 1e9}) {
                TuneSettings t = ownTune(*mt);
                const RateCheck c = rxApplyRate(t, hz, L, mt->minSampleRate);
                CHECK(c.ok && t.sampleRate == hz && t.rateChosen, "%s: the rate %g Hz was refused or changed to %g", mt->id, hz, t.sampleRate);
                const bool inList = hz >= 2e6 && hz <= 20e6;
                if (!inList) CHECK(!c.why.empty(), "%s: %g Hz is outside the HackRF's list and must warn", mt->id, hz);
            }
            TuneSettings t = ownTune(*mt);
            const RateCheck c0 = rxApplyRate(t, 0, L, mt->minSampleRate);   // 0 = the mode's own rate: untouched
            CHECK(!c0.ok && t.sampleRate == ref.sampleRate && !t.rateChosen, "%s: rate 0 must leave the mode's own rate", mt->id);
            // the warning for a width, with what a radio lists: says so, does not refuse
            const std::string w = rxWidthWarning(300e6, L, mt->minSampleRate);
            CHECK(!w.empty(), "%s: 300 Msps on a HackRF must warn", mt->id);
        }
        planned++;
        if (csv) {
            std::string nm = mt->name; for (char& ch : nm) if (ch == ',') ch = ';';
            std::printf("%d,%s,%s,%g,%g,%g,%g,%g,%g,%g,%g,%g,%g\n", mt->stdMode, mt->id, nm.c_str(), mt->minMhz, mt->maxMhz, mt->sampleRate / 1e6, mt->basebandHz, mt->bandwidthMhz,
                        mt->minSampleRate / 1e6, rate6 / 1e6, bbf6, rateTiny / 1e6, rateHuge / 1e6);
            continue;
        }
        // 5. it reaches the engine: the receiver runs at the rate the plan gave, and is processing signal
        if (quick) continue;
        if (const char* only = std::getenv("ONAIR_TEST_MODES")) { const std::string l = std::string(",") + only + ","; if (l.find(std::string(",") + mt->id + ",") == std::string::npos) continue; }   // debugging: the engine starts of these modes only
        TuneSettings wide = ownTune(*mt), typed = ownTune(*mt);
        rxApplyWidth(wide, 6.0, mt->minSampleRate);
        { DeviceInfo hackrf; hackrf.kind = DeviceInfo::HackRF; rxApplyRate(typed, 3.5e6, rateLimitsOf(hackrf), mt->minSampleRate); }
        struct Case { const char* what; TuneSettings t; };
        const Case cases[] = {{"default", ownTune(*mt)}, {"6 MHz wide", wide}, {"typed 3.5 Msps", typed}};
        std::printf("engine: %s (standard %d)\n", mt->id, mt->stdMode);   // where a run stopped at its time limit was
        for (const auto& c : cases) {
            Engine e;
            e.setStandard(mt->stdMode);
            FileOptions fo;
            const bool ok = e.start(syn, c.t, fo);
            CHECK(ok, "%s, %s: the engine did not start (%s)", mt->id, c.what, e.startError().c_str());
            if (!ok) continue;
            engines++;
            const auto tStart = std::chrono::steady_clock::now();
            const bool ran = enginetest::waitFor(20.0, [&] { return enginetest::signalSecs(e) > 0.0; });
            {   // a slow start is printed, so a run near its time limit says which mode costs the time
                const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - tStart).count();
                if (secs > 3.0) std::printf("slow start: %s, %s: %.1f s to the first signal\n", mt->id, c.what, secs);
                std::fflush(stdout);
            }
            // (the test signal of DVB-T2 / DVB-T / ATSC is made at its native rate whatever the radio is asked: those four only need to run)
            const bool fixedSynth = mt->stdMode <= 3;
            CHECK(fixedSynth || std::fabs(e.sampleRate() - c.t.sampleRate) < 1.0 || std::fabs(e.sampleRate() - c.t.sampleRate) < c.t.sampleRate * 1e-3,
                  "%s, %s: asked for %.3f Msps, the engine runs at %.3f", mt->id, c.what, c.t.sampleRate / 1e6, e.sampleRate() / 1e6);
            CHECK(ran, "%s, %s: the engine is not processing signal", mt->id, c.what);
            CHECK(e.running(), "%s, %s: the engine stopped by itself", mt->id, c.what);
            e.stop();
        }
    }
    if (csv) return 0;

    // 5b. a custom capture bandwidth must not break decoding: DVB-T2 on the test signal (an 8 MHz channel) still locks (P1, L1-pre and L1-post decoded)
    // when the radio is asked for 12 MHz, 20 MHz or 40 MHz around it. (Before 3.6 the custom width replaced the channel width the receiver decodes with.)
    if (!quick) {
        for (double wMhz : {0.0, 12.0, 20.0, 40.0}) {
            TuneSettings t = ownTune(classic[0]);   // DVB (auto T2 / T): 8 MHz, 10 Msps
            t.synth.mode = 0; t.synth.snrDb = 30; t.synth.pace = enginetest::kFastPace;
            rxApplyWidth(t, wMhz, minSampleRateFor(1, 8));
            Engine e;
            e.setStandard(1);
            FileOptions fo;
            CHECK(e.start(syn, t, fo), "DVB-T2 at %g MHz: did not start (%s)", wMhz, e.startError().c_str());
            enginetest::waitFor(60.0, [&] { return enginetest::signalSecs(e) > 5.0; });
            RxTelemetry r; e.latestRx(r, 0);
            CHECK(r.p1.valid && r.l1preOk && r.l1postOk, "DVB-T2 with a %g MHz capture: no lock (P1 %d, L1-pre %d, L1-post %d)", wMhz, r.p1.valid, r.l1preOk, r.l1postOk);
            e.stop();
        }
    }

    // 6. the control is in every layout: the Advanced row where the source places it (the classic bar, the side panels, the setup rows) and the
    // Simple view's chip in the Command view; the chip and the row share one implementation of the change
    const char* layouts[] = {"app/ui/toolbar.cpp", "app/ui/ui2.cpp", "app/ui/ui2_faces.cpp", "app/ui/ui2_shell.cpp"};
    for (const char* f : layouts) CHECK(readFile(f).find("captureRowShown") != std::string::npos, "%s does not place the Advanced bandwidth row", f);
    CHECK(readFile("app/ui/toolbar.cpp").find("rxWidthControl(a, retune)") != std::string::npos, "the Advanced row is not drawn by the toolbar");
    CHECK(readFile("app/ui/kit/mode_panel.cpp").find("rxWidthChip(a)") != std::string::npos, "the Simple view has no bandwidth chip");
    CHECK(readFile("app/state/rx_width.cpp").find("rxbwreset") != std::string::npos && readFile("app/state/rx_width.cpp").find("rxbwchipreset") != std::string::npos, "no Reset to default button");
    // every mode goes through the one applyBandwidth(): the shared call is the one it makes
    CHECK(readFile("app/state/app_state.cpp").find("rxApplyWidth(a.tune") != std::string::npos, "applyBandwidth() does not use the shared receive path");
    CHECK(readFile("app/state/app_state.cpp").find("rxApplyRate(a.tune") != std::string::npos, "applyBandwidth() does not use the shared rate path");
    CHECK(readFile("server/server_host.cpp").find("rxApplyWidth(t") != std::string::npos, "the headless server does not apply the custom bandwidth");
    CHECK(readFile("core/src/net/remote_api.cpp").find("/api/v1/capture_bandwidth") != std::string::npos, "the remote API has no capture_bandwidth");
    CHECK(readFile("core/src/mcp/mcp_tools.cpp").find("capture_bandwidth_khz") != std::string::npos, "the MCP set_radio tool has no capture_bandwidth_khz");

    if (fails) std::printf("custom bandwidth, all modes: %d FAILED\n", fails);
    else std::printf("custom bandwidth, all modes: all passed (%zu mode-table rows, %d with a tuning, %d engine standards incl. plugin, %d engine starts)\n", rows.size(), rowsWithTuning, planned, engines);
    return fails ? 1 : 0;
}
