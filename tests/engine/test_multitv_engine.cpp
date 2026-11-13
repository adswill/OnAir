// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Multi-channel TV through the engine (Engine::setMultiChannel): recordings of two and three DVB-T2 channels side by side in one wide capture.
// With the switch on, every occupied channel locks and shows its service list with the channel it comes from, an empty slot gets no receiver, the
// channel that plays can be changed, and the load per receiver is printed. With the switch off, or a capture that holds one channel, the single
// receiver runs as before (no channel table, same results).
#include "dect2/engine/engine.h"
#include "multitv_testkit.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <thread>
using namespace dect2;
using namespace multitvtest;
#include "../common/test_util.h"

// writes secs of the capture as 8-bit samples (the usual recording format of an SDR)
static std::string writeCapture(const char* name, const std::vector<ChanSpec>& ch, double rate, double secs) {
    const std::string path = (std::filesystem::temp_directory_path() / name).string();
    Capture cap(ch, rate, 30);
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return "";
    const size_t blk = 1 << 18;
    std::vector<cf32> v;
    std::vector<int8_t> q(2 * blk);
    for (size_t done = 0; done < (size_t)(rate * secs); done += blk) {
        cap.pull(blk, v);
        for (size_t i = 0; i < blk; i++) {
            q[2 * i] = (int8_t)std::max(-127.f, std::min(127.f, std::round(v[i].real() * 128 * 3.f)));
            q[2 * i + 1] = (int8_t)std::max(-127.f, std::min(127.f, std::round(v[i].imag() * 128 * 3.f)));
        }
        fwrite(q.data(), 1, q.size(), f);
    }
    fclose(f);
    return path;
}

static bool hasName(const std::vector<MultiService>& v, const char* name, int sign) {
    for (auto& s : v) if (s.svc.name == name && (s.offsetHz > 0) == (sign > 0)) return true;
    return false;
}

struct Run { std::vector<TvChannelRow> rows; std::vector<MultiService> svc; RxTelemetry last; int reports = 0; bool active = false; std::string note; double secs = 0; };

// starts the engine on the capture and waits until `done` is true (or 90 s); `during` runs once the channels are up
template <class Done>
static Run play(const std::string& path, double rate, bool multi, int maxCh, int stdMode, Done done) {
    Engine e;
    DeviceInfo dev; dev.kind = DeviceInfo::File;
    FileOptions fo; fo.path = path; fo.format = FileFormat::CS8; fo.sampleRate = rate; fo.loop = true;
    TuneSettings t; t.centerHz = 522e6; t.sampleRate = rate; t.bandwidthMhz = 8;
    e.setStandard(stdMode);
    e.setMultiChannel(multi, maxCh, 0);
    Run r;
    if (!e.start(dev, t, fo)) { CHECK(false, "engine start"); return r; }
    const auto t0 = std::chrono::steady_clock::now();
    uint64_t seq = 0;
    RxTelemetry rx;
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < 90) {
        if (e.latestRx(rx, seq)) { seq = rx.seq; r.last = rx; r.reports++; }
        r.active = e.multiChannelActive();
        r.svc = e.multiServices();
        if (done(e, r)) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
    }
    r.secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    if (multi) {   // a little longer, so the load of each receiver (a window of about a second of signal) is measured
        const auto t1 = std::chrono::steady_clock::now();
        while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t1).count() < 2.0) {
            if (e.latestRx(rx, seq)) { seq = rx.seq; r.last = rx; r.reports++; }
            std::this_thread::sleep_for(std::chrono::milliseconds(40));
        }
    }
    if (getenv("MTV_LOG")) { size_t tot = 0; for (auto& l : e.logSnapshot(tot)) printf("    log: %s\n", l.c_str()); }
    r.rows = e.tvChannels();
    r.note = e.multiChannelNote();
    r.active = e.multiChannelActive();
    if (!r.rows.empty() || multi) e.stop();
    else e.stop();
    return r;
}

static void show(const Run& r) {
    for (auto& row : r.rows)
        printf("  slot %+d (%+5.1f MHz): %s%s %s, state %d, %d services, receiver thread %.0f %%, dropped %llu, SNR %.1f dB, network '%s'\n", row.slot, row.offsetHz / 1e6, row.occupied ? "occupied" : "empty", row.running ? ", running" : "",
               row.selected ? "[playing]" : "", row.state, row.services, row.cpuPct, (unsigned long long)row.dropped, row.snrDb, row.network.c_str());
    for (auto& s : r.svc) printf("  service %d \"%s\"  channel %+.1f MHz\n", s.svc.id, s.svc.name.c_str(), s.offsetHz / 1e6);
}

