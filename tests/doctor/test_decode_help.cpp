// Decode help: the stage ladder, the automatic opening, the failed-signal clip and the antenna and gain coach.
#include "dect2/doctor/decode_progress.h"
#include <cstdio>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>
using namespace dect2::doctor;
static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static Report rep(std::initializer_list<Code> codes) {
    Report r; r.valid = true;
    for (Code c : codes) { Finding f; f.code = c; r.findings.push_back(f); }
    return r;
}

static void testStages() {
    StageInput in;
    Progress p = progress(Report(), in);
    CHECK(p.signal == StageState::No); CHECK(p.carrier == StageState::Unknown); CHECK(p.sync == StageState::Unknown); CHECK(p.fec == StageState::Unknown); CHECK(p.furthest == -1);
    in.samples = true;
    p = progress(rep({Code::NoSignal}), in);
    CHECK(p.signal == StageState::Yes); CHECK(p.carrier == StageState::No); CHECK(p.furthest == 0);
    p = progress(rep({Code::WeakSignal}), in); CHECK(p.carrier == StageState::Partial); CHECK(p.furthest == 1);
    p = progress(rep({Code::LooksFine}), in); CHECK(p.carrier == StageState::Yes); CHECK(p.furthest == 1);
    in.syncState = 0; p = progress(rep({Code::LooksFine}), in); CHECK(p.sync == StageState::No); CHECK(p.furthest == 1);
    in.syncState = 1; p = progress(rep({Code::LooksFine}), in); CHECK(p.sync == StageState::Partial); CHECK(p.furthest == 2);
    in.syncState = 2; in.blocksOk = 0; p = progress(rep({Code::LooksFine}), in); CHECK(p.sync == StageState::Yes); CHECK(p.fec == StageState::No); CHECK(p.furthest == 2);
    in.blocksOk = 10; in.blocksBad = 2; p = progress(rep({Code::LooksFine}), in); CHECK(p.fec == StageState::Yes); CHECK(p.furthest == 3);
    in.blocksOk = 10; in.blocksBad = 100; p = progress(rep({Code::LooksFine}), in); CHECK(p.fec == StageState::Partial);
    in.blocksOk = 0; in.blocksBad = 5; p = progress(rep({Code::LooksFine}), in); CHECK(p.fec == StageState::No);
    // good frames prove the carrier even when the short capture looked empty
    StageInput f; f.samples = true; f.syncState = -1; f.blocksOk = 3;
    p = progress(rep({Code::NoSignal}), f); CHECK(p.carrier == StageState::Yes); CHECK(p.sync == StageState::Yes); CHECK(p.fec == StageState::Yes);
    // a decoder that reports frames but no lock yet (state 0, a burst just passed)
    f.syncState = 0; p = progress(rep({Code::LooksFine}), f); CHECK(p.sync == StageState::Partial);
    // no doctor report yet: the carrier stage is unknown unless the decoder proves it
    StageInput s; s.samples = true; s.syncState = 2; p = progress(Report(), s); CHECK(p.carrier == StageState::Yes); CHECK(p.fec == StageState::Unknown);
    s.syncState = 0; p = progress(Report(), s); CHECK(p.carrier == StageState::Unknown);
}

static void testAutoOpen() {
    AutoOpen a;
    CHECK(!a.update(0, false, false, "dmr"));
    CHECK(!a.update(1, true, false, "dmr"));
    CHECK(!a.update(20, true, false, "dmr"));
    CHECK(a.update(27, true, false, "dmr"));    // 26 s of nothing
    CHECK(!a.update(60, true, false, "dmr"));   // once
    // a new mode starts over
    CHECK(!a.update(61, true, false, "ais"));
    CHECK(a.update(90, true, false, "ais"));
    // stopped and started again starts over
    CHECK(!a.update(100, false, false, "ais"));
    CHECK(!a.update(101, true, false, "ais"));
    CHECK(a.update(130, true, false, "ais"));
    // it decoded: no prompt in this run, even if decoding stops later
    AutoOpen b;
    CHECK(!b.update(0, true, false, "x"));
    CHECK(!b.update(10, true, true, "x"));
    CHECK(!b.update(60, true, false, "x"));
}

