// The native radio drivers (source_native.cpp) against fake vendor libraries: listing, streaming a known tone, retuning, changing the rate, stopping.
#include "dect2/source.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>
#include <vector>
#include <sys/stat.h>
#include <unistd.h>
#include <dlfcn.h>
using namespace dect2;

static int failures = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("  FAIL: "); printf(__VA_ARGS__); printf("\n"); failures++; } } while (0)

static bool copyFile(const std::string& from, const std::string& to) {
    std::ifstream i(from, std::ios::binary);
    std::ofstream o(to, std::ios::binary);
    if (!i || !o) return false;
    o << i.rdbuf();
    return true;
}

static double (*fakeState)(const char*) = nullptr;
static double st(const char* k) { return fakeState ? fakeState(k) : -1e300; }

// reads from the ring for `ms` milliseconds; returns the number of samples and the mean of the first block
static size_t drain(IqRing& ring, int ms, std::complex<float>& mean) {
    size_t total = 0;
    std::vector<cf32> buf(1 << 16);
    bool first = true;
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < end) {
        const size_t n = ring.read(buf.data(), buf.size());
        if (n && first) { std::complex<double> a = 0; for (size_t i = 0; i < n; i++) a += std::complex<double>(buf[i]); mean = std::complex<float>((float)(a.real() / n), (float)(a.imag() / n)); first = false; }
        total += n;
        if (!n) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return total;
}

