// Time machine (time_machine.h, Engine::setTimeMachine): the rolling IQ memory and the pre/post-trigger save. A numbered sample pattern goes through
// the ring and the saved file must hold exactly the samples from `pre` seconds before the trigger to `post` seconds after it, contiguous, with the
// SigMF metadata; the memory budget shortens the window; decimation averages; a retune (discard) forgets; a second trigger during a save is refused;
// and the engine's live path keeps receiving the built-in FM signal while the ring runs.
#include "dect2/engine/engine.h"
#include "dect2/source/time_machine.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <thread>

using namespace dect2;
namespace fs = std::filesystem;

static int fails = 0;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s\n", m); fails++; } } while (0)

static int pat(uint64_t k) { return (int)(k % 200) - 100; }
static int pat2(uint64_t k) { return (int)((k * 7) % 200) - 100; }

static void feed(TimeMachine& tm, uint64_t from, uint64_t n) {
    std::vector<cf32> b(4096);
    for (uint64_t i = 0; i < n;) {
        const size_t m = (size_t)std::min<uint64_t>(b.size(), n - i);
        for (size_t j = 0; j < m; j++) b[j] = cf32(pat(from + i + j) / 127.f, pat2(from + i + j) / 127.f);
        tm.push(b.data(), m);
        i += m;
    }
}

