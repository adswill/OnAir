// Record and replay for any mode (iq_record.h, iq_file.h): a synthetic signal is recorded as 16-bit SigMF in a sweep of modes; each recording's
// metadata gives back the centre frequency, the rate, the format and the mode, the samples read back exactly (to 16 bits), and the engine
// replays a recorded FM signal from the file with no radio attached.
#include "dect2/engine/engine.h"
#include "dect2/engine/mode_tuning.h"
#include "dect2/source/iq_file.h"
#include "dect2/source/iq_record.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <thread>

using namespace dect2;
namespace fs = std::filesystem;

static int fails = 0;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s\n", m); fails++; } } while (0)

static std::vector<cf32> tone(size_t n, double rate) {
    std::vector<cf32> v(n);
    for (size_t i = 0; i < n; i++) { const double p = 2 * M_PI * 123456.0 * i / rate; v[i] = cf32((float)(0.7 * std::cos(p)), (float)(0.7 * std::sin(p))); }
    return v;
}

// records n samples of the tone as 16-bit with SigMF; returns the recording's path
static std::string recordTone(const fs::path& dir, const std::string& id, double centre, double rate, FileFormat fmt, size_t n, std::vector<cf32>& sent) {
    sent = tone(n, rate);
    const std::string path = (dir / recordingFileName(id, centre, rate, fmt)).string();
    IqRecorder r;
    r.setSigmf(true, centre, id);
    std::string err;
    if (!r.start(path, fmt, rate, err)) { printf("  start failed: %s\n", err.c_str()); return ""; }
    for (size_t o = 0; o < n; o += 4096) r.push(sent.data() + o, std::min<size_t>(4096, n - o));
    r.stop();
    return path;
}

static int watchFm(Engine& e, double secs) {
    int best = 0; uint64_t seq = 0;
    const auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < secs) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        RxTelemetry t;
        if (e.latestRx(t, seq)) { seq = t.seq; if (t.standard == 6) best = std::max(best, t.fm.state); }
    }
    return best;
}