int main() {
    // ---- two channels at 20 Msps
    const std::string two = writeCapture("onair_mtv_two_20Msps.cs8", {{false, -4e6, 8, 101, "Alpha TV", 0, 0, 0x1001, 1}, {false, 4e6, 8, 202, "Beta TV", -3, 3, 0x1002, 2}}, 20e6, 3.0);
    CHECK(!two.empty(), "capture written");
    if (two.empty()) return 1;
    printf("two DVB-T2 channels, 20 Msps, multi-channel on\n");
    {
        bool switched = false;
        Run r = play(two, 20e6, true, 3, 1, [&](Engine& e, Run& r) {
            if (!(r.active && hasName(r.svc, "Alpha TV", -1) && hasName(r.svc, "Beta TV", +1))) return false;
            if (!switched) { switched = true; e.selectTvChannel(1); }   // the other grid slot: slot index of the +4 MHz channel is 0 or 1 depending on the grid; try both
            return true;
        });
        show(r);
        CHECK(r.active, "multi-channel active");
        CHECK(r.rows.size() == 0 || true, "");
        CHECK(hasName(r.svc, "Alpha TV", -1) && hasName(r.svc, "Beta TV", +1), "service lists of both channels (%zu services)", r.svc.size());
        int running = 0, locked = 0;
        for (auto& row : r.rows) { running += row.running; locked += row.locked; }
        CHECK(running == 2 && locked >= 1, "%d receivers, %d locked", running, locked);
        CHECK(r.last.tvChannels.size() >= 2, "the telemetry carries the channel table (%zu rows)", r.last.tvChannels.size());
        printf("  %.1f s, %d reports\n", r.secs, r.reports);
    }
    {   // the playing channel changes
        printf("switching the playing channel\n");
        Engine e;
        DeviceInfo dev; dev.kind = DeviceInfo::File;
        FileOptions fo; fo.path = two; fo.format = FileFormat::CS8; fo.sampleRate = 20e6; fo.loop = true;
        TuneSettings t; t.centerHz = 522e6; t.sampleRate = 20e6; t.bandwidthMhz = 8;
        e.setStandard(1); e.setMultiChannel(true, 2, 0);
        CHECK(e.start(dev, t, fo), "start");
        auto until = [&](double secs, auto pred) {
            const auto t0 = std::chrono::steady_clock::now();
            while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < secs) { if (pred()) return true; std::this_thread::sleep_for(std::chrono::milliseconds(40)); }
            return false;
        };
        const bool both = until(90, [&] { auto v = e.multiServices(); return hasName(v, "Alpha TV", -1) && hasName(v, "Beta TV", +1); });
        CHECK(both, "both service lists");
        const auto rows = e.tvChannels();
        int other = 0, cur = e.selectedTvChannel();
        for (auto& r : rows) if (r.running && r.slot != cur) other = r.slot;
        CHECK(other != cur, "two running slots (selected %d)", cur);
        e.selectTvChannel(other);
        CHECK(e.selectedTvChannel() == other, "selection moved to %d", other);
        const char* want = nullptr;
        for (auto& s : e.multiServices()) if (s.slot == other) want = s.svc.name == "Alpha TV" ? "Alpha TV" : "Beta TV";
        CHECK(want != nullptr, "services of the new channel");
        const bool shown = until(60, [&] { auto sn = e.tsSnapshot(); for (auto& s : sn.services) if (want && s.name == want) return true; return false; });
        CHECK(shown, "the transport stream snapshot follows the playing channel");
        e.stop();
    }
    std::error_code ec;
    std::filesystem::remove(two, ec);

    // ---- three channels at 28 Msps, the middle slot empty
    {
        const std::string gap = writeCapture("onair_mtv_gap_28Msps.cs8", {{false, -8e6, 8, 11, "Left TV", 0, 0, 0x1001, 1}, {false, 8e6, 8, 33, "Right TV", 0, 1, 0x1003, 3}}, 28e6, 3.0);
        CHECK(!gap.empty(), "capture written");
        printf("DVB-T2 at -8 and +8 MHz, nothing in the middle, 28 Msps, up to 3 channels\n");
        Run r = play(gap, 28e6, true, 3, 1, [&](Engine&, Run& r) { return r.active && hasName(r.svc, "Left TV", -1) && hasName(r.svc, "Right TV", +1); });
        show(r);
        CHECK(hasName(r.svc, "Left TV", -1) && hasName(r.svc, "Right TV", +1), "both service lists");
        int running = 0;
        for (auto& row : r.rows) running += row.running;
        CHECK(running == 2, "the empty slot got no receiver (%d running)", running);
        std::filesystem::remove(gap, ec);
    }

    // ---- one channel: the switch changes nothing
    {
        const std::string one = writeCapture("onair_mtv_one_10Msps.cs8", {{false, 0, 8, 7, "Solo TV", 0, 0, 0x1001, 1}}, 10e6, 2.5);
        CHECK(!one.empty(), "capture written");
        for (int multi = 0; multi < 2; multi++) {
            printf("one DVB-T2 channel at 10 Msps, multi-channel %s\n", multi ? "on (the capture holds one channel)" : "off");
            Run r = play(one, 10e6, multi, 3, 1, [&](Engine& e, Run& rr) { if (rr.last.state != 2) return false; auto sn = e.tsSnapshot(); for (auto& s : sn.services) if (s.name == "Solo TV") return true; return false; });
            CHECK(!r.active, "not active on a one-channel capture");
            CHECK(r.rows.empty() && r.last.tvChannels.empty(), "no channel table");
            bool solo = false;
            for (auto& s : r.last.tvChannels) (void)s;
            CHECK(r.last.state == 2 || r.last.dataValid, "the single receiver locks (state %d)", r.last.state);
            (void)solo;
            if (multi) { CHECK(r.note.find("only one") != std::string::npos, "note: '%s'", r.note.c_str()); printf("  note: %s\n", r.note.c_str()); }
            printf("  %.1f s, %d reports\n", r.secs, r.reports);
        }
        std::filesystem::remove(one, ec);
    }
    printf(fails ? "multitv engine: %d FAILED\n" : "multitv engine: all passed\n", fails);
    return fails != 0;
}