static void testClip() {
    namespace fs = std::filesystem;
    // a folder of this run only (tests run in parallel); removed with an error code so a file still held open cannot throw
    const fs::path dir = fs::temp_directory_path() / ("onair_clip_test_" + std::to_string((unsigned long long)std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(dir);
    const std::string base = (dir / "c1").string();
    std::vector<cf32> x(1000);
    for (size_t i = 0; i < x.size(); i++) x[i] = cf32((float)std::cos(0.1 * (double)i), (float)std::sin(0.1 * (double)i)) * 0.01f;   // weak: must be scaled up
    ClipInfo info; info.rateHz = 2.4e6; info.freqHz = 145.5e6; info.modeId = "dmr"; info.note = "say \"hi\"";
    std::string err;
    CHECK(writeClip(base, x.data(), x.size(), info, &err));
    CHECK(fs::file_size(base + ".sigmf-data") == 2000);
    CHECK(clipBytes(1000) == 2000); CHECK(clipBytes(kMaxClipSamples * 3) == kMaxClipSamples * 2);
    std::vector<char> buf(2000);
    { std::ifstream d(base + ".sigmf-data", std::ios::binary); d.read(buf.data(), 2000); }   // closed before the folder is removed (Windows)
    int peak = 0; for (char c : buf) peak = std::max(peak, std::abs((int)c));
    CHECK(peak >= 100);   // uses the 8 bits
    const std::string meta = clipMeta(info);
    CHECK(meta.find("\"core:sample_rate\": 2400000") != std::string::npos);
    CHECK(meta.find("\"core:frequency\": 145500000") != std::string::npos);
    CHECK(meta.find("ci8") != std::string::npos);
    CHECK(meta.find("\\\"hi\\\"") != std::string::npos);
    // nothing about where or who
    CHECK(meta.find("geolocation") == std::string::npos); CHECK(meta.find("recorder") == std::string::npos); CHECK(meta.find("author") == std::string::npos);
    CHECK(!writeClip((dir / "no_such_folder" / "x").string(), x.data(), 10, info, &err));
    std::error_code ec;
    fs::remove_all(dir, ec);
}

static bool has(const Coaching& c, Advice a) { for (Advice x : c.advice) if (x == a) return true; return false; }

static void testCoach() {
    Context live; live.live = true; live.gainDb = 40; live.gainMaxDb = 60;
    Report r = rep({Code::LooksFine}); r.m.rmsDbfs = -25; r.m.peak = 0.3f;
    Coaching c = coach(r, live, false, false);
    CHECK(has(c, Advice::GainGood)); CHECK(c.gainDeltaDb == 0);
    // clipping: down
    Report cl = r; cl.m.clipPct = 5; c = coach(cl, live, false, false); CHECK(has(c, Advice::GainDown)); CHECK(c.gainDeltaDb == 12);
    cl.m.clipPct = 0.5; c = coach(cl, live, false, false); CHECK(c.gainDeltaDb == 6);
    Report pk = r; pk.m.peak = 0.99f; CHECK(has(coach(pk, live, false, false), Advice::GainDown));
    // too quiet: up, limited by the headroom
    Report q = r; q.m.rmsDbfs = -70; c = coach(q, live, false, false); CHECK(has(c, Advice::GainUp)); CHECK(c.gainDeltaDb == 10);
    Context nearMax = live; nearMax.gainDb = 55; c = coach(q, nearMax, false, false); CHECK(c.gainDeltaDb == 5);
    // at full gain and no signal: an LNA, then the antenna
    Context full = live; full.gainDb = 60; Report w = rep({Code::WeakSignal}); w.m.rmsDbfs = -70;
    c = coach(w, full, false, false); CHECK(!has(c, Advice::GainUp)); CHECK(has(c, Advice::LnaHelps)); CHECK(has(c, Advice::MoveAntenna));
    // the wrong antenna is said before anything about placing it
    c = coach(w, full, true, false); CHECK(has(c, Advice::NeedsAntennaForBand)); CHECK(!has(c, Advice::MoveAntenna));
    // a good setup with a known antenna
    c = coach(r, live, true, true); CHECK(has(c, Advice::AntennaOk)); CHECK(has(c, Advice::GainGood));
    // a recording has nothing to coach; an invalid report neither
    Context file; file.live = false;
    CHECK(coach(r, file, true, true).advice[0] == Advice::NoAdvice);
    CHECK(coach(Report(), live, false, false).advice[0] == Advice::NoAdvice);
    // unknown gain range is not treated as full
    Context unk; unk.live = true; c = coach(q, unk, false, false); CHECK(has(c, Advice::GainUp));
}

int main() {
    testStages(); testAutoOpen(); testClip(); testCoach();
    if (fails) { printf("%d failures\n", fails); return 1; }
    printf("decode help ok\n");
    return 0;
}
