// Recording the radio's IQ samples (iq_record.h, Engine::startRecording): 3 s of the built-in FM test signal in both formats. The files have the
// size the length and the rate say, the name reads back with guessSampleRate() / guessFormat(), nothing was dropped, and the float file played
// back as a file gives the same receiver state as the live signal. A file source and a stopped engine refuse to record.
#include "dect2/engine/engine.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <string>
#include <filesystem>
#include <thread>

using namespace dect2;
namespace fs = std::filesystem;

static int fails = 0;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s\n", m); fails++; } } while (0)

static const double kRate = 1e6;

static TuneSettings fmTune() {
    TuneSettings t;
    t.sampleRate = kRate;
    t.centerHz = 93.9e6;
    t.bandwidthMhz = 0.2;
    t.synth.mode = 7;
    t.synth.snrDb = 30;
    return t;
}

// highest FM state (0 searching, 1 tracking, 2 locked) the engine reported while we waited secs
static int watchFm(Engine& e, double secs) {
    int best = 0;
    uint64_t seq = 0;
    const auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < secs) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        RxTelemetry t;
        if (e.latestRx(t, seq)) { seq = t.seq; if (t.standard == 6) best = std::max(best, t.fm.state); }
    }
    return best;
}

static uint64_t record(const fs::path& dir, FileFormat fmt, std::string& path, double& wall, RecordingStats& st, int& liveState) {
    Engine e;
    e.setStandard(7);
    e.player().setMuted(true);
    e.fm().setSilent(true);
    DeviceInfo dev; dev.kind = DeviceInfo::Synthetic; dev.name = "test signal";
    std::string err;
    CHECK(!e.startRecording((dir / "x.cs8").string(), fmt, err) && !err.empty(), "a stopped engine refuses to record");
    CHECK(e.start(dev, fmTune(), FileOptions()), "engine started");
    path = (dir / e.recordingName(fmt)).string();
    CHECK(e.startRecording(path, fmt, err), "recording started");
    const auto t0 = std::chrono::steady_clock::now();
    liveState = watchFm(e, 3.0);
    st = e.recordingStats();
    e.stopRecording();
    wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    st = e.recordingStats();
    CHECK(!st.active, "the recording is stopped");
    e.stop();
    return fs::exists(path) ? (uint64_t)fs::file_size(path) : 0;
}

// 4.0: Settings > Recording and files > IQ file name (the pattern with tokens)
static void namePattern() {
    std::tm loc{}, utc{};
    loc.tm_year = 126; loc.tm_mon = 10; loc.tm_mday = 12; loc.tm_hour = 21; loc.tm_min = 5; loc.tm_sec = 9;
    utc = loc; utc.tm_hour = 17;
    CHECK(expandRecordingPattern("onair_{mode}_{freq}MHz_{rate}Msps_{date}-{time}", "fm", 93.9e6, 2e6, loc, utc) == "onair_fm_93.900MHz_2Msps_20261112-210509", "the default pattern gives the built-in name");
    CHECK(expandRecordingPattern("{utcdate}_{utctime}_{khz}kHz", "ais", 161975000, 1e6, loc, utc) == "20261112_170509Z_161975kHz", "UTC tokens and kHz");
    CHECK(expandRecordingPattern("../{mode}:x|{nope}", "a b", 1e6, 1e6, loc, utc) == "_a-b_x_{nope}", "no path characters, unknown tokens kept");
    setRecordingNamePattern("");
    const std::string def = recordingFileName("fm", 93.9e6, 2e6, FileFormat::CS8);
    CHECK(def.rfind("onair_fm_93.900MHz_2Msps_", 0) == 0 && def.size() > 4 && def.compare(def.size() - 4, 4, ".cs8") == 0, "empty pattern: the 3.9 name");
    setRecordingNamePattern("site1_{mode}_{freq}MHz_{rate}Msps_{utcdate}");
    const std::string n = recordingFileName("fm", 93.9e6, 2.4e6, FileFormat::CF32);
    CHECK(n.rfind("site1_fm_93.900MHz_2.4Msps_", 0) == 0 && n.compare(n.size() - 5, 5, ".cf32") == 0, "a pattern names the file, the extension follows the format");
    CHECK(std::fabs(guessSampleRate(n) - 2.4e6) < 1 && guessFormat(n) == FileFormat::CF32, "a pattern with {freq}MHz and {rate}Msps still reads back");
    setRecordingNamePattern("");
}

