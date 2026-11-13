// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// #439: a video file sent as analog TV played as fast as the decoder ran, not at its own speed. It now follows the signal's clock. The demo programme is used
// as the file: its picture shows its frame number (mod 25) as a bar at the bottom, so the frame on show can be read back at 50 and 59.94 fields a second, with
// a generator running far faster than real time, through the file's loop, and while the clock stands still.
#include "dect2/audio/videoin.h"
#include "dect2/tv/demo_ts.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <thread>
#include <vector>
#include "../common/test_util.h"
using namespace dect2;

static constexpr int kW = 640, kH = 360, kFps = 25;

static int frameOnShow(VideoIn& v) {
    std::vector<float> y((size_t)kW * kH);
    if (!v.copyYuv(kW, kH, y.data(), nullptr, nullptr)) return -1;
    int bar = 0;
    for (int x = 20; x < kW - 20; x++) if (y[(size_t)(kH - 17) * kW + x] > 0.55f) bar++;
    return (int)std::lround(bar * (double)kFps / (kW - 40)) % kFps;
}
static int modDiff(int a, int b) { int d = std::abs(a - b) % kFps; return std::min(d, kFps - d); }

int main() {
    const char* tmp = getenv("TMPDIR");
    const std::string path = std::string(tmp ? tmp : "/tmp") + "/onair_pace_test.ts";
    {
        auto src = demoTsSource(3e6);
        std::ofstream f(path, std::ios::binary);
        uint8_t p[188];
        for (long i = 0; i < (long)(3e6 / 1504 * 19.5); i++) { src(p); f.write((const char*)p, 188); }
    }
    for (double fieldHz : {59.94, 50.0}) {
        VideoIn v;
        std::string err;
        CHECK(v.start(VideoIn::File, path, err), "open: %s", err.c_str());
        int checked = 0, wrong = 0, worst = 0;
        for (double t = 0.2; t < 5.0; t += 1.0 / fieldHz) {
            v.setClock(t, 2000);
            const int got = frameOnShow(v), want = (int)std::floor(t * kFps + 1e-6) % kFps;
            if (got < 0) { wrong++; continue; }
            const int d = modDiff(got, want);
            worst = std::max(worst, d); checked++;
            if (d > 2) wrong++;
        }
        printf("%.2f fields/s: %d fields checked, %d off by more than 2 frames, worst %d\n", fieldHz, checked, wrong, worst);
        CHECK(checked > 100 && wrong == 0, "%d wrong of %d at %.2f fields/s", wrong, checked, fieldHz);
        v.setClock(5.0, 2000);                                       // a clock that stands still holds the picture (a decoder left free ran on)
        const int a = frameOnShow(v);
        std::this_thread::sleep_for(std::chrono::milliseconds(800));
        CHECK(modDiff(a, frameOnShow(v)) <= 1, "the picture ran on with the clock stopped");
        v.setClock(21.0, 4000);                                      // past the end of the file (19.5 s): the frames restart, time goes on
        const int f1 = frameOnShow(v);
        v.setClock(21.4, 4000);
        const int f2 = frameOnShow(v);
        printf("after the loop: frames %d, then %d 0.4 s later\n", f1, f2);
        CHECK(f1 >= 0 && f2 >= 0 && modDiff((f1 + 10) % kFps, f2) <= 2, "after the loop: frames %d and %d", f1, f2);
        v.stop();
    }
    std::remove(path.c_str());
    return testSummary("video_file_pace");
}