int main() {
    const fs::path dir = fs::temp_directory_path() / "onair_replay_test";
    fs::remove_all(dir); fs::create_directories(dir);

    // the mode ids of the recording names read back as the mode, for the older modes and every registered one
    CHECK(recordingStdMode("fm") == 7 && recordingStdMode("atsc") == 3 && recordingStdMode("dab") == 4 && recordingStdMode("isdbt") == 6, "older mode ids map to their standard");
    CHECK(recordingStdMode("dvb") == -1 && recordingStdMode("iq") == -1 && recordingStdMode("") == -1 && recordingStdMode("nonsense") == -1, "dvb, iq and unknown ids restore no mode");
    int swept = 0;
    for (int m = 8; m < 400; m++) {
        const ModeTuning* mt = modeTuning(m);
        if (!mt || !*mt->id) continue;
        const int back = recordingStdMode(recordingModeId(m));
        if (back < 0) { printf("FAIL: mode %d id %s does not read back\n", m, mt->id); fails++; continue; }
        const ModeTuning* bt = modeTuning(back);
        CHECK(bt && std::string(bt->id) == mt->id, "an id reads back as a mode with the same id");
        swept++;
    }
    CHECK(swept >= 40, "at least 40 modes are registered");
    printf("  %d modes: id -> standard round trip ok\n", swept);

    // record 16-bit + SigMF for a sweep of modes, centres and rates; read everything back from the files only
    struct Case { std::string id; double centre, rate; } cases[] = {
        {"fm", 93.9e6, 1e6}, {"atsc", 563e6, 8e6}, {"dab", 225.648e6, 2.048e6}, {"adsb", 1090e6, 2e6}, {"ais", 162e6, 0.5e6}, {"dmr", 438.8e6, 0.25e6}};
    for (const Case& c : cases) {
        if (c.id != "fm" && c.id != "atsc" && c.id != "dab" && !modeTuningById(c.id)) { printf("  (mode %s not in this build, skipped)\n", c.id.c_str()); continue; }
        std::vector<cf32> sent;
        const size_t n = 50000;
        const std::string p = recordTone(dir, c.id, c.centre, c.rate, FileFormat::CI16, n, sent);
        CHECK(!p.empty(), "the recording starts");
        if (p.empty()) continue;
        const fs::path meta = fs::path(p).replace_extension(".sigmf-meta");
        CHECK(fs::exists(meta), "the SigMF meta file is written");
        CHECK(fs::file_size(p) == n * 4, "16-bit complex = 4 bytes per sample");
        IqFileInfo info; std::string err;
        CHECK(probeIqFile(meta.string(), info, err), "the meta file opens with no radio");
        CHECK(info.format == FileFormat::CI16, "the format is read back (16-bit)");
        CHECK(std::fabs(info.sampleRate - c.rate) < 1 && std::fabs(info.centerHz - c.centre) < 1, "the rate and the centre frequency are read back");
        CHECK(info.description == c.id && recordingStdMode(info.description) == recordingStdMode(c.id), "the mode id is read back");
        CHECK(guessFormat(p) == FileFormat::CI16, "the .cs16 name gives 16-bit");
        // the samples
        std::ifstream f(fs::path(info.dataPath), std::ios::binary);
        std::vector<int16_t> raw(n * 2);
        f.read((char*)raw.data(), raw.size() * 2);
        double worst = 0;
        for (size_t i = 0; i < n; i++) worst = std::max({worst, (double)std::fabs(raw[2 * i] / 32767.0 - sent[i].real()), (double)std::fabs(raw[2 * i + 1] / 32767.0 - sent[i].imag())});
        CHECK(worst < 1.0 / 32767, "the 16-bit samples are the signal to within one step");
        printf("  %-5s %8.3f MHz %6.3f Msps: ok (max error %.1e)\n", c.id.c_str(), c.centre / 1e6, c.rate / 1e6, worst);
    }
    // 8-bit and float recordings carry the same metadata
    for (FileFormat fmt : {FileFormat::CS8, FileFormat::CF32}) {
        std::vector<cf32> sent;
        const std::string p = recordTone(dir, "fm", 100.1e6, 1e6, fmt, 10000, sent);
        IqFileInfo info; std::string err;
        CHECK(!p.empty() && probeIqFile(fs::path(p).replace_extension(".sigmf-meta").string(), info, err) && info.format == fmt && info.description == "fm", "8-bit and float SigMF recordings read back too");
    }

    // an FM signal recorded from the built-in test signal as 16-bit SigMF, replayed from the file with the metadata only
    std::string path; int live = 0;
    {
        Engine e; e.setStandard(7); e.player().setMuted(true); e.fm().setSilent(true);
        DeviceInfo dev; dev.kind = DeviceInfo::Synthetic; dev.name = "test signal";
        TuneSettings t; t.sampleRate = 1e6; t.centerHz = 93.9e6; t.bandwidthMhz = 0.2; t.synth.mode = 7; t.synth.snrDb = 30;
        e.setRecordSigmf(true);
        CHECK(e.start(dev, t, FileOptions()), "engine started");
        path = (dir / e.recordingName(FileFormat::CI16)).string();
        std::string err;
        CHECK(e.startRecording(path, FileFormat::CI16, err), "engine records 16-bit");
        live = watchFm(e, 3.0);
        e.stopRecording(); e.stop();
    }
    int replay = 0;
    {
        IqFileInfo info; std::string err;
        CHECK(probeIqFile(fs::path(path).replace_extension(".sigmf-meta").string(), info, err), "the engine's recording opens");
        CHECK(recordingStdMode(info.description) == 7 && info.format == FileFormat::CI16 && std::fabs(info.sampleRate - 1e6) < 1, "mode, format and rate come from the metadata");
        Engine e; e.setStandard(recordingStdMode(info.description)); e.player().setMuted(true); e.fm().setSilent(true);
        DeviceInfo dev; dev.kind = DeviceInfo::File; dev.name = info.dataPath;
        FileOptions fo; fo.path = info.dataPath; fo.format = info.format; fo.sampleRate = info.sampleRate; fo.offsetBytes = info.offsetBytes; fo.endBytes = info.endBytes; fo.loop = false;
        TuneSettings t; t.sampleRate = info.sampleRate; t.centerHz = 93.9e6; t.bandwidthMhz = 0.2;
        CHECK(e.start(dev, t, fo), "the recording replays with no radio");
        replay = watchFm(e, 4.0);
        e.stop();
    }
    printf("  engine replay of the 16-bit SigMF: FM state %d (live %d)\n", replay, live);
    CHECK(live >= 1 && replay == live, "the replay gives the same receiver state as the live signal");

    fs::remove_all(dir);
    printf(fails ? "replay: FAILED\n" : "replay: ok\n");
    return fails ? 1 : 0;
}