static bool waitDone(TimeMachine& tm, double secs) {
    secs = secs * 6 + 20;   // CI runs this beside a thousand other tests: a generous deadline, it returns as soon as the save is done
    const auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < secs) {
        if (!tm.stats().saving) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

static std::vector<int8_t> slurp(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::vector<int8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

int main() {
    const fs::path dir = fs::temp_directory_path() / "onair_time_machine_test";
    fs::remove_all(dir);
    fs::create_directories(dir);
    const double kRate = 1e6;

    // 1. pre and post around a trigger, the ring has wrapped twice
    {
        TimeMachine tm;
        CHECK(!tm.enabled(), "off at start");
        std::string err;
        CHECK(!tm.trigger((dir / "off.cs8").string(), 1, 1, 0, true, err) && !err.empty(), "a trigger while off is refused");
        const double win = tm.configure(2.0, kRate, 1, 512);
        CHECK(std::abs(win - 2.0) < 1e-6, "window = what was asked");
        const uint64_t N = 4700000;   // 2.35 windows
        feed(tm, 0, N);
        const TimeMachineStats s0 = tm.stats();
        CHECK(s0.enabled && std::abs(s0.filledSeconds - 2.0) < 1e-6 && s0.bytes == 4000000, "ring full, 4 MB");
        const std::string path = (dir / "a.cs8").string();
        CHECK(tm.trigger(path, 1.0, 0.5, 93.9e6, true, err), "trigger accepted");
        CHECK(!tm.trigger((dir / "b.cs8").string(), 1, 0.1, 0, true, err), "a second trigger during a save is refused");
        feed(tm, N, 500000);   // the "after" part
        CHECK(waitDone(tm, 10), "save finished");
        const TimeMachineStats s1 = tm.stats();
        CHECK(s1.error.empty() && s1.saves == 1 && s1.lastMissing == 0, "no error, nothing missing");
        const auto d = slurp(path);
        CHECK(d.size() == 2 * 1500000, "file = 1.5 s x 1 Msps x 2 bytes");
        bool ok = d.size() == 3000000;
        for (size_t j = 0; ok && j < 1500000; j++) {
            const uint64_t k = N - 1000000 + j;
            if (d[2 * j] != pat(k) || d[2 * j + 1] != pat2(k)) { printf("  mismatch at %zu\n", j); ok = false; }
        }
        CHECK(ok, "the file holds exactly the samples from 1 s before to 0.5 s after the trigger, no gaps");
        CHECK(fs::exists(dir / "a.sigmf-meta") && fs::exists(dir / "a.sigmf-data"), "SigMF metadata and data link written");
        if (fs::exists(dir / "a.sigmf-meta")) {
            const auto m = slurp(dir / "a.sigmf-meta");
            const std::string txt(m.begin(), m.end());
            CHECK(txt.find("ci8_le") != std::string::npos && txt.find("\"core:sample_rate\": 1000000") != std::string::npos || txt.find("1000000") != std::string::npos, "metadata has the type and rate");
        }
        // asking for more than the ring has: only what exists
        const std::string p2 = (dir / "c.cs8").string();
        CHECK(tm.trigger(p2, 100.0, 0.0, 0, false, err), "pre longer than the ring is clipped");
        CHECK(waitDone(tm, 10) && [&]{ std::error_code ec; return fs::file_size(p2, ec) == 4000000 && !ec; }(), "clipped to the 2 s the ring holds");
        // a retune forgets the history
        tm.discard();
        CHECK(tm.stats().filledSeconds < 1e-6, "discard empties the history");
        CHECK(!tm.trigger((dir / "d.cs8").string(), 1, 0, 0, false, err), "nothing to save after a discard");
        tm.off();
        CHECK(!tm.enabled() && tm.stats().bytes == 0, "off frees the ring");
    }
    // 2. the memory budget
    {
        TimeMachine tm;
        const double win = tm.configure(100.0, 10e6, 1, 16);   // 16 MB at 10 Msps x 2 B = 0.8 s
        CHECK(std::abs(win - 16.0 * 1048576 / 2 / 10e6) < 1e-6 && tm.stats().bytes <= 16u << 20, "the memory budget shortens the window");
        const double w2 = tm.configure(100.0, 10e6, 8, 16);   // decimated by 8: 6.7 s
        CHECK(std::abs(w2 - 8 * win) < 1e-6 && std::abs(tm.storedRate() - 1.25e6) < 1, "decimation stretches the window");
    }
    // 3. decimation averages
    {
        TimeMachine tm;
        tm.configure(1.0, 1e6, 4, 64);
        std::vector<cf32> b(1000);
        for (size_t i = 0; i < b.size(); i++) b[i] = cf32(i % 4 == 0 ? 0.4f : 0.2f, -0.1f);   // mean of four: 0.25, -0.1
        for (int k = 0; k < 10; k++) tm.push(b.data(), b.size());
        std::string err;
        const std::string path = (dir / "dec.cs8").string();
        CHECK(tm.trigger(path, 1.0, 0.0, 0, false, err) && waitDone(tm, 5), "decimated save");
        const auto d = slurp(path);
        CHECK(d.size() == 2 * 2500 && d[0] == 32 && d[1] == -13, "boxcar mean of four samples (0.25 -> 32, -0.1 -> -13)");
    }
    // 4. the engine: the live FM signal keeps being received while the ring runs, and a save works
    {
        Engine e;
        e.setStandard(7);
        e.player().setMuted(true);
        e.fm().setSilent(true);
        e.setTimeMachine(3.0, 1, 64);
        DeviceInfo dev; dev.kind = DeviceInfo::Synthetic; dev.name = "test signal";
        TuneSettings t;
        t.sampleRate = kRate; t.centerHz = 93.9e6; t.bandwidthMhz = 0.2; t.synth.mode = 7; t.synth.snrDb = 30;
        CHECK(e.start(dev, t, FileOptions()), "engine started");
        int best = 0;
        uint64_t seq = 0;
        const auto t0 = std::chrono::steady_clock::now();
        std::string path = (dir / e.timeMachineName()).string(), err;
        bool trig = false;
        // run at least 4 s, and (on a starved machine) until the save has been triggered and finished, up to 60 s
        auto el = [&] { return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(); };
        while (el() < 4.0 || (el() < 60.0 && (!trig || e.timeMachineStats().saving || e.timeMachineStats().saves == 0))) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            RxTelemetry rt;
            if (e.latestRx(rt, seq)) { seq = rt.seq; if (rt.standard == 6) best = std::max(best, rt.fm.state); }
            if (!trig && std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() > 1.5) { trig = e.timeMachineSaveTo(path, 1.0, 0.5, err); CHECK(trig, "engine trigger accepted"); }
        }
        const TimeMachineStats s = e.timeMachineStats();
        CHECK(best >= 1, "the live signal was received with the ring running");
        CHECK(!s.saving && s.saves == 1 && s.error.empty(), "the engine's save finished");
        std::error_code fec;
        const double bytes = fs::exists(path, fec) ? (double)fs::file_size(path, fec) : 0;
        printf("  engine save: %s  %.0f bytes (%.2f s), FM state %d\n", fs::path(path).filename().string().c_str(), bytes, bytes / 2 / kRate, best);
        CHECK(std::abs(bytes / 2 / kRate - 1.5) < 0.15, "about 1.5 s saved (pre 1 s + post 0.5 s)");
        CHECK(path.find("rewind-fm") != std::string::npos && std::abs(guessSampleRate(path) - kRate) < 1, "the name has the mode and the rate");
        e.stop();
        CHECK(!e.timeMachineStats().enabled, "stopped with the radio");
    }
    fs::remove_all(dir);
    printf(fails ? "time machine: FAILED\n" : "time machine: ok\n");
    return fails ? 1 : 0;
}
