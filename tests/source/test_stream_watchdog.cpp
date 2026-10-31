// The stall watchdog of a USB sample stream (stream_watchdog.h), with a made-up clock: the ladder clear / clear / reset / reopen, stray errors
// ignored, the ladder reset after the stream has been healthy again, and the counters the verbose log prints.
#include "dect2/source/stream_watchdog.h"
#include "dect2/common/verbose_log.h"
#include <cstdio>
#include <string>
#include <vector>
using namespace dect2;
#include "../common/test_util.h"

using A = StreamWatchdog::Action;

int main() {
    // a healthy stream never triggers anything
    {
        StreamWatchdog w; w.start(0);
        for (int i = 1; i <= 100; i++) { w.onData(i * 0.05, 65536); CHECK(w.poll(i * 0.05) == A::None, "healthy poll"); }
        CHECK(w.stats().transfers == 100 && w.stats().bytes == 100 * 65536ull && w.stats().stalls == 0, "counters");
    }
    // silence: nothing before 2 s, then the ladder, one rung per 2 s of silence
    {
        StreamWatchdog w; w.start(0);
        CHECK(w.poll(1.9) == A::None, "1.9 s is not a stall yet");
        CHECK(w.poll(2.0) == A::ClearHaltRestart && w.rung() == 1, "first stall: clear the halt");
        CHECK(w.poll(3.0) == A::None, "the 2 s count starts again after a restart");
        CHECK(w.poll(4.0) == A::ClearHaltRestart, "second stall: clear again");
        CHECK(w.poll(6.0) == A::ResetRestart, "third stall: reset the device");
        CHECK(w.poll(8.0) == A::Reopen, "fourth: give up, the radio is reopened");
        CHECK(w.poll(10.0) == A::ClearHaltRestart, "the ladder starts over after a reopen");
        CHECK(w.stats().stalls == 5 && w.stats().reopens == 1 && w.stats().resets == 1 && w.stats().restarts == 4, "counters after the ladder");
    }
    // broken pipe: one or two stray errors are ignored, a burst restarts the stream; an unplugged device (-4) reopens at once
    {
        StreamWatchdog w; w.start(0);
        CHECK(w.onError(1, -9) == A::None && w.onError(1.01, -9) == A::None, "stray errors ignored");
        w.onData(1.02, 65536);
        CHECK(w.onError(1.03, -9) == A::None && w.onError(1.04, -9) == A::None, "a good transfer resets the burst");
        CHECK(w.onError(1.05, -9) == A::ClearHaltRestart, "three in a row: clear the halt and restart");
        CHECK(w.onError(2, -4) == A::Reopen, "no device: reopen");
    }
    // healthy for 3 s after a restart: the ladder is at the bottom again, so the next trouble is a first rung, not a reset
    {
        StreamWatchdog w; w.start(0);
        CHECK(w.poll(2.0) == A::ClearHaltRestart && w.poll(4.0) == A::ClearHaltRestart, "two rungs");
        for (int i = 1; i <= 80; i++) w.onData(4 + i * 0.05, 65536);
        CHECK(w.rung() == 0, "ladder back to the start after healthy data (rung %d)", w.rung());
        CHECK(w.poll(10.1) == A::ClearHaltRestart, "the next stall is a first rung");
    }
    // the verbose log is off by default and prints only when switched on
    {
        CHECK(!verboseLogging(), "verbose logging must be off by default");
        setVerboseLogging(true);
        CHECK(verboseLogging(), "verbose on");
        std::vector<std::string> got;
        setDiagSink([&](const std::string& l) { got.push_back(l); });
        verboseLog("usb: %d MB/s", 5);
        diagLog("radio lost");
        setVerboseLogging(false);
        verboseLog("must not appear");
        setDiagSink(nullptr);
        CHECK(!verboseLogging(), "verbose off again");
        CHECK(got.size() == 2 && got[0] == "[v] usb: 5 MB/s" && got[1] == "radio lost", "sink got %zu lines", got.size());
    }
    return testSummary("stream watchdog");
}
