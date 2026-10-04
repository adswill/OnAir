// Sound output: the backend opens (a silent device with correct timing when the computer has no sound), takes audio and plays it in real time.
#include "dect2/audioout.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>
#include <vector>

using namespace dect2;

int main() {
    AudioOut a;
    if (!a.start(48000)) { printf("FAIL: no sound backend could be started\n"); return 1; }
    a.setStartThreshold(2400);
    std::vector<float> buf(2 * 4800);
    double ph = 0;
    auto t0 = std::chrono::steady_clock::now();
    uint64_t sent = 0;
    // feed 1.5 s of a 440 Hz tone in real time, 100 ms at a time
    for (int i = 0; i < 15; i++) {
        for (int k = 0; k < 4800; k++) { const float v = 0.02f * (float)std::sin(ph); ph += 2 * M_PI * 440.0 / 48000; buf[2 * k] = buf[2 * k + 1] = v; }
        sent += (uint64_t)a.write(buf.data(), 4800);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    const uint64_t played = a.playedFrames();
    printf("sent %llu frames, played %llu in %.2f s, underruns %d\n", (unsigned long long)sent, (unsigned long long)played, sec, a.underruns());
    int fails = 0;
    if (sent < 4800 * 14) { printf("FAIL: the output did not take the audio\n"); fails++; }
    if (played < sent * 4 / 10) { printf("FAIL: the output played too little of it\n"); fails++; }
    if (played > sent) { printf("FAIL: more frames played than sent\n"); fails++; }
    a.stop();
    printf(fails ? "audio test FAILED\n" : "audio test passed\n");
    return fails ? 1 : 0;
}
