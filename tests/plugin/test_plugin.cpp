// Plugin system: manifest validation (malformed and hostile input), ABI refusal, a fake plugin loaded for real, the watchdog killing a looping
// plugin (in-process: abandoned; helper process: killed), bounded result marshalling, permission checks, safe mode after a crash, stable mode
// numbers, installing from a .onairplugin zip (and refusing hostile ones), and the three example plugins.
// usage: test_plugin <onair-plugin-host> <examples dir> <fake plugin lib> <fake plugin lib with a wrong ABI>
#include "dect2/plugin/host.h"
#include "onair_plugin.h"
#include "dect2/plugin/zip.h"
#include "dect2/automation/engine.h"
#include "../common/test_util.h"
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <thread>

using namespace dect2::plugin;
namespace fs = std::filesystem;
namespace automation = dect2::automation;
using Clock = std::chrono::steady_clock;

// the sizes the Rust bindings (sdk/rust/onair-plugin) assert on a 64-bit target
#if defined(__LP64__) || defined(_WIN64)
static_assert(sizeof(OnAirIqMeta) == 40 && sizeof(OnAirCell) == 32 && sizeof(OnAirField) == 32 && sizeof(OnAirEvent) == 56 &&
              sizeof(OnAirPluginVTable) == 80 && sizeof(OnAirHostApi) == 152, "the C ABI layout changed: bump ONAIR_PLUGIN_ABI_VERSION and the Rust bindings");
#endif

