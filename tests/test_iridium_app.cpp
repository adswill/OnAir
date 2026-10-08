// Iridium as the interface runs it: the synthetic source with the settings applyBandwidth() gives the mode (10 Msps, 10.5 MHz,
// 9 MHz baseband filter, 1622 MHz, the test signal's defaults), and the interface's tick pushing the tuned frequency and the
// threshold to the receiver once it runs. A threshold pushed while samples flow used to restart the detector's sample count but
// not the ring's, so every later burst was read from the wrong place and almost nothing decoded.
#include "dect2/engine.h"
#include "dect2/iridium_frame.h"
#include "dect2/iridium_gen.h"
#include "dect2/iridium_rx.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <thread>
using namespace dect2;
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define SANITIZED 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
#define SANITIZED 1
#endif
#endif
#ifndef SANITIZED
#define SANITIZED 0
#endif
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

// the pager texts of the test signal (iridium_gen.cpp)
static bool knownText(const std::string& s) {
    static const char* const t[4] = {
        "MEET AT GATE 4 AT 1530 BRING PASSPORTS",
        "CALL OFFICE ASAP RE SHIPMENT 2231. CUSTOMS NEED THE INVOICE AND THE PACKING LIST BEFORE 1700 TODAY",
        "WX DXB 34C WIND 330/12 VIS 8KM NOSIG",
        "ETA PORT RASHID 0600 LT. CREW 14 ALL WELL. REQUEST FRESH WATER 40T AND PROVISIONS ON ARRIVAL. MASTER",
    };
    for (const char* x : t) if (s == x) return true;
    return false;
}

int main() {
    if (SANITIZED) { printf("iridium app: skipped in the sanitised build (10 Msps in real time)\n"); return 0; }
    Engine e;
    DeviceInfo dev;                    // the synthetic source
    TuneSettings t;                    // what applyBandwidth() sets for the mode
    const ModeTuning m = iridiumTuning();
    t.centerHz = m.defMhz * 1e6;
    t.bandwidthMhz = m.bandwidthMhz;
    t.sampleRate = m.sampleRate;
    t.basebandFilterHz = m.basebandHz;
    t.synth.mode = 21;                 // everything else as the app starts it (SNR 30 dB, 3 satellites, seed 0, traffic on)
    FileOptions fo;
    e.setStandard(21);
    CHECK(e.start(dev, t, fo), "engine start");
    const double signalSecs = 20;
    bool pushed = false;
    const auto t0 = std::chrono::steady_clock::now();
    for (;;) {
        const double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        if (el >= signalSecs) break;
        if (!pushed && el > 1.0) {     // the tick of app/iridium_ui.cpp, a frame or more after the start
            e.iridium().setCenterMhz(m.defMhz);
            e.iridium().setThresholdDb(13);
            pushed = true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    RxTelemetry rx;
    e.latestRx(rx, 0);
    const IridiumTelemetry& g = rx.iridium;
    const uint64_t voice = g.voiceFrames;
    const uint64_t nonVoice = g.uwOk > voice ? g.uwOk - voice : 0;
    const uint64_t nonVoiceOk = g.blocksOk > voice ? g.blocksOk - voice : 0;
    const uint64_t ira = g.typeCount[(int)IridiumType::IRA];
    int complete = 0, wrong = 0;
    for (const auto& pm : g.messages) { if (pm.complete) complete++; if (pm.complete && !knownText(pm.text)) wrong++; }
    size_t raPos = 0;
    for (const auto& ra : g.ringAlerts) if (ra.hasPos) raPos++;
    printf("app settings, %.1f s of signal: %llu bursts, %llu unique words, %llu dropped, %llu frames ok (%llu voice), %llu failed, %llu IRA, %zu satellites, "
           "%d complete pager messages, %llu dropped samples\n  %s\n",
           g.timeSec, (unsigned long long)g.bursts, (unsigned long long)g.uwOk, (unsigned long long)g.dropped, (unsigned long long)g.blocksOk,
           (unsigned long long)voice, (unsigned long long)g.blocksBad, (unsigned long long)ira, g.sats.size(), complete,
           (unsigned long long)e.droppedSamples(), iridiumSummary(g).c_str());
    CHECK(pushed && g.timeSec > signalSecs - 2, "signal time %.1f s", g.timeSec);
    CHECK(e.droppedSamples() == 0 && g.dropped == 0, "dropped %llu samples, %llu bursts", (unsigned long long)e.droppedSamples(), (unsigned long long)g.dropped);
    CHECK(g.uwOk >= 0.9 * (g.bursts - g.duplicates), "unique words in %llu of %llu bursts", (unsigned long long)g.uwOk, (unsigned long long)(g.bursts - g.duplicates));
    CHECK(nonVoiceOk >= 0.9 * nonVoice && nonVoice > 0, "non-voice frames: %llu of %llu ok", (unsigned long long)nonVoiceOk, (unsigned long long)nonVoice);
    // a ring alert per satellite per 90 ms frame
    CHECK(ira >= 0.85 * 3 * g.timeSec / 0.09, "%llu ring alerts in %.1f s", (unsigned long long)ira, g.timeSec);
    CHECK(g.sats.size() == 3 && raPos >= g.ringAlerts.size() * 9 / 10 && g.positions.size() > 100, "%zu satellites, %zu of %zu ring alerts with a position, %zu positions",
          g.sats.size(), raPos, g.ringAlerts.size(), g.positions.size());
    CHECK(complete >= 10 && wrong == 0, "%d complete pager messages, %d with the wrong text", complete, wrong);
    CHECK(g.hasTime && std::fabs(g.iridiumUtc - (kIridiumSynthEpoch + g.timeSec)) < 0.5, "Iridium time %.2f s after the epoch at %.2f s", g.iridiumUtc - kIridiumSynthEpoch, g.timeSec);
    e.stop();
    printf(fails ? "iridium app: %d FAILED\n" : "iridium app: all passed\n", fails);
    return fails ? 1 : 0;
}