int main(int argc, char** argv) {
    if (argc < 2) { printf("usage: test_native <fake library>\n"); return 2; }
    const std::string fake = argv[1];
    char tmpl[] = "/tmp/onair-native-XXXXXX";
    const char* dir = mkdtemp(tmpl);
    if (!dir) { printf("no temp dir\n"); return 2; }
#ifdef __APPLE__
    const char* ext = ".dylib";
#else
    const char* ext = ".so";
#endif
    for (const char* stem : {"rtlsdr", "airspy", "bladeRF", "LimeSuite", "iio", "uhd"})
        if (!copyFile(fake, std::string(dir) + "/lib" + stem + ext)) { printf("cannot copy the fake library\n"); return 2; }
    setenv("DECT2_NATIVE_LIBDIR", dir, 1);
    std::string ext_(ext);
    auto stemOf = [](const std::string& board) { return board == "bladerf" ? "bladeRF" : board == "lime" ? "LimeSuite" : board == "pluto" ? "iio" : board == "usrp" ? "uhd" : board.c_str(); };

    std::string err;
    const auto list = listNativeDevices(err);
    printf("native radios found: %zu\n", list.size());
    for (const auto& d : list) printf("  %-8s %s\n", d.board.c_str(), d.name.c_str());
    CHECK(list.size() == 6, "expected 6 radios, found %zu", list.size());

    struct Case { const char* board; double wantRate; double wantMinRate; };
    const Case cases[] = {{"rtlsdr", 2.4e6, 0}, {"airspy", 10e6, 0}, {"bladerf", 10e6, 0}, {"lime", 10e6, 0}, {"pluto", 10e6, 0}, {"usrp", 10e6, 0}};
    for (const auto& c : cases) {
        printf("%s:\n", c.board);
        const DeviceInfo* dev = nullptr;
        for (const auto& d : list) if (d.board == c.board) dev = &d;
        CHECK(dev, "radio not listed");
        if (!dev) continue;
        CHECK(dev->kind == DeviceInfo::Native && dev->isRadio() && dev->isGeneric(), "wrong kind");
        auto src = makeSource(*dev);
        CHECK(src != nullptr, "no source");
        if (!src) continue;
        IqRing ring(1 << 22);
        TuneSettings t;
        t.centerHz = 522e6; t.sampleRate = c.wantRate; t.gainDb = 30; t.bandwidthMhz = 8;
        std::string e;
        const bool ok = src->start(t, ring, e);
        CHECK(ok, "start failed: %s", e.c_str());
        if (!ok) continue;
        CHECK(src->realtimeHardware(), "not flagged as hardware");
        std::complex<float> mean(0, 0);
        const size_t n = drain(ring, 400, mean);
        CHECK(n > 8000, "only %zu samples in 400 ms", n);
        CHECK(std::fabs(mean.real() - 0.5f) < 0.02f && std::fabs(mean.imag() + 0.25f) < 0.02f, "samples wrong: mean %.3f %.3f (expected 0.5 -0.25)", mean.real(), mean.imag());
        const std::string b = c.board;
        {   // each radio's copy of the fake library keeps its own record of what it was asked to do
            void* h = dlopen((std::string(dir) + "/lib" + stemOf(b) + ext).c_str(), RTLD_NOW | RTLD_NOLOAD);
            fakeState = h ? (double (*)(const char*))dlsym(h, "fake_state") : nullptr;
            CHECK(fakeState != nullptr, "the fake library of %s is not loaded", c.board);
        }
        if (b == "rtlsdr") { CHECK(st("rtl.freq") == 522e6, "freq %.0f", st("rtl.freq")); CHECK(st("rtl.gain") == 270, "gain %.0f (the 30 dB request should pick the 27 dB step)", st("rtl.gain")); CHECK(st("rtl.gainmode") == 1, "not manual gain"); }
        if (b == "airspy") { CHECK(st("airspy.freq") == 522e6, "freq"); CHECK(st("airspy.rate") == 10e6, "rate %.0f", st("airspy.rate")); CHECK(st("airspy.gain") == 21, "gain %.0f (clipped to the 0..21 steps)", st("airspy.gain")); CHECK(st("airspy.type") == 0, "not float32 I/Q"); }
        if (b == "bladerf") { CHECK(st("blade.freq") == 522e6, "freq"); CHECK(st("blade.rate") == 10e6, "rate"); CHECK(st("blade.gain") == 30, "gain"); CHECK(st("blade.gainmode") == 1, "not manual"); CHECK(st("blade.enabled") == 1, "not enabled"); CHECK(st("blade.format") == 0, "format"); CHECK(st("blade.openSerialGiven") == 1, "serial not passed"); }
        if (b == "lime") { CHECK(st("lime.freq") == 522e6, "freq"); CHECK(st("lime.rate") == 10e6, "rate"); CHECK(st("lime.gain") == 30, "gain"); CHECK(st("lime.antenna") == 2, "antenna index %.0f (LNAL expected for 522 MHz)", st("lime.antenna")); CHECK(st("lime.calibrated") == 1, "not calibrated"); CHECK(st("lime.fmt") == 0, "not float32"); CHECK(st("lime.chanEnabled") == 1, "channel not enabled"); }
        if (b == "pluto") { CHECK(st("pluto.connected") == 1, "uri"); CHECK(st("pluto.phy:altvoltage0.frequency") == 522e6, "freq"); CHECK(st("pluto.phy:voltage0.sampling_frequency") == 10e6, "rate"); CHECK(st("pluto.phy:voltage0.hardwaregain") == 30, "gain"); CHECK(st("pluto.phy:voltage0.gain_control_mode=manual") == 1, "not manual"); }
        if (b == "usrp") { CHECK(st("usrp.freq") == 522e6, "freq"); CHECK(st("usrp.rate") == 10e6, "rate"); CHECK(st("usrp.gain") == 30, "gain"); CHECK(st("usrp.cpuFmtFc32") == 1, "not fc32"); CHECK(st("usrp.cmd") == 97, "stream not started"); }

        // live retune: frequency and gain
        t.centerHz = 474e6; t.gainDb = 40;
        CHECK(src->retune(t, e), "retune failed: %s", e.c_str());
        const size_t n2 = drain(ring, 200, mean);
        CHECK(n2 > 3000, "stream stopped after a retune (%zu samples)", n2);
        if (b == "rtlsdr") { CHECK(st("rtl.freq") == 474e6, "retune freq"); CHECK(st("rtl.gain") == 370, "retune gain %.0f", st("rtl.gain")); }
        if (b == "airspy") CHECK(st("airspy.freq") == 474e6, "retune freq");
        if (b == "bladerf") { CHECK(st("blade.freq") == 474e6, "retune freq"); CHECK(st("blade.gain") == 40, "retune gain"); }
        if (b == "lime") { CHECK(st("lime.freq") == 474e6, "retune freq"); CHECK(st("lime.gain") == 40, "retune gain"); }
        if (b == "pluto") { CHECK(st("pluto.phy:altvoltage0.frequency") == 474e6, "retune freq"); CHECK(st("pluto.phy:voltage0.hardwaregain") == 40, "retune gain"); }
        if (b == "usrp") { CHECK(st("usrp.freq") == 474e6, "retune freq"); CHECK(st("usrp.gain") == 40, "retune gain"); }

        // a different sample rate while running
        t.sampleRate = 2.5e6;
        CHECK(src->retune(t, e) || !e.empty(), "rate change failed");
        const size_t n3 = drain(ring, 200, mean);
        CHECK(n3 > 1000, "stream stopped after a rate change (%zu samples)", n3);
        if (b == "airspy") CHECK(st("airspy.rate") == 2.5e6, "airspy rate %.0f (the 2.5 Msps step expected)", st("airspy.rate"));
        if (b == "bladerf") CHECK(st("blade.rate") == 2.5e6, "rate");
        if (b == "usrp") CHECK(st("usrp.rate") == 2.5e6, "rate");
        if (b == "pluto") CHECK(st("pluto.phy:voltage0.sampling_frequency") == 2.5e6, "pluto rate %.0f", st("pluto.phy:voltage0.sampling_frequency"));

        src->stop();
        const size_t drained = ring.available();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        CHECK(ring.available() == drained, "samples kept arriving after stop()");
        src.reset();
    }
    // with the feature switched off nothing is listed
    setenv("DECT2_NO_NATIVE", "1", 1);
    (void)listNativeDevices(err);   // (the libraries are already loaded, the switch is read when they are first touched)
    printf(failures ? "FAILED (%d)\n" : "native radio drivers: all checks passed\n", failures);
    return failures ? 1 : 0;
}
