// The channel scanner on ATSC 3.0 and DTMB: the built-in test signal stands in for the radio and sits on one of three channels (the other two are
// silent). The scan must find exactly that channel, lock it, and fill in the note and the mode.
#include "dect2/scanner.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>

using namespace dect2;

static int fails = 0;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s\n", m); fails++; } } while (0)

static void runCase(const char* name, ScanConfig cfg, int standard, double signalMHz) {
    cfg.identifyServices = false;
    cfg.lockTimeoutSec = 25;
    cfg.testTune = [=](double f, TuneSettings& t) {
        t.synth.mode = std::abs(f - signalMHz) < 0.01 ? standard : 99;   // 99: no such test signal, the source sends silence
        t.synth.snrDb = 30;
    };
    DeviceInfo dev; dev.kind = DeviceInfo::Synthetic; dev.name = "test signal";
    Scanner s;
    std::string err;
    CHECK(s.start(dev, cfg, err), "the scan starts");
    for (int i = 0; i < 1800 && (s.progress().running || i < 5); i++) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const ScanProgress pr = s.progress();
    const auto res = s.results();
    printf("  %s: %s\n", name, pr.phase.c_str());
    CHECK(!pr.running && pr.phase == "finished", "the scan finished");
    CHECK(res.size() == 3, "three channels were scanned");
    int found = 0;
    for (const auto& r : res) {
        printf("    %.1f MHz  occupied %d locked %d  %s | %s | %s  snr %.1f\n", r.freqMHz, (int)r.occupied, (int)r.t2, r.standard.c_str(), r.mode.c_str(), r.plpInfo.c_str(), r.snrDb);
        if (r.t2) {
            found++;
            CHECK(std::abs(r.freqMHz - signalMHz) < 0.01, "the locked channel is the one with the signal");
            CHECK(r.standard == name && r.note == name && !r.mode.empty() && !r.plpInfo.empty(), "standard, note, mode and PLP info are filled in");
        } else CHECK(std::abs(r.freqMHz - signalMHz) > 0.01 && r.note == "empty", "an empty channel is reported as empty");
    }
    CHECK(found == 1, "exactly one channel locked");
}

int main() {
    // bwMhz and autoBandwidth as a scan tab left on the DVB defaults hands them over: ATSC 3.0 is still measured as a 6 MHz channel
    ScanConfig a3; a3.atsc3 = true; a3.startMHz = 473; a3.stopMHz = 485; a3.stepMHz = 6; a3.bwMhz = 8; a3.autoBandwidth = true;
    runCase("ATSC 3.0", a3, 5, 479);
    ScanConfig dt; dt.dtmb = true; dt.startMHz = 474; dt.stopMHz = 490; dt.stepMHz = 8; dt.bwMhz = 8; dt.autoBandwidth = false;
    runCase("DTMB", dt, 9, 482);
    // a radio that cannot reach the channel's sample rate is refused
    DeviceInfo rtl; rtl.kind = DeviceInfo::Soapy; rtl.name = "RTL-SDR"; rtl.maxRateHz = 3.2e6;
    std::string err;
    CHECK(!Scanner::check(rtl, a3, err) && !Scanner::check(rtl, dt, err), "an RTL-SDR cannot scan ATSC 3.0 or DTMB");
    printf(fails ? "scan synth: FAILED\n" : "scan synth: ok\n");
    return fails ? 1 : 0;
}