namespace {
std::string gHost, gExamples, gFake, gFakeBad;

bool waitFor(const std::function<bool()>& f, double sec) {
    const auto t0 = Clock::now();
    while (std::chrono::duration<double>(Clock::now() - t0).count() < sec) { if (f()) return true; std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
    return f();
}
double secSince(Clock::time_point t) { return std::chrono::duration<double>(Clock::now() - t).count(); }

std::string base(const std::string& p) { return fs::path(p).filename().string(); }

std::string manifestFor(const std::string& lib, const std::string& extra = "", const std::string& perms = "[]", const std::string& id = "fake", const std::string& kind = "mode") {
    return "{\"id\":\"" + id + "\",\"name\":\"Fake\",\"version\":\"1.2.3\",\"author\":\"t\",\"license\":\"MIT\",\"kind\":\"" + kind + "\",\"abi_version\":1,"
           "\"library\":\"" + lib + "\",\"permissions\":" + perms + ","
           "\"modes\":[{\"id\":\"ok\",\"name\":\"Ok\"},{\"id\":\"loop\",\"name\":\"Loop\"},{\"id\":\"refuse\",\"name\":\"Refuse\"},{\"id\":\"flood\",\"name\":\"Flood\"},{\"id\":\"perm\",\"name\":\"Perm\"},"
           "{\"id\":\"gain\",\"name\":\"Gain\"},{\"id\":\"event\",\"name\":\"Event\"},{\"id\":\"crash\",\"name\":\"Crash\"},{\"id\":\"hang-create\",\"name\":\"HC\"}],"
           "\"settings\":[{\"key\":\"gain\",\"label\":\"Gain\",\"type\":\"slider\",\"min\":0,\"max\":10,\"default\":2}]" + extra + "}";
}
void writeFile(const fs::path& p, const std::string& s) { fs::create_directories(p.parent_path()); std::ofstream(p, std::ios::binary) << s; }
std::string readAll(const std::string& p) { std::ifstream f(p, std::ios::binary); return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>()); }

void installFake(const fs::path& pluginsDir, const std::string& perms = "[]", const std::string& id = "fake") {
    const fs::path d = pluginsDir / id;
    fs::create_directories(d);
    fs::copy_file(gFake, d / base(gFake), fs::copy_options::overwrite_existing);
    // the id in the library is "fake": other ids are only for numbering tests, which never start them
    writeFile(d / "manifest.json", manifestFor(base(gFake), "", perms, id));
}

struct Env {
    fs::path dir;
    Env(const char* n) { dir = fs::temp_directory_path() / (std::string("onair-plugin-test-") + n); fs::remove_all(dir); fs::create_directories(dir); }
    ~Env() { std::error_code ec; fs::remove_all(dir, ec); }
};

std::vector<float> tone(size_t n, double fs, double hz, double amp) {
    std::vector<float> v(2 * n);
    for (size_t i = 0; i < n; i++) { v[2 * i] = (float)(amp * cos(2 * M_PI * hz * i / fs)); v[2 * i + 1] = (float)(amp * sin(2 * M_PI * hz * i / fs)); }
    return v;
}

// ---------------------------------------------------------------- manifest
void testManifest() {
    Manifest m; std::string err;
    CHECK(parseManifest(manifestFor("x.so"), m, err, "0.2.7"), "valid manifest: %s", err.c_str());
    CHECK(m.id == "fake" && m.modes.size() == 9 && m.settings.size() == 1, "fields parsed");
    CHECK(!parseManifest("", m, err), "empty refused");
    CHECK(!parseManifest("{not json", m, err), "malformed refused");
    CHECK(!parseManifest("[1,2]", m, err), "array refused");
    CHECK(!parseManifest("{}", m, err), "missing fields refused");
    std::string deep(200, '['); deep += std::string(200, ']');
    CHECK(!parseManifest(deep, m, err), "deep nesting refused");
    CHECK(!parseManifest(std::string(70000, ' ') + "{}", m, err), "over 64 KiB refused");
    CHECK(!parseManifest(std::string("{\"id\":\"") + std::string(10000, 'a') + "\"}", m, err), "huge id refused");
    for (const char* bad : {"../evil", "A", "a/b", "a b", "-x", ".hidden", "a..b", ""}) {
        std::string t = manifestFor("x.so"); const size_t p = t.find("\"fake\""); t.replace(p, 6, std::string("\"") + bad + "\"");
        CHECK(!parseManifest(t, m, err), "id '%s' refused", bad);
    }
    for (const char* lib : {"../x.so", "/etc/passwd", "a/b.so", "..\\\\x.dll", ".hidden", "c:x.dll"}) {
        CHECK(!parseManifest(manifestFor(lib), m, err), "library '%s' refused", lib);
    }
    CHECK(!parseManifest(manifestFor("x.so", ",\"abi_version\":2"), m, err), "duplicate key/ABI 2 refused (%s)", err.c_str());
    std::string t = manifestFor("x.so"); t.replace(t.find("\"abi_version\":1"), 15, "\"abi_version\":7");
    CHECK(!parseManifest(t, m, err) && err.find("ABI") != std::string::npos, "ABI mismatch message: %s", err.c_str());
    CHECK(!parseManifest(manifestFor("x.so", ",\"minimum_onair\":\"9.0.0\""), m, err, "0.2.7"), "needs a newer OnAir refused");
    CHECK(parseManifest(manifestFor("x.so", ",\"minimum_onair\":\"0.2.7\""), m, err, "0.2.10"), "0.2.10 >= 0.2.7");
    CHECK(!parseManifest(manifestFor("x.so", "", "[\"root\"]"), m, err), "unknown permission refused");
    CHECK(!parseManifest(manifestFor("x.so", "", "\"network\""), m, err), "permissions must be a list");
    t = manifestFor("x.so"); t.replace(t.find("\"kind\":\"mode\""), 14, "\"kind\":\"banana\"");
    CHECK(!parseManifest(t, m, err), "unknown kind refused");
    CHECK(!parseManifest(manifestFor("x.so", ",\"homepage\":\"javascript:alert(1)\""), m, err), "non-http homepage refused");
    t = manifestFor("x.so"); t.replace(t.find("{\"id\":\"ok\",\"name\":\"Ok\"}"), 24, "{\"id\":\"loop\",\"name\":\"Ok\"}");
    CHECK(!parseManifest(t, m, err), "duplicate mode id refused");
    CHECK(compareVersions("0.2.10", "0.2.9") > 0 && compareVersions("1.0", "1.0.0") == 0 && compareVersions("0.2.7-beta", "0.2.7") == 0 && compareVersions("0.1", "0.2") < 0, "version compare");
    CHECK(permissionNames(PermNetwork | PermSpawn).size() == 2, "permission names");
    // settings
    Manifest ms; parseManifest(manifestFor("x.so"), ms, err);
    SettingValues sv(ms);
    CHECK(sv.number("gain", -1) == 2, "default setting");
    CHECK(sv.set("gain", "99") && sv.number("gain", -1) == 10, "slider clamps");
    CHECK(!sv.set("nope", "1") && !sv.set("gain", "abc"), "unknown key / bad number refused");
}

// ---------------------------------------------------------------- results
void testResults() {
    Results r;
    CHECK(r.defineTable("t", "T", {{"a", 0}, {"b", 1}}) == kOk, "define");
    for (int i = 0; i < 5000; i++) r.addRow("t", {Cell{0, 0, 0, std::string(5000, 'x')}, Cell{1, i, 0, ""}});
    Snapshot s = r.snapshot();
    CHECK(s.tables[0].rows.size() == kMaxRows && s.tables[0].rowsTotal == 5000, "rows capped at %zu", kMaxRows);
    CHECK(s.tables[0].rows.back()[0].s.size() == kMaxCellText, "cell text cut");
    CHECK(s.tables[0].rows.back()[1].i == 4999, "newest rows kept");
    CHECK(r.addRow("nope", {Cell{}}) == kBadArg && r.addRow("t", {Cell{}, Cell{}, Cell{}}) == kLimit, "bad table / too many cells");
    std::vector<float> big(200000, 0.5f);
    r.setPlot("p", "P", 1, big.data(), big.size(), 0, 1);
    CHECK(r.snapshot().plots[0].data.size() == kMaxPlotFloats, "plot capped");
    float nanv[2] = {NAN, INFINITY};
    r.setPlot("q", "Q", 0, nanv, 2, 0, 0);
    CHECK(r.snapshot().plots[1].data[0] == 0 && r.snapshot().plots[1].dx == 1, "non-finite values cleaned");
    std::vector<uint8_t> img(100, 1);
    CHECK(r.setImage("i", 0, 10, 10, img.data(), 100) == kBadArg, "RGBA size must match");
    CHECK(r.setImage("i", 1, 0, 0, img.data(), 100) == kBadArg, "PNG signature checked");
    for (int i = 0; i < 20; i++) r.addLog(std::string(5000, 'l'));
    CHECK(r.snapshot().log.back().size() == kMaxLogLine, "log line cut");
    for (int i = 0; i < 2000; i++) r.addLog("x");
    CHECK(r.snapshot().log.size() == kMaxLogLines, "log capped");
    CHECK(r.setPoint("m", "x", 91, 0) == kBadArg && r.setPoint("m", "x", 54.7, 25.3) == kOk, "map point range");
    for (int i = 0; i < 100; i++) r.setStatus("k" + std::to_string(i), "t", 1);
    CHECK(r.snapshot().status.size() == kMaxStatus && r.refused() > 0, "status capped, refusals counted");
    // wire round trip and hostile messages
    Results r2;
    wireApply(r2, Wire::TableDefine, nullptr, 0);   // must not crash
    auto def = wireTableDefine("w", "W", {{"c", 2}});
    CHECK(wireApply(r2, Wire::TableDefine, def.data(), def.size()), "wire define");
    auto row = wireTableRow("w", {Cell{2, 0, 1.5, ""}});
    CHECK(wireApply(r2, Wire::TableRow, row.data(), row.size()) && r2.snapshot().tables[0].rows.size() == 1, "wire row");
    CHECK(!wireApply(r2, Wire::TableRow, row.data(), row.size() - 3), "truncated message refused");
    std::vector<uint8_t> evil = {0xff, 0xff, 0xff, 0x7f};
    CHECK(!wireApply(r2, Wire::TableRow, evil.data(), evil.size()), "oversized length refused");
    auto pl = wirePlot("pp", "P", 0, big.data(), 10, 0, 1);
    CHECK(wireApply(r2, Wire::Plot, pl.data(), pl.size()), "wire plot");
    pl.resize(pl.size() - 8);
    CHECK(!wireApply(r2, Wire::Plot, pl.data(), pl.size()), "short plot refused");
}

// ---------------------------------------------------------------- zip
void testZip() {
    std::string err;
    auto z = zipStore({{"manifest.json", {'{', '}'}}, {"a/b.txt", {1, 2, 3}}});
    ZipReader r;
    CHECK(r.open(z, err) && r.entries().size() == 2, "zip opens: %s", err.c_str());
    std::vector<uint8_t> out;
    CHECK(r.read(r.entries()[1], out, err) && out.size() == 3, "zip reads");
    auto bad = zipStore({{"../evil", {1}}});
    CHECK(!ZipReader().open(bad, err), "path traversal refused");
    CHECK(!safeZipName("/abs") && !safeZipName("a\\b") && !safeZipName("a/../b") && !safeZipName("c:x") && safeZipName("a/b/c.so"), "safe names");
    auto corrupt = z; corrupt[43] ^= 0xff;
    ZipReader r2; bool opened = r2.open(corrupt, err);
    if (opened) { bool anyBad = false; for (auto& e : r2.entries()) { std::vector<uint8_t> o; if (!r2.read(e, o, err)) anyBad = true; } CHECK(anyBad, "corrupt data caught by CRC"); }
    CHECK(!ZipReader().open({1, 2, 3}, err), "garbage refused");
}

// ---------------------------------------------------------------- libraries and runners
StartOptions opts(const Manifest& m, const std::string& mode, uint32_t granted, std::shared_ptr<Results> res, double wd = 0.5) {
    StartOptions o;
    o.libraryPath = gFake; o.manifest = &m; o.modeId = mode; o.granted = granted; o.results = res; o.watchdogSeconds = wd; o.hostExe = gHost;
    o.services.log = [](int, const std::string&) {};
    return o;
}
std::string statusText(Results& r, const std::string& key) { for (auto& s : r.snapshot().status) if (s.key == key) return s.text; return {}; }

void testAbi() {
    Manifest m; std::string err;
    parseManifest(manifestFor(base(gFake)), m, err);
    CHECK(probeLibrary(gFake, m, err), "fake loads: %s", err.c_str());
    Manifest wrongId = m; wrongId.id = "other";
    CHECK(!probeLibrary(gFake, wrongId, err) && err.find("id") != std::string::npos, "library id must match manifest: %s", err.c_str());
    Manifest m2; parseManifest(manifestFor(base(gFakeBad)), m2, err);
    CHECK(!probeLibrary(gFakeBad, m2, err) && err.find("ABI") != std::string::npos, "library with wrong ABI refused: %s", err.c_str());
    CHECK(!probeLibrary("/nonexistent/lib.so", m, err), "missing library refused");
    CHECK(!probeLibrary(gHost, m, err), "executable without entry refused: %s", err.c_str());
    auto res = std::make_shared<Results>();
    auto o = opts(m, "refuse", 0, res);
    CHECK(!startInProcess(o, err) && err.find("refused") != std::string::npos, "create returning NULL: %s", err.c_str());
}

void testInProcess() {
    Manifest m; std::string err;
    parseManifest(manifestFor(base(gFake)), m, err);
    auto res = std::make_shared<Results>();
    auto r = startInProcess(opts(m, "ok", 0, res), err);
    CHECK(r != nullptr, "start: %s", err.c_str());
    if (!r) return;
    r->configure(100e6, 1e6);
    auto iq = tone(4096, 1e6, 1000, 0.5);
    for (int i = 0; i < 10; i++) r->feedIq(iq.data(), 4096, IqMeta{100e6, 1e6, 1.0, (uint64_t)i * 4096});
    CHECK(waitFor([&] { return r->processedBlocks() == 10; }, 3), "10 blocks processed (%llu)", (unsigned long long)r->processedBlocks());
    CHECK(statusText(*res, "n").find("40960 samples at 100000000 Hz / 1000000 sps") == 0, "status from the plugin: '%s'", statusText(*res, "n").c_str());
    CHECK(res->snapshot().log.size() == 1 && res->snapshot().log[0] == "100000000 1000000", "configure reached the plugin");
    auto t0 = Clock::now();
    r->stop();
    CHECK(secSince(t0) < 1.0 && r->state() == RunState::Stopped, "clean stop is quick");
}

void testWatchdog() {
    Manifest m; std::string err;
    parseManifest(manifestFor(base(gFake)), m, err);
    auto res = std::make_shared<Results>();
    auto r = startInProcess(opts(m, "loop", 0, res, 0.4), err);
    CHECK(r != nullptr, "start loop: %s", err.c_str());
    if (!r) return;
    auto iq = tone(1024, 1e6, 1000, 0.5);
    auto t0 = Clock::now();
    r->feedIq(iq.data(), 1024, IqMeta{100e6, 1e6, 1.0, 0});
    CHECK(waitFor([&] { return r->state() == RunState::Hung; }, 3), "looping plugin detected as hung");
    CHECK(secSince(t0) < 2.0, "...within the limit (%.2f s)", secSince(t0));
    CHECK(r->lastError().find("stopped answering") != std::string::npos, "error: %s", r->lastError().c_str());
    for (int i = 0; i < 5; i++) r->feedIq(iq.data(), 1024, IqMeta{});
    CHECK(r->droppedBlocks() >= 5, "nothing is queued for a hung plugin");
    t0 = Clock::now();
    r->stop();
    CHECK(secSince(t0) < 1.5, "stop does not wait for the stuck thread (%.2f s)", secSince(t0));
    // a plugin whose create() never returns
    auto r2 = startInProcess(opts(m, "hang-create", 0, res, 0.3), err);
    CHECK(!r2 && err.find("stopped answering") != std::string::npos, "hung create refused: %s", err.c_str());
}

void testFloodAndPerms() {
    Manifest m; std::string err;
    parseManifest(manifestFor(base(gFake)), m, err);
    auto res = std::make_shared<Results>();
    auto r = startInProcess(opts(m, "flood", 0, res), err);
    CHECK(r != nullptr, "flood start: %s", err.c_str());
    if (r) {
        const std::string s = statusText(*res, "flood");
        CHECK(s == "tables=8 plot=0 img=-2 badimg=-1 map=-1", "limits seen by the plugin: %s", s.c_str());
        Snapshot sn = res->snapshot();
        CHECK(sn.tables.size() == kMaxTables && sn.tables[0].rows.size() == kMaxRows, "bounded tables");
        CHECK(res->approxBytes() < 16u << 20, "memory stays bounded (%zu bytes)", res->approxBytes());
        r->stop();
    }
    auto res2 = std::make_shared<Results>();
    auto o = opts(m, "perm", 0, res2);
    o.services.dataDir = "/tmp/x"; o.services.audioOut = [](const float*, size_t, double) { return 0; };
    auto r2 = startInProcess(o, err);
    CHECK(r2 && statusText(*res2, "perm") == "audio=-3 dir=-3 path=", "no permission: both refused (%s)", statusText(*res2, "perm").c_str());
    if (r2) r2->stop();
    auto res3 = std::make_shared<Results>();
    o = opts(m, "perm", PermAudioOut | PermFilesystem, res3);
    o.services.dataDir = "/tmp/x"; o.services.audioOut = [](const float*, size_t, double) { return 0; };
    auto r3 = startInProcess(o, err);
    CHECK(r3 && statusText(*res3, "perm") == "audio=0 dir=0 path=/tmp/x", "permissions granted: %s", statusText(*res3, "perm").c_str());
    if (r3) r3->stop();
}

void testProcess() {
#ifndef _WIN32
    Manifest m; std::string err;
    parseManifest(manifestFor(base(gFake)), m, err);
    auto res = std::make_shared<Results>();
    auto r = startProcess(opts(m, "ok", 0, res, 1.0), err);
    CHECK(r != nullptr, "helper start: %s", err.c_str());
    if (r) {
        r->configure(433e6, 2e6);
        auto iq = tone(70000, 2e6, 1000, 0.5);   // more than one pipe chunk
        r->feedIq(iq.data(), 70000, IqMeta{433e6, 2e6, 5.0, 0});
        CHECK(waitFor([&] { return statusText(*res, "n").find("70000 samples at 433000000 Hz / 2000000 sps") == 0 || statusText(*res, "n") == "4464 samples at 433000000 Hz / 2000000 sps"; }, 5), "IQ in, results out: '%s'", statusText(*res, "n").c_str());
        CHECK(waitFor([&] { return res->snapshot().log.size() == 1; }, 3) && res->snapshot().log[0] == "433000000 2000000", "configure reached the helper");
        r->stop();
        CHECK(r->state() == RunState::Stopped, "helper stopped");
    }
    // a looping plugin: the helper is killed
    res = std::make_shared<Results>();
    r = startProcess(opts(m, "loop", 0, res, 0.5), err);
    CHECK(r != nullptr, "helper loop start: %s", err.c_str());
    if (r) {
        auto iq = tone(1024, 1e6, 1000, 0.5);
        auto t0 = Clock::now();
        r->feedIq(iq.data(), 1024, IqMeta{1e6, 1e6, 1, 0});
        CHECK(waitFor([&] { return r->state() == RunState::Hung; }, 4) && secSince(t0) < 3, "helper of a looping plugin killed (%.2f s)", secSince(t0));
        r->stop();
    }
    // a crashing plugin: only the helper dies
    res = std::make_shared<Results>();
    r = startProcess(opts(m, "crash", 0, res, 1.0), err);
    CHECK(r != nullptr, "helper crash start: %s", err.c_str());
    if (r) {
        auto iq = tone(1024, 1e6, 1000, 0.5);
        r->feedIq(iq.data(), 1024, IqMeta{1e6, 1e6, 1, 0});
        CHECK(waitFor([&] { return r->state() == RunState::Failed; }, 4), "crash in the helper is reported as a failure, this process lives");
        r->stop();
    }
    // refusal and a missing helper
    res = std::make_shared<Results>();
    CHECK(!startProcess(opts(m, "refuse", 0, res, 1.0), err) && err.find("refused") != std::string::npos, "helper: create NULL reported: %s", err.c_str());
    auto o = opts(m, "ok", 0, res); o.hostExe = "/nonexistent/host";
    CHECK(!startProcess(o, err), "missing helper refused");
    // a wrong-ABI library is refused inside the helper
    o = opts(m, "ok", 0, res); o.libraryPath = gFakeBad; o.manifest = &m;
    CHECK(!startProcess(o, err) && err.find("ABI") != std::string::npos, "helper: wrong ABI refused: %s", err.c_str());
#endif
}

// ---------------------------------------------------------------- manager
ManagerConfig cfgFor(const fs::path& dir, const std::string& bundled = "") {
    ManagerConfig c;
    c.pluginsDir = (dir / "plugins").string(); c.bundledDir = bundled; c.hostVersion = "0.2.7"; c.hostExe = gHost;
    return c;
}

void testManager() {
    Env env("mgr");
    installFake(env.dir / "plugins");
    std::string err;
    {
        Manager m(cfgFor(env.dir));
        m.scan(); m.beginSession();
        auto es = m.entries();
        CHECK(es.size() == 1 && es[0].valid && !es[0].enabled, "discovered, off by default (%s)", es.empty() ? "" : es[0].error.c_str());
        CHECK(m.start("fake", "ok", err) == nullptr && err.find("disabled") != std::string::npos, "a disabled plugin does not start: %s", err.c_str());
        CHECK(m.setEnabled("fake", true, err), "enable");
        auto rows = m.modeRows();
        CHECK(rows.size() == 9 && rows[0].number == 1000 && rows[1].number == 1001 && rows[0].available, "mode numbers from 1000 (%zu)", rows.size());
        // permissions prompt
        std::string e2; installFake(env.dir / "plugins", "[\"filesystem\",\"audio-out\"]", "fake"); m.scan();
        CHECK(m.entries()[0].needsPrompt(), "permissions need a first-use answer");
        CHECK(m.start("fake", "ok", err) == nullptr && err.find("permissions") != std::string::npos, "not started before the answer: %s", err.c_str());
        m.setEnabled("fake", true, e2);
        CHECK(m.setGranted("fake", PermFilesystem, err), "answer the prompt");
        auto en = m.entries()[0];
        CHECK(en.granted == PermFilesystem && !en.needsPrompt(), "only the approved permission is granted");
        Runner* r = m.start("fake", "perm", err);
        CHECK(r != nullptr, "start with permissions: %s", err.c_str());
        if (r) {
            auto res = m.results("fake", "perm");
            CHECK(waitFor([&] { return !statusText(*res, "perm").empty(); }, 2) && statusText(*res, "perm").find("audio=-3 dir=0 path=") == 0, "filesystem yes, audio no: %s", statusText(*res, "perm").c_str());
            CHECK(fs::exists(env.dir / "plugins" / ".running"), "crash marker written while plugin code runs");
        }
        m.stop("fake", "perm");
        CHECK(!fs::exists(env.dir / "plugins" / ".running"), "marker removed when the last plugin stopped");
        // settings persisted and delivered
        CHECK(m.setSetting("fake", "gain", "7.5") && !m.setSetting("fake", "bogus", "1"), "settings");
        r = m.start("fake", "gain", err);
        if (r) {
            auto res = m.results("fake", "gain");
            auto iq = tone(256, 1e6, 100, 0.5);
            r->feedIq(iq.data(), 256, IqMeta{1e6, 1e6, 0, 0});
            CHECK(waitFor([&] { return statusText(*res, "gain") == "gain=7.50"; }, 2), "the plugin reads the setting: %s", statusText(*res, "gain").c_str());
        }
        // events: plugin emit -> automation
        std::vector<automation::Event> seen;
        // (observer for plugin.* events)
        automation::global().setTap([&](const automation::Event& e) { seen.push_back(e); }, automation::familyBit(automation::FPlugin));
        r = m.start("fake", "event", err);
        if (r) {
            auto iq = tone(16, 1e6, 100, 0.5);
            r->feedIq(iq.data(), 16, IqMeta{1e6, 1e6, 0, 0});
            CHECK(waitFor([&] { return !seen.empty(); }, 2), "plugin event reached the automation engine");
            if (!seen.empty()) CHECK(seen[0].type == "plugin.hello_world" && seen[0].f.count("n") && seen[0].f["who"].s == "fake" && seen[0].mode == "plugin:fake", "event fields: %s", seen[0].type.c_str());
        }
        automation::global().setTap(nullptr, 0);
        m.stopAll();
        m.endSession();
    }
    // numbering survives a restart, a new plugin and a removal
    {
        installFake(env.dir / "plugins", "[]", "fake2");   // manifest says id fake2 with the fake library: never started
        Manager m(cfgFor(env.dir));
        m.scan();
        auto rows = m.modeRows();
        int n_fake_ok = -1, n_fake2_ok = -1;
        for (auto& r : rows) { if (r.pluginId == "fake" && r.modeId == "ok") n_fake_ok = r.number; if (r.pluginId == "fake2" && r.modeId == "ok") n_fake2_ok = r.number; }
        CHECK(n_fake_ok == 1000, "fake/ok keeps 1000 after a restart (%d)", n_fake_ok);
        CHECK(n_fake2_ok == 1009, "new plugin numbers continue after the old ones (%d)", n_fake2_ok);
        std::string pid, mid;
        CHECK(m.modeFromNumber(1009, pid, mid) && pid == "fake2" && mid == "ok", "number to mode");
        CHECK(m.remove("fake2", err), "remove: %s", err.c_str());
        CHECK(!fs::exists(env.dir / "plugins" / "fake2"), "folder gone");
        installFake(env.dir / "plugins", "[]", "fake2"); m.scan();
        CHECK(m.modeNumber("fake2", "ok") == 1009, "a reinstalled plugin gets its number back");
        installFake(env.dir / "plugins", "[]", "fake3"); m.scan();
        CHECK(m.modeNumber("fake3", "ok") > 1017, "numbers are never reused (%d)", m.modeNumber("fake3", "ok"));
    }
}

void testSafeMode() {
    Env env("safe");
    installFake(env.dir / "plugins");
    std::string err;
    {
        Manager m(cfgFor(env.dir));
        m.scan(); m.beginSession();
        CHECK(!m.safeMode(), "fresh start: no safe mode");
        m.setEnabled("fake", true, err);
        CHECK(m.start("fake", "ok", err) != nullptr, "start: %s", err.c_str());
        // simulate a crash: leave the marker behind (a real crash never runs the destructor)
        const std::string marker = readAll((env.dir / "plugins" / ".running").string());
        m.stopAll();
        writeFile(env.dir / "plugins" / ".running", marker);
    }
    {
        Manager m(cfgFor(env.dir));
        m.scan(); m.beginSession();
        CHECK(m.safeMode() && m.safeModeReason().find("fake") != std::string::npos, "next start is in safe mode: %s", m.safeModeReason().c_str());
        CHECK(m.start("fake", "ok", err) == nullptr && err.find("safe mode") != std::string::npos, "nothing starts in safe mode: %s", err.c_str());
        CHECK(!m.entries()[0].enabled, "the plugin that was running is off");
        for (auto& r : m.modeRows()) CHECK(!r.available, "modes unavailable in safe mode");
    }
    {
        Manager m(cfgFor(env.dir));
        m.scan(); m.beginSession();
        CHECK(m.safeMode(), "safe mode stays on until the user leaves it");
        m.leaveSafeMode();
        CHECK(!m.safeMode(), "left safe mode");
        m.setEnabled("fake", true, err);
        CHECK(m.start("fake", "ok", err) != nullptr, "starts again: %s", err.c_str());
        m.stopAll(); m.endSession();
    }
    {
        Manager m(cfgFor(env.dir));
        m.scan(); m.beginSession();
        CHECK(!m.safeMode(), "and the next start is normal");
        m.forceSafeMode(true);
        CHECK(m.safeMode() && m.start("fake", "ok", err) == nullptr, "forced safe mode");
    }
#ifndef _WIN32   // the helper-process mode is not available on Windows yet
    {   // a failed plugin is switched off by the manager's tick
        Env e2("fail");
        installFake(e2.dir / "plugins");
        Manager m(cfgFor(e2.dir));
        m.scan(); m.beginSession(); m.setEnabled("fake", true, err);
        m.setOutOfProcess("fake", true);
        Runner* r = m.start("fake", "crash", err);
        CHECK(r != nullptr, "start crash mode in a helper: %s", err.c_str());
        if (r) {
            auto iq = tone(64, 1e6, 100, 0.5);
            r->feedIq(iq.data(), 64, IqMeta{1e6, 1e6, 0, 0});
            CHECK(waitFor([&] { return r->state() == RunState::Failed; }, 4), "failed");
            m.tick(0);
            CHECK(m.running("fake", "crash") == nullptr && !m.entries()[0].enabled, "tick stops and disables a failed plugin");
            CHECK(!m.log("fake").empty(), "failure is in the plugin log");
        }
        CHECK(!fs::exists(e2.dir / "plugins" / ".running"), "a helper-process plugin leaves no crash marker");
    }
#endif
}

void testInstall() {
    Env env("install");
    Manager m(cfgFor(env.dir));
    m.scan();
    std::string err, id;
    const std::string lib = readAll(gFake);
    std::vector<uint8_t> libBytes(lib.begin(), lib.end());
    const std::string mf = manifestFor(base(gFake));
    auto good = zipStore({{"manifest.json", std::vector<uint8_t>(mf.begin(), mf.end())}, {base(gFake), libBytes}});
    CHECK(m.installBytes(good, id, err) && id == "fake", "install: %s", err.c_str());
    CHECK(fs::exists(env.dir / "plugins" / "fake" / base(gFake)) && m.entries().size() == 1 && m.entries()[0].valid, "installed and found");
    auto top = zipStore({{"fake/manifest.json", std::vector<uint8_t>(mf.begin(), mf.end())}, {"fake/" + base(gFake), libBytes}});
    CHECK(m.installBytes(top, id, err), "a single top folder is accepted: %s", err.c_str());
    CHECK(!m.installBytes(zipStore({{"manifest.json", std::vector<uint8_t>(mf.begin(), mf.end())}}), id, err) && err.find("library") != std::string::npos, "no library for this platform: %s", err.c_str());
    CHECK(!m.installBytes(zipStore({{base(gFake), libBytes}}), id, err), "no manifest refused: %s", err.c_str());
    CHECK(!m.installBytes(zipStore({{"manifest.json", {'{', '}'}}}), id, err), "bad manifest refused");
    CHECK(!m.installBytes(zipStore({{"manifest.json", std::vector<uint8_t>(mf.begin(), mf.end())}, {"../../evil.so", {1}}}), id, err), "path traversal refused: %s", err.c_str());
    CHECK(!fs::exists(env.dir / "evil.so") && !fs::exists(env.dir.parent_path() / "evil.so"), "nothing written outside");
    CHECK(!m.installBytes({1, 2, 3, 4}, id, err), "garbage refused");
    std::string newer = manifestFor(base(gFake), ",\"minimum_onair\":\"9.9.9\"");
    CHECK(!m.installBytes(zipStore({{"manifest.json", std::vector<uint8_t>(newer.begin(), newer.end())}, {base(gFake), libBytes}}), id, err) && err.find("newer") != std::string::npos, "needs a newer OnAir: %s", err.c_str());
    CHECK(!m.remove("nonexistent", err), "remove unknown");
    CHECK(m.remove("fake", err) && m.entries().empty(), "remove");
}

void testExamples() {
    Env env("examples");
    Manager m(cfgFor(env.dir, gExamples));
    m.scan(); m.beginSession();
    std::string err;
    auto es = m.entries();
    int valid = 0;
    for (auto& e : es) if (e.valid) valid++; else printf("  example %s: %s\n", e.m.id.c_str(), e.error.c_str());
    CHECK(es.size() == 3 && valid == 3, "three bundled examples are valid (%zu / %d)", es.size(), valid);
    if (valid != 3) return;
    for (auto& e : es) CHECK(e.bundled, "marked bundled");
    // tone detector: a tone at +50 kHz
    m.setEnabled("tone-detector", true, err);
    Runner* r = m.start("tone-detector", "tone", err);
    CHECK(r != nullptr, "tone detector starts: %s", err.c_str());
    if (r) {
        r->configure(100e6, 1e6);
        auto iq = tone(4096 * 8, 1e6, 50000, 0.5);
        r->feedIq(iq.data(), 4096 * 8, IqMeta{100e6, 1e6, 10.0, 0});
        auto res = m.results("tone-detector", "tone");
        CHECK(waitFor([&] { return statusText(*res, "tone").find("Hz") != std::string::npos; }, 3), "tone reported");
        const std::string s = statusText(*res, "tone");
        double hz = 0, db = 0; sscanf(s.c_str(), "%lf Hz from centre, %lf dBFS", &hz, &db);
        CHECK(std::fabs(hz - 50000) < 300 && db > -12 && db < 0, "tone at +50 kHz, level near -6 dBFS: '%s'", s.c_str());
        auto sn = res->snapshot();
        CHECK(sn.plots.size() == 1 && sn.plots[0].data.size() == 4096, "spectrum plot delivered");
        m.stop("tone-detector", "tone");
    }
    // burst counter: noise with three bursts
    m.setEnabled("burst-counter", true, err);
    r = m.start("burst-counter", "bursts", err);
    CHECK(r != nullptr, "burst counter starts: %s", err.c_str());
    if (r) {
        r->configure(433.92e6, 1e6);
        const size_t N = 1000000;
        std::vector<float> iq(2 * N);
        unsigned seed = 1;
        auto rnd = [&] { seed = seed * 1664525u + 1013904223u; return ((seed >> 8) & 0xffff) / 65536.0f - 0.5f; };
        for (size_t i = 0; i < N; i++) { iq[2 * i] = 0.01f * rnd(); iq[2 * i + 1] = 0.01f * rnd(); }
        for (size_t b : {100000u, 400000u, 700000u}) for (size_t i = b; i < b + 20000; i++) { iq[2 * i] += 0.3f * (float)cos(0.5 * i); iq[2 * i + 1] += 0.3f * (float)sin(0.5 * i); }
        r->feedIq(iq.data(), N, IqMeta{433.92e6, 1e6, 100.0, 0});
        auto res = m.results("burst-counter", "bursts");
        CHECK(waitFor([&] { return statusText(*res, "count") == "3 bursts"; }, 4), "three bursts counted: '%s'", statusText(*res, "count").c_str());
        auto sn = res->snapshot();
        if (!sn.tables.empty() && sn.tables[0].rows.size() == 3) {
            const auto& row = sn.tables[0].rows[0];
            CHECK(std::fabs(row[1].d - 100.1) < 0.01 && std::fabs(row[2].d - 20) < 2, "first burst at 100.1 s, 20 ms long (%.3f, %.1f)", row[1].d, row[2].d);
        } else CHECK(false, "burst table has 3 rows");
        m.stop("burst-counter", "bursts");
    }
    // log to file (action): needs the filesystem permission
    m.setEnabled("log-to-file", true, err);
    CHECK(m.start("log-to-file", "", err) == nullptr && err.find("permissions") != std::string::npos, "action waits for its permission prompt");
    m.setGranted("log-to-file", PermFilesystem, err);
    m.setSetting("log-to-file", "prefix", "adsb.");
    r = m.start("log-to-file", "", err);
    CHECK(r != nullptr, "log-to-file starts: %s", err.c_str());
    if (r) {
        automation::Event a("adsb.aircraft"); a.at(1792232100.0, "ADS-B", 1090e6); a.str("hex", "4CA2B1").num("alt_ft", 12000);
        automation::Event b("pager.message"); b.at(1792232101.0, "POCSAG", 466e6); b.str("text", "hello");
        m.dispatchEvent(a); m.dispatchEvent(b);
        const fs::path f = env.dir / "plugins" / "log-to-file" / "data" / "events.log";
        CHECK(waitFor([&] { return fs::exists(f) && readAll(f.string()).find("adsb.aircraft") != std::string::npos; }, 3), "event written");
        const std::string text = readAll(f.string());
        CHECK(text.find("hex=\"4CA2B1\"") != std::string::npos && text.find("alt_ft=12000") != std::string::npos && text.find("pager.message") == std::string::npos, "fields written, prefix filter applied: %s", text.c_str());
        m.stopAll();
    }
    m.endSession();
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 5) { printf("usage: test_plugin <onair-plugin-host> <examples dir> <fake lib> <fake lib bad abi>\n"); return 2; }
    gHost = argv[1]; gExamples = argv[2]; gFake = argv[3]; gFakeBad = argv[4];
    testManifest();
    testResults();
    testZip();
    testAbi();
    testInProcess();
    testWatchdog();
    testFloodAndPerms();
    testProcess();
    testManager();
    testSafeMode();
    testInstall();
    testExamples();
    return testSummary("plugin");
}