int main() {
    namePattern();
    const fs::path dir = fs::temp_directory_path() / "onair_iq_record_test";
    fs::remove_all(dir);
    fs::create_directories(dir);
    std::string pCs8, pCf32;
    double wCs8 = 0, wCf32 = 0;
    RecordingStats sCs8, sCf32;
    int liveCs8 = 0, liveCf32 = 0;
    const uint64_t bCs8 = record(dir, FileFormat::CS8, pCs8, wCs8, sCs8, liveCs8);
    const uint64_t bCf32 = record(dir, FileFormat::CF32, pCf32, wCf32, sCf32, liveCf32);
    printf("  cs8:  %s  %llu bytes in %.2f s, recorded %.2f s, dropped %llu, FM state %d\n", fs::path(pCs8).filename().string().c_str(), (unsigned long long)bCs8, wCs8, sCs8.seconds, (unsigned long long)sCs8.droppedSamples, liveCs8);
    printf("  cf32: %s  %llu bytes in %.2f s, recorded %.2f s, dropped %llu, FM state %d\n", fs::path(pCf32).filename().string().c_str(), (unsigned long long)bCf32, wCf32, sCf32.seconds, (unsigned long long)sCf32.droppedSamples, liveCf32);

    CHECK(std::abs(bCs8 - sCs8.seconds * kRate * 2) < 0.05 * sCs8.seconds * kRate * 2, "CS8 size = seconds x rate x 2 bytes (5 %)");
    CHECK(std::abs(bCf32 - sCf32.seconds * kRate * 8) < 0.05 * sCf32.seconds * kRate * 8, "CF32 size = seconds x rate x 8 bytes (5 %)");
    CHECK(bCs8 == sCs8.bytes && bCf32 == sCf32.bytes, "the reported size is the file size");
    CHECK(sCs8.droppedSamples == 0 && sCf32.droppedSamples == 0, "nothing dropped");
    CHECK(std::abs(guessSampleRate(pCs8) - kRate) < 1 && std::abs(guessSampleRate(pCf32) - kRate) < 1, "the name gives the sample rate back");
    CHECK(guessFormat(pCs8) == FileFormat::CS8 && guessFormat(pCf32) == FileFormat::CF32, "the name gives the format back");
    CHECK(fs::path(pCf32).filename().string().rfind("onair_fm_93.900MHz_1Msps_", 0) == 0, "the name has the mode, the frequency and the rate");
    CHECK(guessFormat("/home/u/iqdatIQ.bin") == FileFormat::CF32 && std::abs(guessSampleRate("/home/u/iqdatIQ.bin") - 1.125e6) < 1, "AOR ariq_rcv iqdatIQ.bin reads as cf32 at 1.125 Msps");
    CHECK(guessFormat("dump.bin") == FileFormat::CS8 && guessSampleRate("dump.bin") == 0, "other .bin names are not taken for AOR files");
    CHECK(std::abs(guessSampleRate("onair_dvb_554.000MHz_10Msps_20261009-071530.cs8") - 10e6) < 1 && std::abs(guessSampleRate("onair_dab_225.648MHz_2.048Msps_x.cf32") - 2.048e6) < 1, "the documented file names parse");
    CHECK(liveCs8 >= 1 && liveCf32 >= 1, "the live signal was received");

    // the float file played back as a file: the same receiver state as the live signal
    int replay = 0;
    {
        Engine e;
        e.setStandard(7);
        e.player().setMuted(true);
        e.fm().setSilent(true);
        DeviceInfo dev; dev.kind = DeviceInfo::File; dev.name = pCf32;
        FileOptions fo;
        fo.path = pCf32; fo.format = guessFormat(pCf32); fo.sampleRate = guessSampleRate(pCf32); fo.loop = false;
        CHECK(e.start(dev, fmTune(), fo), "the recording opens as a file");
        std::string err;
        CHECK(!e.startRecording((dir / "y.cs8").string(), FileFormat::CS8, err) && !err.empty(), "a file source refuses to record");
        printf("  refusal: %s\n", err.c_str());
        replay = watchFm(e, 4.0);
        e.stop();
    }
    printf("  replay of the float file: FM state %d (live %d)\n", replay, liveCf32);
    CHECK(replay == liveCf32, "the replay gives the same receiver state as the live signal");

    fs::remove_all(dir);
    printf(fails ? "iq record: FAILED\n" : "iq record: ok\n");
    return fails ? 1 : 0;
}
