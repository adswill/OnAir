// The native radio drivers (source_native.cpp) against fake vendor libraries: listing, streaming a known tone, retuning, changing the rate, stopping.
// (The SDRplay fake checks the driver's calls and settings; the struct layouts themselves are checked in sdrplay_api_min.h.)
#include "dect2/source.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <set>
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

// ---- native-robust: library lookup, open retries, reconnect after an unplug, USB error texts and the Linux USB hints
#include "../core/src/native_common.h"
#include "dect2/usb_diag.h"
#include <fcntl.h>
#include <functional>

// runs f with standard error going to a file, and returns what was written there
static std::string captureStderr(const std::string& file, const std::function<void()>& f) {
    fflush(stderr);
    const int saved = dup(2);
    const int fd = open(file.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (saved < 0 || fd < 0) { f(); return {}; }
    dup2(fd, 2);
    close(fd);
    f();
    fflush(stderr);
    dup2(saved, 2);
    close(saved);
    std::ifstream i(file);
    return std::string((std::istreambuf_iterator<char>(i)), std::istreambuf_iterator<char>());
}

static void writeFile(const std::string& path, const std::string& text) { std::ofstream o(path); o << text << "\n"; }

static void nativeRobustChecks(const std::string& dir, const char* ext, const std::vector<DeviceInfo>& list) {
    using namespace dect2::native;
    printf("native-robust:\n");
    const std::string logFile = dir + "/stderr.txt";

    // #12: a missing required function is reported once, naming the library; a missing optional one is not
    {
        DynLib lib;
        const std::string log = captureStderr(logFile, [&] {
            CHECK(lib.open({dir + "/librtlsdr" + ext}), "the fake library does not open");
            void (*fn)() = nullptr;
            CHECK(!lib.opt(fn, "rtlsdr_no_such_optional"), "optional lookup found a missing function");
            CHECK(!lib.get(fn, "rtlsdr_no_such_function") && !lib.get(fn, "rtlsdr_another_missing"), "lookup found a missing function");
            CHECK(lib.get(fn, "rtlsdr_open"), "rtlsdr_open not found");
        });
        CHECK(log.find("lacks rtlsdr_no_such_function (too old, or another library with the same name): rtlsdr radio disabled") != std::string::npos, "no message for the missing function: %s", log.c_str());
        CHECK(log.find("rtlsdr_no_such_optional") == std::string::npos && log.find("rtlsdr_another_missing") == std::string::npos, "logged more than once, or an optional function: %s", log.c_str());
    }

    // #27: versioned library names, newest first
    {
        CHECK(newerVersion("4.9.0", "4.8.0") && newerVersion("1.10", "1.9") && newerVersion("0.6.0", "0.6") && !newerVersion("2.0", "10.0") && !newerVersion("1.2", "1.2"), "version order");
        const std::string vd = dir + "/versions";
        mkdir(vd.c_str(), 0755);
#ifdef __APPLE__
        for (const char* v : {"1.2", "1.10", "0"}) writeFile(vd + "/libfoo." + v + ".dylib", "");
        writeFile(vd + "/libfoobar.9.dylib", "");
        const std::vector<std::string> want = {vd + "/libfoo.1.10.dylib", vd + "/libfoo.1.2.dylib", vd + "/libfoo.0.dylib"};
#else
        for (const char* v : {"1.2", "1.10", "0"}) writeFile(vd + "/libfoo.so." + v, "");
        writeFile(vd + "/libfoobar.so.9", "");
        const std::vector<std::string> want = {vd + "/libfoo.so.1.10", vd + "/libfoo.so.1.2", vd + "/libfoo.so.0"};
#endif
        const auto got = versionedLibs("foo", {vd + "/"});
        CHECK(got == want, "versioned libraries: %zu found, first %s", got.size(), got.empty() ? "-" : got[0].c_str());
    }

    // #9, #3: USB errors keep libusb's name, say what it means, and on Windows point at Zadig where a driver is the likely cause
    {
        const std::string a = usbErrorText(-3, true), b = usbErrorText(-6, true), c = usbErrorText(-12, false), d = usbErrorText(-12, true);
        CHECK(a.find("LIBUSB_ERROR_ACCESS: ") == 0 && a.find(windowsUsbDriverHint()) != std::string::npos, "access on Windows: %s", a.c_str());
        CHECK(b.find("LIBUSB_ERROR_BUSY: ") == 0 && b.find("Zadig") == std::string::npos, "busy: %s", b.c_str());
        CHECK(c.find("LIBUSB_ERROR_NOT_SUPPORTED: ") == 0 && c.find("Zadig") == std::string::npos, "not supported elsewhere: %s", c.c_str());
        CHECK(d.find("zadig.akeo.ie") != std::string::npos, "not supported on Windows: %s", d.c_str());
    }

    const DeviceInfo* rtl = nullptr;
    for (const auto& d : list) if (d.board == "rtlsdr") rtl = &d;
    void* h = dlopen((dir + "/librtlsdr" + ext).c_str(), RTLD_NOW | RTLD_NOLOAD);
    auto set = h ? (void (*)(const char*, double))dlsym(h, "fake_set") : nullptr;
    fakeState = h ? (double (*)(const char*))dlsym(h, "fake_state") : nullptr;
    CHECK(rtl && set && fakeState, "the RTL-SDR fake is not there");
    if (!rtl || !set || !fakeState) return;
    TuneSettings t;
    t.centerHz = 522e6; t.sampleRate = 2.4e6; t.gainDb = 30; t.bandwidthMhz = 8;
    std::complex<float> mean(0, 0);

    // #28: an open that fails with "busy" is tried again; one that keeps failing gives up after 3 more tries
    {
        IqRing ring(1 << 22);
        auto src = makeSource(*rtl);
        std::string e;
        set("rtl.busyOpens", 2);
        const double opens0 = std::max(0.0, st("rtl.opens"));
        const auto t0 = std::chrono::steady_clock::now();
        bool ok = false;
        const std::string log = captureStderr(logFile, [&] { ok = src->start(t, ring, e); });
        const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        CHECK(ok, "start after two busy opens failed: %s", e.c_str());
        CHECK(st("rtl.opens") - opens0 == 3, "%.0f opens (3 expected)", st("rtl.opens") - opens0);
        CHECK(secs > 0.9 && secs < 3, "the retries took %.2f s", secs);
        CHECK(log.find("trying again (1 of 3)") != std::string::npos && log.find("trying again (2 of 3)") != std::string::npos && log.find("(3 of 3)") == std::string::npos, "retry log: %s", log.c_str());
        if (ok) src->stop();
        set("rtl.busyOpens", 10);
        const double opens1 = st("rtl.opens");
        const auto t1 = std::chrono::steady_clock::now();
        captureStderr(logFile, [&] { ok = src->start(t, ring, e); });
        const double secs1 = std::chrono::duration<double>(std::chrono::steady_clock::now() - t1).count();
        CHECK(!ok && st("rtl.opens") - opens1 == 4, "a radio that stays busy: started %d, %.0f opens (4 expected)", ok, st("rtl.opens") - opens1);
        CHECK(secs1 > 1.3 && secs1 < 3, "giving up took %.2f s (about 1.5 expected)", secs1);
        set("rtl.busyOpens", 0);
    }

    // #13: the radio is unplugged while streaming and comes back: the source opens it again with the settings in use, into the same ring
    {
        IqRing ring(1 << 22);
        auto src = makeSource(*rtl);
        std::string e;
        bool ok = false;
        size_t before = 0, away = 0, back = 0;
        const std::string log = captureStderr(logFile, [&] {
            ok = src->start(t, ring, e);
            if (!ok) return;
            before = drain(ring, 300, mean);
            set("rtl.gone", 1);
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            std::vector<cf32> sink(1 << 16);
            while (ring.read(sink.data(), sink.size())) {}
            away = drain(ring, 1500, mean);
            t.centerHz = 600e6; t.gainDb = 40;   // changed while the radio is away: used when it is back
            std::string e2;
            CHECK(src->retune(t, e2), "retune while the radio is away: %s", e2.c_str());
            set("rtl.gone", 0);
            const auto t0 = std::chrono::steady_clock::now();
            while (std::chrono::steady_clock::now() - t0 < std::chrono::seconds(4) && ring.available() == 0) std::this_thread::sleep_for(std::chrono::milliseconds(20));
            back = drain(ring, 400, mean);
            src->stop();
        });
        CHECK(ok, "start failed: %s", e.c_str());
#ifdef __APPLE__
        CHECK(st("rtl.syncReads") > 0, "macOS should read the RTL-SDR block by block (issue #14), not with read_async");
#else
        CHECK(st("rtl.syncReads") <= 0, "only macOS reads the RTL-SDR block by block");
#endif
        CHECK(before > 8000, "only %zu samples before the unplug", before);
        CHECK(away == 0, "%zu samples while the radio was away", away);
        CHECK(back > 8000, "only %zu samples after the radio came back", back);
        CHECK(std::fabs(mean.real() - 0.5f) < 0.02f && std::fabs(mean.imag() + 0.25f) < 0.02f, "samples after the reconnect wrong: %.3f %.3f", mean.real(), mean.imag());
        CHECK(st("rtl.freq") == 600e6 && st("rtl.gain") == 370, "reopened with freq %.0f gain %.0f (the settings of the retune expected)", st("rtl.freq"), st("rtl.gain"));
        CHECK(log.find("radio lost") != std::string::npos, "no \"radio lost\": %s", log.c_str());
        CHECK(log.find("radio reconnected") != std::string::npos, "no \"radio reconnected\": %s", log.c_str());
        const size_t na = log.find("radio not available yet");
        CHECK(na != std::string::npos && log.find("radio not available yet", na + 1) == std::string::npos, "the reason the radio is not back should be logged once: %s", log.c_str());
        const size_t after = ring.available();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        CHECK(ring.available() == after, "samples kept arriving after stop()");
    }
    // a radio that is away when stop() is called: stop() returns at once (no reconnect attempt holds it up)
    {
        IqRing ring(1 << 22);
        auto src = makeSource(*rtl);
        std::string e;
        captureStderr(logFile, [&] {
            if (!src->start(t, ring, e)) return;
            set("rtl.gone", 1);
            std::this_thread::sleep_for(std::chrono::milliseconds(400));
            const auto t0 = std::chrono::steady_clock::now();
            src->stop();
            const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            CHECK(secs < 0.5, "stop() while the radio is away took %.2f s", secs);
        });
        set("rtl.gone", 0);
    }
    // the same for an Airspy: libairspy only stops streaming when the radio goes (airspy_is_streaming), the driver has to notice
    {
        const DeviceInfo* asp = nullptr;
        for (const auto& d : list) if (d.board == "airspy") asp = &d;
        void* ha = dlopen((dir + "/libairspy" + ext).c_str(), RTLD_NOW | RTLD_NOLOAD);
        auto aset = ha ? (void (*)(const char*, double))dlsym(ha, "fake_set") : nullptr;
        auto ast = ha ? (double (*)(const char*))dlsym(ha, "fake_state") : nullptr;
        CHECK(asp && aset && ast, "the Airspy fake is not there");
        if (asp && aset && ast) {
            IqRing ring(1 << 22);
            auto src = makeSource(*asp);
            std::string e;
            bool ok = false;
            size_t away = 0, back = 0;
            TuneSettings ta = t;
            ta.sampleRate = 10e6;
            const std::string log = captureStderr(logFile, [&] {
                ok = src->start(ta, ring, e);
                if (!ok) return;
                aset("airspy.gone", 1);
                std::this_thread::sleep_for(std::chrono::milliseconds(300));
                std::vector<cf32> sink(1 << 16);
                while (ring.read(sink.data(), sink.size())) {}
                away = drain(ring, 1000, mean);
                aset("airspy.gone", 0);
                const auto t0 = std::chrono::steady_clock::now();
                while (std::chrono::steady_clock::now() - t0 < std::chrono::seconds(4) && ring.available() == 0) std::this_thread::sleep_for(std::chrono::milliseconds(20));
                back = drain(ring, 300, mean);
                src->stop();
            });
            aset("airspy.gone", 0);
            CHECK(ok, "Airspy start failed: %s", e.c_str());
            CHECK(away == 0 && back > 8000, "Airspy unplug: %zu samples while away, %zu after it came back", away, back);
            CHECK(log.find("radio lost") != std::string::npos && log.find("radio reconnected") != std::string::npos, "Airspy unplug log: %s", log.c_str());
        }
    }
    fakeState = nullptr;

    // #8: the Linux USB check against a fake /sys and /dev tree
    {
        const std::string root = dir + "/root";
        for (const char* d : {"", "/sys", "/sys/bus", "/sys/bus/usb", "/sys/bus/usb/devices", "/sys/module", "/sys/module/dvb_usb_rtl28xxu", "/dev", "/dev/bus", "/dev/bus/usb", "/dev/bus/usb/001",
                              "/sys/bus/usb/devices/1-1", "/sys/bus/usb/devices/1-2", "/sys/bus/usb/devices/1-2:1.0", "/sys/bus/usb/devices/1-3", "/sys/bus/usb/devices/usb1",
                              "/sys/bus/usb/devices/1-4", "/sys/bus/usb/devices/1-4:1.0"})
            mkdir((root + d).c_str(), 0755);
        auto dev = [&](const char* name, const char* vid, const char* pid, int devnum, int mode) {
            const std::string p = root + "/sys/bus/usb/devices/" + name;
            writeFile(p + "/idVendor", vid); writeFile(p + "/idProduct", pid); writeFile(p + "/busnum", "1"); writeFile(p + "/devnum", std::to_string(devnum));
            char node[64];
            snprintf(node, sizeof node, "/dev/bus/usb/001/%03d", devnum);
            writeFile(root + node, "");
            chmod((root + node).c_str(), mode);
        };
        dev("1-1", "1d50", "6089", 5, 0);       // a HackRF this user may not open
        dev("1-2", "0bda", "2838", 6, 0666);    // an RTL-SDR held by the TV driver
        dev("1-3", "046d", "c52b", 7, 0);       // a mouse
        dev("usb1", "1d6b", "0002", 1, 0);      // the root hub
        dev("1-4", "1df7", "2500", 8, 0666);    // an RSP1 held by the kernel's Mirics driver (#17)
        symlink("../../../../bus/usb/drivers/dvb_usb_rtl28xxu", (root + "/sys/bus/usb/devices/1-2:1.0/driver").c_str());
        symlink("../../../../bus/usb/drivers/msi2500", (root + "/sys/bus/usb/devices/1-4:1.0/driver").c_str());
        setenv("DECT2_SYSFS_ROOT", root.c_str(), 1);
        const auto hints = usbRadioHints();
        unsetenv("DECT2_SYSFS_ROOT");
        for (const auto& s : hints) printf("  hint: %s\n", s.c_str());
        const bool root0 = geteuid() == 0;   // root may open anything
        CHECK(hints.size() == (root0 ? 2u : 3u), "%zu hints", hints.size());
        bool hack = false, tv = false, miri = false;
        for (const auto& s : hints) {
            if (s.find("HackRF is plugged in but this user may not open it: install OnAir's udev rules") == 0) hack = true;
            if (s.find("The Linux TV driver dvb_usb_rtl28xxu holds the RTL-SDR: run sudo modprobe -r dvb_usb_rtl28xxu") == 0) tv = true;
            if (s.find("The Linux driver msi2500 holds the SDRplay RSP") == 0) miri = true;
        }
        CHECK((hack || root0) && tv && miri, "hints: HackRF permission %d, RTL TV driver %d, SDRplay held by msi2500 %d", hack, tv, miri);
    }
}
// ---- end native-robust

// ---- radios: the RTL-SDR's direct sampling below 24 MHz, tuning ranges and their error, the RTL-SDR's rate limit, the bladeRF 1's FPGA,
// LimeSDR recalibration after a big retune, the library's own error text
static void radiosChecks(const char* dir, const char* ext) {
    printf("radios:\n");
    void (*set)(const char*, double) = nullptr;
    auto use = [&](const char* stem) {
        void* h = dlopen((std::string(dir) + "/lib" + stem + ext).c_str(), RTLD_NOW | RTLD_NOLOAD);
        set = h ? (void (*)(const char*, double))dlsym(h, "fake_set") : nullptr;
        fakeState = h ? (double (*)(const char*))dlsym(h, "fake_state") : nullptr;
        CHECK(set && fakeState, "the fake lib%s is not loaded", stem);
        return set && fakeState;
    };
    auto find = [](const char* board) {
        std::string e;
        for (const auto& d : listNativeDevices(e)) if (d.board == board) return d;
        return DeviceInfo{};
    };
    auto has = [](const std::string& s, const char* what) { return s.find(what) != std::string::npos; };
    std::complex<float> mean(0, 0);

    if (use("rtlsdr")) {   // a generic R820T dongle has no HF input: 24-1766 MHz; the Blog V3: 0.5-1766 MHz, direct sampling (Q branch) below 24 MHz, switched live
        {
            const DeviceInfo g = find("rtlsdr");
            CHECK(g.minFreqHz == 24e6 && g.maxFreqHz == 1766e6, "generic RTL range %.0f-%.0f", g.minFreqHz, g.maxFreqHz);
            IqRing ring(1 << 22);
            auto src = makeSource(g);
            TuneSettings t;
            t.centerHz = 7e6; t.sampleRate = 2.048e6; t.gainDb = 30; t.bandwidthMhz = 0.2;
            std::string e;
            const double calls = std::max(0.0, st("rtl.dsCalls"));
            // no frequency is refused: a generic dongle at 7 MHz is asked as it is, through the Q input (Auto), with a note that it has no HF input
            const bool g7 = src->start(t, ring, e);
            CHECK(g7 && st("rtl.ds") == 2 && st("rtl.freq") == 7e6 && has(e, "weak or absent") && std::max(0.0, st("rtl.dsCalls")) == calls + 1, "generic dongle at 7 MHz: %s (ds %.0f)", e.c_str(), st("rtl.ds"));
            if (g7) src->stop();
            src.reset();
            // Off: never direct sampling, the tuner is asked all the same (the driver decides), with the range note
            t.radio["ds"] = "off";
            auto off = makeSource(g);
            e.clear();
            const bool o7 = off->start(t, ring, e);
            CHECK(o7 && std::max(0.0, st("rtl.dsCalls")) == calls + 1 && st("rtl.freq") == 7e6 && has(e, "specified for 24 to 1766 MHz") && has(e, "weak or absent"), "generic dongle, direct sampling off, 7 MHz: %s", e.c_str());
            if (o7) off->stop();
            t.radio.erase("ds");
            // a dongle modified for HF: the Q input picked in the radio settings
            t.radio["ds"] = "q";
            auto q = makeSource(g);
            e.clear();
            const bool ok = q->start(t, ring, e);
            CHECK(ok && st("rtl.ds") == 2 && st("rtl.freq") == 7e6, "generic dongle, Q input picked: %s (ds %.0f)", e.c_str(), st("rtl.ds"));
            if (ok) q->stop();
        }
        set("rtl.v3", 1);
        const DeviceInfo d = find("rtlsdr");
        CHECK(d.minFreqHz == 0.5e6 && d.maxFreqHz == 1766e6, "RTL Blog V3 range %.0f-%.0f", d.minFreqHz, d.maxFreqHz);
        IqRing ring(1 << 22);
        auto src = makeSource(d);
        TuneSettings t;
        t.centerHz = 7e6; t.sampleRate = 2.048e6; t.gainDb = 30; t.bandwidthMhz = 0.2;
        std::string e;
        const bool ok = src && src->start(t, ring, e);
        CHECK(ok, "RTL start at 7 MHz: %s", e.c_str());
        if (ok) {
            CHECK(st("rtl.ds") == 2 && st("rtl.freq") == 7e6, "7 MHz: direct sampling %.0f, freq %.0f", st("rtl.ds"), st("rtl.freq"));
            CHECK(drain(ring, 150, mean) > 1000, "no samples with direct sampling");
            t.centerHz = 100e6;
            CHECK(src->retune(t, e) && st("rtl.ds") == 0 && st("rtl.freq") == 100e6, "100 MHz: direct sampling %.0f (%s)", st("rtl.ds"), e.c_str());
            t.centerHz = 14e6;
            CHECK(src->retune(t, e) && st("rtl.ds") == 2 && st("rtl.freq") == 14e6, "back to 14 MHz: direct sampling %.0f", st("rtl.ds"));
            t.centerHz = 2000e6;
            e.clear();
            CHECK(src->retune(t, e) && st("rtl.freq") == 2000e6 && st("rtl.ds") == 0 && has(e, "specified for 0.5 to 1766 MHz") && has(e, "weak or absent"), "above the range: passed on with a note: %s", e.c_str());
            CHECK(drain(ring, 100, mean) > 1000, "the stream stopped after an out-of-range retune");
            src->stop();
        }
        src.reset();
        // a TV channel at 10 Msps: refused with the reason, not squeezed into 2.56 Msps
        t.centerHz = 522e6; t.sampleRate = 10e6; t.bandwidthMhz = 8;
        auto tv = makeSource(d);
        e.clear();
        CHECK(!tv->start(t, ring, e) && has(e, "RTL-SDR gives at most 3.2 Msps; DVB-T/T2 needs about 10 Msps: use a faster radio"), "10 Msps: %s", e.c_str());
        tv.reset();
        // an R820T whose PLL does not lock at the first try is asked again; a second refusal is not final for a frequency it is specified for
        {
            t.centerHz = 100e6; t.sampleRate = 2.048e6; t.bandwidthMhz = 0.2;
            set("rtl.freqFail", 1);
            auto pll = makeSource(d);
            e.clear();
            CHECK(pll->start(t, ring, e), "PLL unlocked once: %s", e.c_str());
            pll->stop();
            set("rtl.freqFail", 0);
        }
        // the Blog V4 converts HF up itself: no direct sampling
        set("rtl.v4", 1);
        const double calls = std::max(0.0, st("rtl.dsCalls"));
        auto v4 = makeSource(d);
        t.centerHz = 7e6; t.sampleRate = 2.048e6; t.bandwidthMhz = 0.2;
        e.clear();
        CHECK(v4->start(t, ring, e), "V4 at 7 MHz: %s", e.c_str());
        CHECK(std::max(0.0, st("rtl.dsCalls")) == calls && st("rtl.freq") == 7e6 && e.empty(), "V4: direct sampling switched (%.0f calls), note '%s'", st("rtl.dsCalls") - calls, e.c_str());
        // a V4 goes down to 9 MHz (and below) and up through the same call: nothing is refused, direct sampling is never asked for
        for (double fq : {9e6, 100e3, 1e9, 2e9}) {
            t.centerHz = fq; e.clear();
            CHECK(v4->retune(t, e) && st("rtl.freq") == fq && std::max(0.0, st("rtl.dsCalls")) == calls, "V4 at %.0f Hz: freq %.0f, direct sampling calls %.0f, %s", fq, st("rtl.freq"), st("rtl.dsCalls") - calls, e.c_str());
        }
        // above what a 32-bit Hz value holds: cut to that, and said
        t.centerHz = 6e9; e.clear();
        CHECK(v4->retune(t, e) && st("rtl.freq") == 4.29e9 && has(e, "4.29 GHz"), "V4 at 6 GHz: freq %.0f, %s", st("rtl.freq"), e.c_str());
        v4->stop();
        v4.reset();
        set("rtl.v4", 0);
        t.centerHz = 7e6;
        // the V4 Lite (an R820T, USB product "Blog V4L") upconverts HF in its library as well: no direct sampling, listed from 0.5 MHz
        set("rtl.v3", 0); set("rtl.v4l", 1);
        const DeviceInfo dl = find("rtlsdr");
        CHECK(dl.minFreqHz == 0.5e6, "V4 Lite range from %.0f", dl.minFreqHz);
        auto v4l = makeSource(dl);
        e.clear();
        CHECK(v4l->start(t, ring, e), "V4 Lite at 7 MHz: %s", e.c_str());
        CHECK(std::max(0.0, st("rtl.dsCalls")) == calls && st("rtl.freq") == 7e6, "V4 Lite: direct sampling switched (%.0f calls)", st("rtl.dsCalls") - calls);
        t.centerHz = 9e6; e.clear();
        CHECK(v4l->retune(t, e) && st("rtl.freq") == 9e6 && std::max(0.0, st("rtl.dsCalls")) == calls, "V4 Lite at 9 MHz: %.0f, %s", st("rtl.freq"), e.c_str());
        v4l->stop();
        v4l.reset();
        set("rtl.v4l", 0);
    }

    if (use("bladeRF")) {
        CHECK(find("bladerf").minFreqHz == 70e6 && find("bladerf").maxFreqHz == 6e9, "bladeRF 2 range");
        IqRing ring(1 << 22);
        TuneSettings t;
        t.centerHz = 522e6; t.sampleRate = 10e6; t.gainDb = 30;
        std::string e;
        // the library's own words for a failed open
        set("blade.openErr", -7);
        auto src = makeSource(find("bladerf"));
        CHECK(!src->start(t, ring, e) && has(e, "No device(s) available"), "bladeRF open error: %s", e.c_str());
        src.reset();
        set("blade.openErr", 0);
        auto entries = [](const char* board) {
            std::vector<DeviceInfo> l;
            std::string le;
            for (const auto& d : listNativeDevices(le)) if (d.board == board) l.push_back(d);
            return l;
        };
        {   // the bladeRF 2.0 lists RX1 (what OnAir used before) and RX2, which streams BLADERF_CHANNEL_RX(1) = 2
            const auto l = entries("bladerf");
            CHECK(l.size() == 2 && has(l[0].name, " RX1 ") && has(l[1].name, " RX2 "), "bladeRF 2.0: %zu entries (%s)", l.size(), l.empty() ? "" : l[0].name.c_str());
            for (size_t i = 0; i < l.size() && i < 2; i++) {
                auto s2 = makeSource(l[i]);
                e.clear();
                const bool ok = s2->start(t, ring, e);
                const double ch = 2.0 * (double)i;
                CHECK(ok && st("blade.enabledCh") == ch && st("blade.freqCh") == ch && st("blade.gainCh") == ch && st("blade.layout") == 0,
                      "bladeRF %s: channel enabled %.0f, tuned %.0f, gain %.0f, layout %.0f (%s)", i ? "RX2" : "RX1", st("blade.enabledCh"), st("blade.freqCh"), st("blade.gainCh"), st("blade.layout"), e.c_str());
                CHECK(has(l[i].nativeArgs, "0123456789ABCDEF"), "bladeRF entry without the serial: %s", l[i].nativeArgs.c_str());
                if (ok) s2->stop();
            }
        }
        // a bladeRF 1 whose FPGA is not loaded: no image anywhere, then one in the search folder
        set("blade.v1", 1);
        const DeviceInfo d1 = find("bladerf");
        CHECK(entries("bladerf").size() == 1 && !has(d1.name, "RX1"), "bladeRF 1: %zu entries (%s)", entries("bladerf").size(), d1.name.c_str());
        CHECK(d1.minFreqHz == 237.5e6 && d1.maxFreqHz == 3.8e9, "bladeRF 1 range %.0f-%.0f", d1.minFreqHz, d1.maxFreqHz);
        const std::string fdir = std::string(dir) + "/fpga";
        mkdir(fdir.c_str(), 0755);
        setenv("BLADERF_SEARCH_DIR", fdir.c_str(), 1);
        src = makeSource(d1);
        e.clear();
        const bool ok0 = src->start(t, ring, e);
        CHECK((!ok0 && has(e, "bladeRF 1 needs its FPGA image (hostedx40.rbf or hostedx115.rbf): download it from nuand.com and put it next to OnAir")) ||
              (ok0 && st("blade.fpgaLoaded") == 1),   // (an image installed on this machine is found too)
              "bladeRF 1 without an image: %s", e.c_str());
        if (ok0) src->stop();
        src.reset();
        { std::ofstream(fdir + "/hostedx40.rbf") << "fake"; }
        src = makeSource(d1);
        e.clear();
        const bool ok1 = src->start(t, ring, e);
        CHECK(ok1 && st("blade.fpgaLoaded") == 1, "bladeRF 1 with hostedx40.rbf: %s", e.c_str());
        if (ok1) {
            t.centerHz = 100e6;
            e.clear();
            CHECK(src->retune(t, e) && st("blade.freq") == 100e6 && has(e, "specified for 237.5 to 3800 MHz") && has(e, "weak or absent"), "bladeRF 1 at 100 MHz is passed on with a note: %s", e.c_str());
            src->stop();
        }
        src.reset();
        unsetenv("BLADERF_SEARCH_DIR");
        set("blade.v1", 0);
    }

    if (use("LimeSuite")) {   // one entry per socket after the automatic one; the LNAW entry uses LNAW even at 522 MHz (where auto takes LNAL)
        std::string e;
        std::vector<DeviceInfo> lime;
        for (const auto& d : listNativeDevices(e)) if (d.board == "lime") lime.push_back(d);
        CHECK(lime.size() == 4 && has(lime[1].name, "LNAL") && has(lime[2].name, "LNAH") && has(lime[3].name, "LNAW") && !has(lime[0].name, "LNA"),
              "LimeSDR-USB entries: %zu", lime.size());
        if (lime.size() == 4) {
            IqRing ring(1 << 22);
            auto src = makeSource(lime[3]);
            TuneSettings t;
            t.centerHz = 522e6; t.sampleRate = 10e6; t.gainDb = 30;
            const bool ok = src->start(t, ring, e);
            CHECK(ok && st("lime.antenna") == 3, "LNAW entry: antenna index %.0f, LNAW (3) expected", st("lime.antenna"));
            t.centerHz = 2400e6;
            CHECK(src->retune(t, e) && st("lime.antenna") == 3, "LNAW entry switched input on a retune to 2.4 GHz (%.0f)", st("lime.antenna"));
            src->stop();
        }
    }
    if (use("LimeSuite")) {   // the range LMS_GetLOFrequencyRange gave at the last open is listed; a big retune recalibrates
        const DeviceInfo d = find("lime");
        CHECK(d.minFreqHz == 30e6 && d.maxFreqHz == 3.8e9, "LimeSDR range %.0f-%.0f", d.minFreqHz, d.maxFreqHz);
        IqRing ring(1 << 22);
        auto src = makeSource(d);
        TuneSettings t;
        t.centerHz = 522e6; t.sampleRate = 10e6; t.gainDb = 30;
        std::string e;
        const bool ok = src->start(t, ring, e);
        CHECK(ok, "LimeSDR start: %s", e.c_str());
        if (ok) {
            const double c0 = st("lime.calibrations");
            t.centerHz = 560e6;
            CHECK(src->retune(t, e) && st("lime.calibrations") == c0, "a 7 %% retune recalibrated");
            t.centerHz = 2400e6;
            CHECK(src->retune(t, e) && st("lime.calibrations") == c0 + 1, "no recalibration after 560 -> 2400 MHz (%.0f)", st("lime.calibrations") - c0);
            CHECK(st("lime.streaming") == 1 && drain(ring, 150, mean) > 3000, "the stream did not come back after the recalibration");
            t.centerHz = 2450e6;
            CHECK(src->retune(t, e) && st("lime.calibrations") == c0 + 1, "a 2 %% retune recalibrated");
            t.centerHz = 5000e6;
            e.clear();
            CHECK(src->retune(t, e) && st("lime.freq") == 5000e6 && has(e, "specified for 30 to 3800 MHz") && has(e, "weak or absent"), "LimeSDR at 5 GHz is passed on with a note: %s", e.c_str());
            src->stop();
        }
    }

    if (use("iio")) {   // over USB a Pluto carries 4 Msps without loss (steadyRateHz), the converter goes to 61.44; a stock one tunes 325-3800 MHz
        const DeviceInfo d = find("pluto");
        CHECK(d.maxRateHz == 61.44e6 && d.steadyRateHz == 4e6, "Pluto over USB: max rate %.0f, steady %.0f", d.maxRateHz, d.steadyRateHz);
        IqRing ring(1 << 22);
        auto src = makeSource(d);
        TuneSettings t;
        t.centerHz = 100e6; t.sampleRate = 4e6; t.gainDb = 30;
        std::string e;
        CHECK(src->start(t, ring, e) && st("pluto.phy:altvoltage0.frequency") == 100e6 && has(e, "specified for 325 to 3800 MHz") && has(e, "weak or absent"), "Pluto at 100 MHz is passed on with a note: %s", e.c_str());
        src->stop();
    }

    if (use("uhd")) {
        const DeviceInfo d = find("usrp");
        CHECK(d.minFreqHz == 70e6 && d.maxFreqHz == 6e9, "USRP range %.0f-%.0f", d.minFreqHz, d.maxFreqHz);
        IqRing ring(1 << 22);
        auto src = makeSource(d);
        TuneSettings t;
        t.centerHz = 10e6; t.sampleRate = 10e6; t.gainDb = 30;
        std::string e;
        const bool u10 = src->start(t, ring, e);
        CHECK(u10 && st("usrp.freq") == 10e6 && has(e, "specified for 70 to 6000 MHz") && has(e, "weak or absent"), "USRP at 10 MHz is passed on with a note: %s", e.c_str());
        if (u10) src->stop();
        src.reset();
        // a B200 lists RX2 (UHD's default, left alone: what OnAir did before) and TX/RX; a B210 the same on RF A (channel 0) and RF B (1)
        struct Want { const char* label; double ch, antenna; };   // antenna 0 = not set, 1 = RX2, 2 = TX/RX
        const Want b200[] = {{" RX2 ", 0, 0}, {" TX/RX ", 0, 2}};
        const Want b210[] = {{" RF A RX2 ", 0, 0}, {" RF A TX/RX ", 0, 2}, {" RF B RX2 ", 1, 1}, {" RF B TX/RX ", 1, 2}};
        t.centerHz = 522e6;
        for (int m = 0; m < 2; m++) {
            set("usrp.b210", m);
            std::vector<DeviceInfo> l;
            std::string le;
            for (const auto& x : listNativeDevices(le)) if (x.board == "usrp") l.push_back(x);
            const Want* w = m ? b210 : b200;
            const size_t n = m ? 4 : 2;
            CHECK(l.size() == n, "USRP %s: %zu entries", m ? "B210" : "B200", l.size());
            if (l.size() == n) CHECK(l[0].nativeArgs == "type=b200,name=,serial=30C6E4D,product=" + std::string(m ? "B210" : "B200"), "USRP default entry args: %s", l[0].nativeArgs.c_str());
            for (size_t i = 0; i < l.size() && i < n; i++) {
                CHECK(has(l[i].name, w[i].label), "USRP entry %zu: %s (%s expected)", i, l[i].name.c_str(), w[i].label);
                set("usrp.antennaCalls", 0);
                auto s2 = makeSource(l[i]);
                e.clear();
                const bool ok = s2->start(t, ring, e);
                CHECK(ok && st("usrp.freqCh") == w[i].ch && st("usrp.streamCh") == w[i].ch, "USRP%s: tuned channel %.0f, streamed %.0f (%s)", w[i].label, st("usrp.freqCh"), st("usrp.streamCh"), e.c_str());
                if (w[i].antenna == 0) CHECK(st("usrp.antennaCalls") == 0, "USRP%s: the default entry set the antenna", w[i].label);
                else CHECK(st("usrp.antennaCalls") == 1 && st("usrp.antenna") == w[i].antenna && st("usrp.antennaCh") == w[i].ch, "USRP%s: antenna %.0f on channel %.0f", w[i].label, st("usrp.antenna"), st("usrp.antennaCh"));
                if (ok) s2->stop();
            }
        }
        set("usrp.b210", 0);
    }
    fakeState = nullptr;
}
// ---- end radios

// ---- radio settings
// The radios' own settings (DeviceInfo::settings, TuneSettings::radio): each reaches its vendor call, and a radio started without any
// calls nothing new (what OnAir did before the settings existed).
namespace dect2 {
uint32_t hackrfAutoFilterHz(double sampleRate, double channelMhz);   // hackrf_usb.cpp
void hackrfBoardRange(int id, double& lo, double& hi);
}
static bool hasSetting(const DeviceInfo& d, const char* key) {
    for (const auto& r : d.settings) if (r.key == key) return true;
    return false;
}
static void radioSettingsChecks(const char* dir, const char* ext) {
    printf("radio settings:\n");
    void (*set)(const char*, double) = nullptr;
    auto use = [&](const char* file) {
        void* h = dlopen((std::string(dir) + "/" + file).c_str(), RTLD_NOW | RTLD_NOLOAD);
        set = h ? (void (*)(const char*, double))dlsym(h, "fake_set") : nullptr;
        fakeState = h ? (double (*)(const char*))dlsym(h, "fake_state") : nullptr;
        CHECK(set && fakeState, "the fake %s is not loaded", file);
        return set && fakeState;
    };
    auto lib = [&](const char* stem) { return std::string("lib") + stem + ext; };
    auto entries = [](const char* board) {
        std::vector<DeviceInfo> l;
        std::string e;
        for (const auto& d : listNativeDevices(e)) if (d.board == board) l.push_back(d);
        return l;
    };
    auto first = [&](const char* board) { const auto l = entries(board); return l.empty() ? DeviceInfo{} : l[0]; };
    auto has = [](const std::string& s, const char* what) { return s.find(what) != std::string::npos; };
    const double kNone = -1e300;   // a call that never happened
    IqRing ring(1 << 22);
    std::string e;

    // the helpers: an untouched correction leaves every frequency exactly as it was
    {
        TuneSettings t;
        CHECK(radioPpm(t) == 0 && ppmCorrectedHz(522e6, radioPpm(t)) == 522e6, "no correction: %.3f", ppmCorrectedHz(522e6, radioPpm(t)));
        t.radio["ppm"] = "20";
        CHECK(std::fabs(ppmCorrectedHz(100e6, radioPpm(t)) - 100e6 / 1.00002) < 1e-3, "20 ppm: %.3f", ppmCorrectedHz(100e6, radioPpm(t)));
        t.radio["ppm"] = "nonsense";
        CHECK(radioPpm(t) == 0, "a value that is not a number counts as none");
    }
    // HackRF: the automatic baseband filter is unchanged for 7-8 MHz at 10 Msps and narrow channels, and wide enough for a 6 MHz channel at 8 Msps
    CHECK(hackrfAutoFilterHz(10e6, 8) == 5000000 && hackrfAutoFilterHz(10e6, 7) == 5000000, "HackRF filter 8/7 MHz at 10 Msps: %u / %u", hackrfAutoFilterHz(10e6, 8), hackrfAutoFilterHz(10e6, 7));
    CHECK(hackrfAutoFilterHz(8e6, 6) == 5000000, "HackRF filter 6 MHz at 8 Msps: %u (5 MHz expected)", hackrfAutoFilterHz(8e6, 6));
    CHECK(hackrfAutoFilterHz(8e6, 5) == 3500000 && hackrfAutoFilterHz(8e6, 1.7) == 3500000 && hackrfAutoFilterHz(4e6, 0.25) == 1750000, "HackRF filter narrow channels: %u %u %u", hackrfAutoFilterHz(8e6, 5), hackrfAutoFilterHz(8e6, 1.7), hackrfAutoFilterHz(4e6, 0.25));
    {
        double lo = 0, hi = 0;
        hackrfBoardRange(2, lo, hi);
        CHECK(lo == 1e6 && hi == 6e9, "HackRF One range %.0f-%.0f", lo, hi);
        hackrfBoardRange(5, lo, hi);
        CHECK(lo == 0.1e6 && hi == 6e9, "HackRF Pro range %.0f-%.0f", lo, hi);
    }

    if (use(lib("rtlsdr").c_str())) {
        DeviceInfo d = first("rtlsdr");
        CHECK(hasSetting(d, "ppm") && hasSetting(d, "ds") && !hasSetting(d, "offset"), "R820T settings: ppm %d ds %d offset %d", hasSetting(d, "ppm"), hasSetting(d, "ds"), hasSetting(d, "offset"));
        TuneSettings t;
        t.centerHz = 100e6; t.sampleRate = 2.048e6; t.gainDb = 30; t.bandwidthMhz = 0.2;
        const double ds0 = std::max(0.0, st("rtl.dsCalls"));
        auto src = makeSource(d);
        e.clear();
        bool ok = src->start(t, ring, e);
        CHECK(ok && st("rtl.ppmCalls") == kNone && st("rtl.offsetCalls") == kNone && std::max(0.0, st("rtl.dsCalls")) == ds0 && st("rtl.freq") == 100e6,
              "RTL defaults: ppm calls %.0f, offset calls %.0f, ds calls %.0f, freq %.0f (%s)", st("rtl.ppmCalls"), st("rtl.offsetCalls"), st("rtl.dsCalls") - ds0, st("rtl.freq"), e.c_str());
        if (ok) {
            t.radio["ppm"] = "12.4";
            CHECK(src->retune(t, e) && st("rtl.ppm") == 12 && st("rtl.ppmCalls") == 1 && st("rtl.freq") == 100e6, "RTL 12.4 ppm: %.0f (%.0f calls), freq %.0f", st("rtl.ppm"), st("rtl.ppmCalls"), st("rtl.freq"));
            CHECK(src->retune(t, e) && st("rtl.ppmCalls") == 1, "RTL: the same correction set again (%.0f calls)", st("rtl.ppmCalls"));
            t.radio.erase("ppm");
            CHECK(src->retune(t, e) && st("rtl.ppm") == 0, "RTL: correction back to 0: %.0f", st("rtl.ppm"));
            src->stop();
        }
        src.reset();
        // a Blog V3 with direct sampling switched off: below 24 MHz is asked of the tuner all the same, with a note that says why
        set("rtl.v3", 1);
        t.centerHz = 7e6; t.radio["ds"] = "off";
        auto off = makeSource(first("rtlsdr"));
        e.clear();
        const double dsBefore = std::max(0.0, st("rtl.dsCalls"));
        CHECK(off->start(t, ring, e) && st("rtl.freq") == 7e6 && std::max(0.0, st("rtl.dsCalls")) == dsBefore && has(e, "specified for 24 to 1766 MHz") && has(e, "direct sampling is off"), "V3, direct sampling off, 7 MHz: asked all the same, with a note: %s", e.c_str());
        off->stop();
        off.reset();
        // the I input
        t.radio["ds"] = "i";
        auto iin = makeSource(first("rtlsdr"));
        e.clear();
        ok = iin->start(t, ring, e);
        CHECK(ok && st("rtl.ds") == 1, "V3, I input: ds %.0f (%s)", st("rtl.ds"), e.c_str());
        if (ok) iin->stop();
        iin.reset();
        set("rtl.v3", 0);
        t.radio.clear();
        // an E4000 takes offset tuning (an R820T does not: the Blog library switches the antenna power with that call). The listing keeps
        // the tuner it read first (a streaming dongle cannot be opened to ask), so only the driver, which reads it at open, is checked here
        set("rtl.tuner", 1);
        t.centerHz = 100e6;
        auto e4 = makeSource(d);
        e.clear();
        ok = e4->start(t, ring, e);
        CHECK(ok && st("rtl.offsetCalls") == kNone, "E4000 default: offset tuning called (%.0f) %s", st("rtl.offsetCalls"), e.c_str());
        if (ok) {
            t.radio["offset"] = "1";
            CHECK(e4->retune(t, e) && st("rtl.offset") == 1 && st("rtl.offsetCalls") == 1, "E4000 offset tuning: %.0f (%.0f calls)", st("rtl.offset"), st("rtl.offsetCalls"));
            e4->stop();
        }
        e4.reset();
        set("rtl.tuner", 0);
        set("rtl.v4", 1);
        CHECK(!hasSetting(first("rtlsdr"), "ds"), "the Blog V4 offers direct sampling");
        set("rtl.v4", 0);
    }

    if (use(lib("airspy").c_str())) {
        const DeviceInfo d = first("airspy");
        CHECK(hasSetting(d, "gainmode") && hasSetting(d, "ppm"), "Airspy settings");
        CHECK(d.maxRateHz == 10e6 && d.minRateHz == 2.5e6 && d.maxFreqHz == 1.8e9, "Airspy R2 listed %.0f-%.0f sps to %.0f Hz", d.minRateHz, d.maxRateHz, d.maxFreqHz);
        TuneSettings t;
        t.centerHz = 522e6; t.sampleRate = 10e6; t.gainDb = 15; t.bandwidthMhz = 8;
        auto src = makeSource(d);
        e.clear();
        const bool ok = src->start(t, ring, e);
        CHECK(ok && st("airspy.table") == 0 && st("airspy.gain") == 15 && st("airspy.freq") == 522e6, "Airspy defaults: table %.0f gain %.0f freq %.0f", st("airspy.table"), st("airspy.gain"), st("airspy.freq"));
        if (ok) {
            t.radio["gainmode"] = "sensitivity"; t.radio["ppm"] = "10";
            CHECK(src->retune(t, e) && st("airspy.table") == 1 && st("airspy.gain") == 15, "Airspy sensitivity: table %.0f gain %.0f", st("airspy.table"), st("airspy.gain"));
            CHECK(st("airspy.freq") == std::llround(522e6 / 1.00001), "Airspy 10 ppm: freq %.0f", st("airspy.freq"));
            src->stop();
        }
        src.reset();
        set("airspy.mini", 1);   // a Mini: its own rates, and 1.7 GHz at most
        const DeviceInfo m = first("airspy");
        // 10 Msps is listed too, used only with the "10 Msps (unofficial)" setting (#49)
        CHECK(m.maxRateHz == 10e6 && m.minRateHz == 3e6 && m.maxFreqHz == 1.7e9, "Airspy Mini listed %.0f-%.0f sps to %.0f Hz", m.minRateHz, m.maxRateHz, m.maxFreqHz);
        CHECK(hasSetting(m, "mini10"), "Airspy Mini offers the unofficial 10 Msps");
        {
            TuneSettings t;
            t.centerHz = 522e6; t.sampleRate = 10e6; t.gainDb = 10; t.bandwidthMhz = 8;
            for (int on = 0; on < 2; on++) {
                if (on) t.radio["mini10"] = "1";
                auto ms = makeSource(m);
                e.clear();
                if (ms->start(t, ring, e)) {
                    CHECK(st("airspy.rate") == (on ? 10e6 : 6e6), "Airspy Mini rate %.0f with mini10 %s", st("airspy.rate"), on ? "on" : "off");
                    ms->stop();
                } else CHECK(false, "Airspy Mini start: %s", e.c_str());
            }
        }
        set("airspy.mini", 0);
        (void)first("airspy");
    }

    if (use(lib("airspyhf").c_str())) {
        DeviceInfo d = first("airspyhf");
        CHECK(hasSetting(d, "ppm") && hasSetting(d, "preamp") && hasSetting(d, "agc") && !d.hasBiasTee, "HF+ settings / antenna power");
        TuneSettings t;
        t.centerHz = 7.1e6; t.sampleRate = 768e3; t.gainDb = 30; t.bandwidthMhz = 0.02;
        auto src = makeSource(d);
        e.clear();
        bool ok = src->start(t, ring, e);
        CHECK(ok && st("airspyhf.calCalls") == kNone && st("airspyhf.agc") == 0 && st("airspyhf.agcThr") == kNone && st("airspyhf.lna") == 0 && st("airspyhf.freq") == 7.1e6,
              "HF+ defaults: cal calls %.0f agc %.0f thr %.0f lna %.0f freq %.0f", st("airspyhf.calCalls"), st("airspyhf.agc"), st("airspyhf.agcThr"), st("airspyhf.lna"), st("airspyhf.freq"));
        if (ok) {
            t.radio["ppm"] = "1.5";   // on top of the radio's own 1234 ppb; a fast clock tunes lower
            CHECK(src->retune(t, e) && st("airspyhf.cal") == 1234 - 1500 && st("airspyhf.freq") == 7.1e6, "HF+ 1.5 ppm: calibration %.0f ppb, freq %.0f", st("airspyhf.cal"), st("airspyhf.freq"));
            t.radio["preamp"] = "on"; t.gainDb = 0;
            CHECK(src->retune(t, e) && st("airspyhf.lna") == 1 && st("airspyhf.att") == 8, "HF+ preamp on at gain 0: lna %.0f att %.0f", st("airspyhf.lna"), st("airspyhf.att"));
            t.radio["preamp"] = "off"; t.gainDb = 48;
            CHECK(src->retune(t, e) && st("airspyhf.lna") == 0 && st("airspyhf.att") == 0, "HF+ preamp off at gain 48: lna %.0f att %.0f", st("airspyhf.lna"), st("airspyhf.att"));
            t.radio["agc"] = "high";
            CHECK(src->retune(t, e) && st("airspyhf.agc") == 1 && st("airspyhf.agcThr") == 1, "HF+ AGC high: agc %.0f thr %.0f", st("airspyhf.agc"), st("airspyhf.agcThr"));
            t.radio.erase("ppm");
            CHECK(src->retune(t, e) && st("airspyhf.cal") == 1234, "HF+ correction back to the radio's own: %.0f", st("airspyhf.cal"));
            src->stop();
        }
        src.reset();
        // a model with antenna power
        set("airspyhf.biasCount", 1);
        d = first("airspyhf");
        CHECK(d.hasBiasTee, "HF+ with a bias-tee: not offered");
        t.radio.clear(); t.biasTee = true;
        auto b = makeSource(d);
        e.clear();
        ok = b->start(t, ring, e);
        CHECK(ok && st("airspyhf.bias") == 1, "HF+ antenna power on at start: %.0f", st("airspyhf.bias"));
        if (ok) b->stop();
        CHECK(st("airspyhf.bias") == 0, "HF+ antenna power after stop: %.0f", st("airspyhf.bias"));
        set("airspyhf.biasCount", 0);
    }

    if (use("libsdrplay_api.so")) {
        const unsigned kPpm = 0x2, kNotch1a = 0x20, kDab1a = 0x40;
        DeviceInfo d = first("sdrplay");
        CHECK(hasSetting(d, "ppm") && hasSetting(d, "rfnotch") && hasSetting(d, "dabnotch") && !hasSetting(d, "hdr"), "RSP1B settings");
        CHECK(d.minFreqHz == 1e3 && d.maxFreqHz == 2e9, "RSP1B range %.0f-%.0f", d.minFreqHz, d.maxFreqHz);
        TuneSettings t;
        t.centerHz = 522e6; t.sampleRate = 8e6; t.gainDb = 40; t.bandwidthMhz = 8;
        auto src = makeSource(d);
        e.clear();
        bool ok = src->start(t, ring, e);
        CHECK(ok && st("sdrplay.rfNotch1a") == 0 && st("sdrplay.dabNotch1a") == 0 && st("sdrplay.ppm") == 0, "RSP1B defaults: notch %.0f dab %.0f ppm %.2f", st("sdrplay.rfNotch1a"), st("sdrplay.dabNotch1a"), st("sdrplay.ppm"));
        if (ok) {
            t.centerHz = 530e6;
            CHECK(src->retune(t, e) && ((unsigned)st("sdrplay.reasons") & (kPpm | kNotch1a | kDab1a)) == 0, "RSP1B: an untouched setting was updated (reasons 0x%08x)", (unsigned)st("sdrplay.reasons"));
            t.radio["rfnotch"] = "1"; t.radio["ppm"] = "2.5";
            CHECK(src->retune(t, e) && st("sdrplay.rfNotch1a") == 1 && st("sdrplay.ppm") == 2.5 && ((unsigned)st("sdrplay.reasons") & (kPpm | kNotch1a)) == (kPpm | kNotch1a),
                  "RSP1B FM/MW notch and 2.5 ppm live: notch %.0f ppm %.2f reasons 0x%08x", st("sdrplay.rfNotch1a"), st("sdrplay.ppm"), (unsigned)st("sdrplay.reasons"));
            src->stop();
        }
        src.reset();
        t.radio = {{"dabnotch", "1"}};
        auto s2 = makeSource(d);
        e.clear();
        ok = s2->start(t, ring, e);
        CHECK(ok && st("sdrplay.dabNotch1a") == 1 && st("sdrplay.rfNotch1a") == 0, "RSP1B DAB notch at start: %.0f (FM/MW %.0f)", st("sdrplay.dabNotch1a"), st("sdrplay.rfNotch1a"));
        if (ok) s2->stop();
        s2.reset();
        t.radio.clear();
        // the RSP2 and RSPduo list their Hi-Z input last; it selects AMPORT_1 (the 50 ohm entries keep AMPORT_2)
        for (int model : {2, 3}) {
            set("sdrplay.model", model);
            const auto l = entries("sdrplay");
            CHECK(l.size() == 3 && has(l[2].name, "Hi-Z") && l[2].maxFreqHz == 30e6, "%s: %zu entries (%s)", model == 2 ? "RSP2" : "RSPduo", l.size(), l.size() > 2 ? l[2].name.c_str() : "");
            if (l.size() != 3) continue;
            const char* key = model == 2 ? "sdrplay.amPort2" : "sdrplay.amPortDuo";
            t.centerHz = 7e6; t.sampleRate = 2e6; t.bandwidthMhz = 0.2;
            for (int i : {0, 2}) {
                auto s3 = makeSource(l[(size_t)i]);
                e.clear();
                ok = s3->start(t, ring, e);
                CHECK(ok && st(key) == (i == 2 ? 1 : 0), "%s entry %d: AM port %.0f", model == 2 ? "RSP2" : "RSPduo", i, st(key));
                if (ok && i == 2) {
                    t.centerHz = 100e6;
                    e.clear();
                    CHECK(s3->retune(t, e) && has(e, "Hi-Z input is for 1 kHz to 30 MHz"), "Hi-Z at 100 MHz: no note (%s)", e.c_str());
                    t.centerHz = 7e6;
                    if (model == 3) {   // the RSPduo's Hi-Z input has its own MW notch
                        t.radio["rfnotch"] = "1";
                        CHECK(s3->retune(t, e) && st("sdrplay.amNotchDuo") == 1 && st("sdrplay.rfNotchDuo") == 0, "RSPduo Hi-Z notch: AM %.0f RF %.0f", st("sdrplay.amNotchDuo"), st("sdrplay.rfNotchDuo"));
                        t.radio.clear();
                    }
                }
                if (ok) s3->stop();
            }
        }
        // the RSPdx: HDR below 2 MHz only
        set("sdrplay.model", 4);
        d = first("sdrplay");
        CHECK(hasSetting(d, "hdr"), "RSPdx: no HDR setting");
        t.centerHz = 1e6; t.sampleRate = 2e6; t.bandwidthMhz = 0.2; t.radio = {{"hdr", "1"}};
        auto dx = makeSource(d);
        e.clear();
        ok = dx->start(t, ring, e);
        CHECK(ok && st("sdrplay.hdr") == 1, "RSPdx HDR at 1 MHz: %.0f", st("sdrplay.hdr"));
        if (ok) {
            t.centerHz = 10e6;
            CHECK(dx->retune(t, e) && st("sdrplay.hdr") == 0 && ((unsigned)st("sdrplay.reasons1") & 0x1), "RSPdx HDR at 10 MHz: %.0f (ext1 0x%x)", st("sdrplay.hdr"), (unsigned)st("sdrplay.reasons1"));
            dx->stop();
        }
        dx.reset();
        set("sdrplay.model", 0);
    }

    if (use(lib("iio").c_str())) {
        const DeviceInfo d = first("pluto");
        CHECK(hasSetting(d, "ppm") && hasSetting(d, "gainmode"), "Pluto settings");
        TuneSettings t;
        t.centerHz = 522e6; t.sampleRate = 4e6; t.gainDb = 30; t.bandwidthMhz = 1.7;
        auto src = makeSource(d);
        e.clear();
        const bool ok = src->start(t, ring, e);
        CHECK(ok && st("pluto.xoWrites") == kNone && st("pluto.phy:voltage0.gain_control_mode=manual") == 1, "Pluto defaults: xo writes %.0f (%s)", st("pluto.xoWrites"), e.c_str());
        CHECK(st("pluto.bufSamples") == 65536, "Pluto over USB: buffer of %.0f samples", st("pluto.bufSamples"));
        if (ok) {
            t.radio["ppm"] = "10";
            CHECK(src->retune(t, e) && st("pluto.phy.xo_correction") == 40000400 && st("pluto.phy:altvoltage0.frequency") == 522e6, "Pluto 10 ppm: xo %.0f, freq %.0f", st("pluto.phy.xo_correction"), st("pluto.phy:altvoltage0.frequency"));
            t.radio["gainmode"] = "fast_attack"; t.gainDb = 50;
            CHECK(src->retune(t, e) && st("pluto.phy:voltage0.gain_control_mode=fast_attack") == 1 && st("pluto.phy:voltage0.hardwaregain") == 30, "Pluto fast attack: gain written %.0f", st("pluto.phy:voltage0.hardwaregain"));
            // 2.048 Msps (DAB): below the converter's 2.083 Msps, so through its decimating FIR (the converter at twice the rate), off again for another rate
            t.sampleRate = 2.048e6;
            CHECK(src->retune(t, e) && st("pluto.phy:voltage0.sampling_frequency") == 2.048e6 && st("pluto.phy:voltage0.filter_fir_en") == 1 && st("pluto.firLoads") == 1 && st("pluto.phy.firTaps") == 16,
                  "Pluto 2.048 Msps: rate %.0f, FIR en %.0f, loads %.0f, taps %.0f (%s)", st("pluto.phy:voltage0.sampling_frequency"), st("pluto.phy:voltage0.filter_fir_en"), st("pluto.firLoads"), st("pluto.phy.firTaps"), e.c_str());
            t.sampleRate = 10e6;
            CHECK(src->retune(t, e) && st("pluto.phy:voltage0.sampling_frequency") == 10e6 && st("pluto.phy:voltage0.filter_fir_en") == 0, "Pluto 10 Msps after the FIR: rate %.0f, FIR en %.0f", st("pluto.phy:voltage0.sampling_frequency"), st("pluto.phy:voltage0.filter_fir_en"));
            t.sampleRate = 2.048e6;
            CHECK(src->retune(t, e) && st("pluto.phy:voltage0.filter_fir_en") == 1, "Pluto 2.048 Msps again");
            src->stop();
            CHECK(st("pluto.phy:voltage0.filter_fir_en") == 0, "Pluto: the FIR left on at stop");
            CHECK(st("pluto.phy.xo_correction") == 40000000, "Pluto: reference clock not restored at stop (%.0f)", st("pluto.phy.xo_correction"));
        }
        // over a network every refill is a round trip to iiod: ~25 ms per buffer, 64k to 1M samples (#114)
        DeviceInfo nd = d; nd.nativeArgs = "ip:10.0.0.7";
        auto net = makeSource(nd);
        t = TuneSettings(); t.centerHz = 1291e6; t.sampleRate = 16e6; t.gainDb = 30;
        e.clear();
        if (net->start(t, ring, e)) {
            CHECK(st("pluto.bufSamples") == 524288, "Pluto on Ethernet, 16 Msps: buffer of %.0f samples", st("pluto.bufSamples"));
            t.sampleRate = 30e6;
            CHECK(net->retune(t, e) && st("pluto.bufSamples") == 1048576, "Pluto on Ethernet, 30 Msps: buffer of %.0f samples", st("pluto.bufSamples"));
            t.sampleRate = 2.5e6;
            CHECK(net->retune(t, e) && st("pluto.bufSamples") == 65536, "Pluto on Ethernet, 2.5 Msps: buffer of %.0f samples", st("pluto.bufSamples"));
            net->stop();
        } else CHECK(false, "Pluto on Ethernet did not start (%s)", e.c_str());
    }

    if (use(lib("bladeRF").c_str())) {
        const DeviceInfo d = first("bladerf");
        CHECK(hasSetting(d, "gainmode") && !hasSetting(d, "xb200"), "bladeRF 2.0 settings");
        TuneSettings t;
        t.centerHz = 522e6; t.sampleRate = 10e6; t.gainDb = 30; t.bandwidthMhz = 8;
        auto src = makeSource(d);
        e.clear();
        bool ok = src->start(t, ring, e);
        CHECK(ok && st("blade.gainmode") == 1 && st("blade.xb") == kNone && st("blade.freq") == 522e6, "bladeRF defaults: gain mode %.0f xb %.0f freq %.0f", st("blade.gainmode"), st("blade.xb"), st("blade.freq"));
        if (ok) {
            t.radio["gainmode"] = "fast"; t.radio["ppm"] = "5";
            CHECK(src->retune(t, e) && st("blade.gainmode") == 2 && st("blade.freq") == std::llround(522e6 / 1.000005), "bladeRF fast attack, 5 ppm: mode %.0f freq %.0f", st("blade.gainmode"), st("blade.freq"));
            src->stop();
        }
        src.reset();
        // a bladeRF 1 with the XB-200: attached at open, then 100 MHz is in range
        set("blade.v1", 1);
        const DeviceInfo d1 = first("bladerf");
        CHECK(hasSetting(d1, "xb200"), "bladeRF 1: no XB-200 setting");
        setenv("BLADERF_SEARCH_DIR", (std::string(dir) + "/fpga").c_str(), 1);
        t.radio = {{"xb200", "1"}}; t.centerHz = 100e6;
        auto x = makeSource(d1);
        e.clear();
        ok = x->start(t, ring, e);
        CHECK(ok && st("blade.xb") == 2 && st("blade.freq") == 100e6, "bladeRF 1 + XB-200 at 100 MHz: xb %.0f freq %.0f (%s)", st("blade.xb"), st("blade.freq"), e.c_str());
        if (ok) x->stop();
        unsetenv("BLADERF_SEARCH_DIR");
        set("blade.v1", 0); set("blade.xb", 0);
    }

    if (use(lib("LimeSuite").c_str())) {
        TuneSettings t;
        t.centerHz = 522e6; t.sampleRate = 10e6; t.gainDb = 30; t.radio = {{"ppm", "-3"}};
        auto src = makeSource(first("lime"));
        e.clear();
        const bool ok = src->start(t, ring, e);
        CHECK(ok && std::fabs(st("lime.freq") - 522e6 / (1 - 3e-6)) < 1e-3, "LimeSDR -3 ppm: freq %.3f", st("lime.freq"));
        if (ok) src->stop();
    }

    if (use(lib("uhd").c_str())) {
        const DeviceInfo d = first("usrp");
        CHECK(hasSetting(d, "clock"), "USRP: no reference clock setting");
        TuneSettings t;
        t.centerHz = 522e6; t.sampleRate = 10e6; t.gainDb = 30;
        auto src = makeSource(d);
        e.clear();
        bool ok = src->start(t, ring, e);
        CHECK(ok && st("usrp.clockCalls") == kNone && st("usrp.freq") == 522e6, "USRP defaults: clock calls %.0f", st("usrp.clockCalls"));
        if (ok) src->stop();
        src.reset();
        t.radio = {{"clock", "external"}, {"ppm", "1"}};
        auto ext = makeSource(d);
        e.clear();
        ok = ext->start(t, ring, e);
        CHECK(ok && st("usrp.clock") == 2 && st("usrp.clockMb") == 0 && std::fabs(st("usrp.freq") - 522e6 / 1.000001) < 1e-3, "USRP external reference, 1 ppm: clock %.0f freq %.3f", st("usrp.clock"), st("usrp.freq"));
        if (ok) ext->stop();
    }
    fakeState = nullptr;
}
// ---- end radio settings

// ---- bias-tee
// Antenna power: set at start when asked, changed on a live retune, and off at stop, for the radios whose library has the call.
static void biasTeeChecks(const char* dir, const char* ext) {
    printf("bias-tee:\n");
    auto find = [](const char* board) {
        std::string e;
        for (const auto& d : listNativeDevices(e)) if (d.board == board) return d;
        return DeviceInfo{};
    };
    struct Radio { const char* board; const char* file; const char* key; };
    const Radio radios[] = {{"rtlsdr", "librtlsdr", "rtl.bias"}, {"airspy", "libairspy", "airspy.bias"}, {"bladerf", "libbladeRF", "blade.bias"}, {"sdrplay", "libsdrplay_api", "sdrplay.bias"}};
    for (const auto& r : radios) {
        const DeviceInfo d = find(r.board);
        CHECK(d.hasBiasTee, "%s: hasBiasTee is false", r.board);
        const std::string file = std::string(dir) + "/" + r.file + (std::string(r.board) == "sdrplay" ? ".so" : ext);
        void* h = dlopen(file.c_str(), RTLD_NOW | RTLD_NOLOAD);
        fakeState = h ? (double (*)(const char*))dlsym(h, "fake_state") : nullptr;
        CHECK(fakeState != nullptr, "%s: the fake library is not loaded", r.board);
        auto src = makeSource(d);
        if (!src || !fakeState) continue;
        IqRing ring(1 << 22);
        TuneSettings t;
        t.centerHz = 522e6; t.sampleRate = 2.5e6; t.gainDb = 30; t.bandwidthMhz = 0.2;
        t.biasTee = true;
        std::string e;
        const bool ok = src->start(t, ring, e);
        CHECK(ok, "%s: start failed: %s", r.board, e.c_str());
        if (!ok) continue;
        CHECK(st(r.key) == 1, "%s: antenna power %.0f after a start with it on", r.board, st(r.key));
        t.biasTee = false;
        CHECK(src->retune(t, e), "%s: retune failed: %s", r.board, e.c_str());
        CHECK(st(r.key) == 0, "%s: antenna power %.0f after a retune with it off", r.board, st(r.key));
        t.biasTee = true;
        CHECK(src->retune(t, e), "%s: retune failed: %s", r.board, e.c_str());
        CHECK(st(r.key) == 1, "%s: antenna power %.0f after a retune with it on", r.board, st(r.key));
        if (std::string(r.board) == "bladerf") CHECK(st("blade.biasCh") == 0, "bladeRF: bias-tee set on channel %.0f (RX0 expected)", st("blade.biasCh"));
        if (std::string(r.board) == "sdrplay") {   // the update that carried it names the RSP1A/1B flag
            CHECK(((unsigned)st("sdrplay.reasons") & 0x00000010) != 0, "SDRplay: update reasons 0x%08x lack Rsp1a_BiasTControl", (unsigned)st("sdrplay.reasons"));
        }
        src->stop();
        CHECK(st(r.key) == 0, "%s: antenna power %.0f after stop (must be off)", r.board, st(r.key));
        // with it off at start the radio is not powered
        t.biasTee = false;
        auto again = makeSource(d);
        CHECK(again && again->start(t, ring, e), "%s: second start failed: %s", r.board, e.c_str());
        CHECK(st(r.key) == 0, "%s: antenna power %.0f after a start with it off", r.board, st(r.key));
        if (again) again->stop();
    }
    for (const char* b : {"lime", "pluto", "usrp", "airspyhf", "fobos"}) CHECK(!find(b).hasBiasTee, "%s: hasBiasTee is true (no antenna power on this radio)", b);
}
// libraries without the functions: no radio claims the option and a start with it asked for still works
static void biasTeeMissingChecks() {
    printf("bias-tee (library without it):\n");
    for (const char* b : {"rtlsdr", "airspy", "bladerf"}) {
        std::string e;
        DeviceInfo d;
        for (const auto& x : listNativeDevices(e)) if (x.board == b) d = x;
        CHECK(d.board == b, "%s not listed", b);
        CHECK(!d.hasBiasTee, "%s: hasBiasTee is true although the library lacks the call", b);
        auto src = makeSource(d);
        IqRing ring(1 << 22);
        TuneSettings t;
        t.centerHz = 522e6; t.sampleRate = 2.5e6; t.gainDb = 30; t.bandwidthMhz = 0.2; t.biasTee = true;
        CHECK(src && src->start(t, ring, e), "%s: start with the bias-tee asked failed: %s", b, e.c_str());
        if (src) src->stop();
    }
}
// ---- end bias-tee


// ---- fobos: the Fobos SDR with the agile firmware (source_fobos.cpp) against the fake libfobos_sdr: the listing, the RF input, the gain split,
// the HF inputs with the driver's own frequency shift and decimation, and the ways the library's blocking stream can be stopped
static void fobosChecks(const char* dir, const char* ext) {
    printf("fobos:\n");
    void* h = dlopen((std::string(dir) + "/libfobos_sdr" + ext).c_str(), RTLD_NOW | RTLD_NOLOAD);
    fakeState = h ? (double (*)(const char*))dlsym(h, "fake_state") : nullptr;
    auto set = h ? (void (*)(const char*, double))dlsym(h, "fake_set") : nullptr;
    CHECK(fakeState && set, "the fake library of the Fobos SDR is not loaded");
    if (!fakeState || !set) return;
    const double kNone = -1e300;
    auto find = [&] {
        std::string e;
        DeviceInfo d;
        for (const auto& x : listNativeDevices(e)) if (x.board == "fobos") d = x;
        return d;
    };
    auto hazards = [&](const char* what) {
        for (const char* k : {"fobos.rateWhileStreaming", "fobos.directWhileStreaming", "fobos.closeWhileStreaming", "fobos.deadCalls", "fobos.openTwice"})
            CHECK(st(k) == kNone, "%s: %s = %.0f", what, k, st(k));
    };
    // the frequency and the rms amplitude of what the radio delivers (a tone), read at the rate it runs at
    auto tone = [&](IqRing& ring, double rate, double& amp) {
        std::vector<cf32> buf(1 << 14), got;
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        while (ring.read(buf.data(), buf.size())) {}   // what an earlier setting left behind
        const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(400);
        while (std::chrono::steady_clock::now() < end && got.size() < 8192) {
            const size_t n = ring.read(buf.data(), std::min(buf.size(), (size_t)8192 - got.size()));
            got.insert(got.end(), buf.begin(), buf.begin() + (long)n);
            if (!n) std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        if (got.size() < 1024) { amp = 0; return -1e300; }
        std::complex<double> a = 0;
        double p = 0;
        for (size_t i = 0; i + 1 < got.size(); i++) { a += std::complex<double>(got[i + 1]) * std::conj(std::complex<double>(got[i])); p += std::norm(std::complex<double>(got[i])); }
        amp = std::sqrt(p / (double)(got.size() - 1));
        return std::arg(a) * rate / (2 * M_PI);
    };

    // the listing: opened once to read the rates, then not again
    DeviceInfo dev = find();
    CHECK(dev.board == "fobos", "radio not listed");
    if (dev.board != "fobos") return;
    CHECK(dev.kind == DeviceInfo::Native && dev.isRadio() && dev.isGeneric(), "wrong kind");
    CHECK(dev.name == "Fobos SDR FOBOS0001 (agile, experimental)" && dev.serial == "FOBOS0001", "name/serial: %s / %s", dev.name.c_str(), dev.serial.c_str());
    CHECK(dev.minRateHz == 8e6 && dev.maxRateHz == 50e6 && dev.rateRanges.size() == 9, "rates %.0f..%.0f, %zu listed", dev.minRateHz, dev.maxRateHz, dev.rateRanges.size());
    CHECK(dev.gainMinDb == 0 && dev.gainMaxDb == 95, "gain range %.0f..%.0f", dev.gainMinDb, dev.gainMaxDb);
    CHECK(dev.minFreqHz == 0.1e6 && dev.maxFreqHz == 6e9, "tuning range %.0f..%.0f", dev.minFreqHz, dev.maxFreqHz);
    CHECK(!dev.hasBiasTee, "hasBiasTee is true (no antenna power on this radio)");
    {
        std::set<std::string> keys;
        for (const auto& x : dev.settings) keys.insert(x.key);
        CHECK(keys == std::set<std::string>({"ppm", "hf", "hfadc", "lna", "clock"}), "settings: %zu listed", keys.size());
    }
    CHECK(st("fobos.open") == 0 && st("fobos.opens") == st("fobos.closes"), "a listing left the radio open (opens %.0f closes %.0f)", st("fobos.opens"), st("fobos.closes"));
    {
        const double opens = st("fobos.opens");
        (void)find();
        CHECK(st("fobos.opens") == opens && opens >= 1, "a rescan opened the radio again (%.0f, then %.0f)", opens, st("fobos.opens"));
    }

    IqRing ring(1 << 22);
    TuneSettings t;
    t.centerHz = 522e6; t.sampleRate = 10e6; t.gainDb = 30; t.bandwidthMhz = 8;
    std::string e;
    std::complex<float> mean(0, 0);
    double amp = 0;
    {
        auto src = makeSource(dev);
        CHECK(src != nullptr, "no source");
        if (!src) return;
        // the RF input
        const double streams0 = std::max(0.0, st("fobos.streams"));   // (the fake counts on from the checks before this one)
        bool ok = src->start(t, ring, e);
        CHECK(ok, "start failed: %s", e.c_str());
        if (!ok) return;
        CHECK(e.empty(), "a note for a rate and a frequency the radio has: %s", e.c_str());
        CHECK(src->realtimeHardware() && src->sampleRate() == 10e6 && st("fobos.rate") == 10e6, "rate %.0f, radio set to %.0f", src->sampleRate(), st("fobos.rate"));
        CHECK(st("fobos.freq") == 522e6 && st("fobos.openIndex") == 0, "freq %.0f, opened at position %.0f", st("fobos.freq"), st("fobos.openIndex"));
        CHECK(st("fobos.directCalls") == kNone && st("fobos.clockCalls") == kNone, "defaults: direct sampling calls %.0f, clock calls %.0f (none expected)", st("fobos.directCalls"), st("fobos.clockCalls"));
        CHECK(st("fobos.lna") == 2 && st("fobos.vga") == 7, "gain 30 dB: LNA step %.0f VGA step %.0f (2 = +16 dB and 7 = 14 dB expected)", st("fobos.lna"), st("fobos.vga"));
        CHECK(st("fobos.bufCount") == 16 && st("fobos.bufLength") == 65536, "stream buffers %.0f x %.0f", st("fobos.bufCount"), st("fobos.bufLength"));
        size_t n = drain(ring, 300, mean);
        // the library gives full scale as 0.25: the driver brings it to 1.0
        CHECK(n > 8000 && std::fabs(mean.real() - 0.5f) < 0.02f && std::fabs(mean.imag() + 0.25f) < 0.02f, "RF samples: %zu, mean %.3f %.3f (0.5 -0.25 expected)", n, mean.real(), mean.imag());
        // a rescan while it streams keeps the radio, with its serial (the library can no longer read it)
        {
            const DeviceInfo again = find();
            CHECK(again.board == "fobos" && again.serial == "FOBOS0001", "the radio left the list while streaming (serial %s)", again.serial.c_str());
        }
        // live: frequency, gain, correction, a fixed LNA step, the reference clock; the stream goes on
        t.centerHz = 474e6; t.gainDb = 40;
        CHECK(src->retune(t, e) && st("fobos.freq") == 474e6 && st("fobos.lna") == 3 && st("fobos.vga") == 4, "retune: freq %.0f, LNA %.0f VGA %.0f (3 and 4 expected for 40 dB)", st("fobos.freq"), st("fobos.lna"), st("fobos.vga"));
        t.gainDb = 200;
        CHECK(src->retune(t, e) && st("fobos.lna") == 3 && st("fobos.vga") == 31, "gain beyond the slider: LNA %.0f VGA %.0f", st("fobos.lna"), st("fobos.vga"));
        t.gainDb = 10;
        CHECK(src->retune(t, e) && st("fobos.lna") == 0 && st("fobos.vga") == 5, "gain 10 dB: LNA %.0f VGA %.0f (0 and 5 expected)", st("fobos.lna"), st("fobos.vga"));
        t.gainDb = 40; t.radio["lna"] = "0";
        CHECK(src->retune(t, e) && st("fobos.lna") == 0 && st("fobos.vga") == 20, "LNA fixed at 0 dB, gain 40: LNA %.0f VGA %.0f (0 and 20 expected)", st("fobos.lna"), st("fobos.vga"));
        t.radio["lna"] = "33";
        CHECK(src->retune(t, e) && st("fobos.lna") == 3 && st("fobos.vga") == 4, "LNA fixed at +33 dB, gain 40: LNA %.0f VGA %.0f", st("fobos.lna"), st("fobos.vga"));
        t.radio.erase("lna");
        t.radio["ppm"] = "10";
        CHECK(src->retune(t, e) && std::fabs(st("fobos.freq") - 474e6 / 1.00001) < 0.5, "10 ppm: radio tuned to %.1f", st("fobos.freq"));
        t.radio.erase("ppm");
        t.radio["clock"] = "external";
        CHECK(src->retune(t, e) && st("fobos.clock") == 1, "external clock: %.0f", st("fobos.clock"));
        t.radio.erase("clock");
        CHECK(src->retune(t, e) && st("fobos.clock") == 0 && st("fobos.freq") == 474e6, "back to the internal clock: %.0f", st("fobos.clock"));
        CHECK(st("fobos.streams") == streams0 + 1, "the stream was restarted for a retune (%.0f starts)", st("fobos.streams") - streams0);
        n = drain(ring, 150, mean);
        CHECK(n > 3000, "stream stopped after the retunes (%zu samples)", n);
        hazards("RF input");

        // the HF1 input: 7.1 MHz at 1 Msps. The fake puts a sine at 7.2 MHz on HF1 and one at 6.9 MHz on HF2, both at half scale.
        t.centerHz = 7.1e6; t.sampleRate = 1e6; t.bandwidthMhz = 0.2;
        CHECK(src->retune(t, e), "retune to HF failed: %s", e.c_str());
        CHECK(e.empty(), "a note at 7.1 MHz: %s", e.c_str());
        CHECK(st("fobos.direct") == 1 && st("fobos.rate") == 50e6 && src->sampleRate() == 1e6, "HF1: direct %.0f, converter %.0f, output %.0f", st("fobos.direct"), st("fobos.rate"), src->sampleRate());
        double f = tone(ring, 1e6, amp);
        CHECK(std::fabs(f - 100e3) < 200 && std::fabs(amp - 0.5) < 0.02, "HF1 at 7.1 MHz: tone at %.0f Hz, amplitude %.3f (+100000 and 0.5 expected)", f, amp);
        const double streams = st("fobos.streams");
        t.centerHz = 7.3e6;   // live, on the same input: only the driver's own frequency shift moves
        CHECK(src->retune(t, e), "retune within HF failed: %s", e.c_str());
        f = tone(ring, 1e6, amp);
        CHECK(std::fabs(f + 100e3) < 200 && std::fabs(amp - 0.5) < 0.02, "HF1 at 7.3 MHz: tone at %.0f Hz, amplitude %.3f (-100000 expected)", f, amp);
        CHECK(st("fobos.streams") == streams, "the stream was restarted for a retune on the same HF input");
        t.radio["ppm"] = "100";   // a clock 100 ppm fast: the shift is that much lower, the tone that much higher
        CHECK(src->retune(t, e), "retune failed: %s", e.c_str());
        f = tone(ring, 1e6, amp);
        CHECK(std::fabs(f - (7.2e6 - 7.3e6 / 1.0001)) < 200, "HF1 with 100 ppm: tone at %.0f Hz (%.0f expected)", f, 7.2e6 - 7.3e6 / 1.0001);
        t.radio.erase("ppm");
        // the other connector
        t.radio["hf"] = "hf2"; t.centerHz = 7.0e6;
        CHECK(src->retune(t, e), "switch to HF2 failed: %s", e.c_str());
        f = tone(ring, 1e6, amp);
        CHECK(std::fabs(f + 100e3) < 200 && std::fabs(amp - 0.5) < 0.02, "HF2 at 7.0 MHz: tone at %.0f Hz, amplitude %.3f (-100000 expected)", f, amp);
        // a slower converter, and a rate the decimation does not hit exactly (25 Msps / 12 for 2 Msps asked)
        t.radio["hfadc"] = "25"; t.sampleRate = 2e6;
        CHECK(src->retune(t, e), "slower converter failed: %s", e.c_str());
        CHECK(st("fobos.rate") == 25e6 && std::fabs(src->sampleRate() - 25e6 / 12) < 1, "25 Msps converter: %.0f, output %.1f", st("fobos.rate"), src->sampleRate());
        f = tone(ring, src->sampleRate(), amp);
        CHECK(std::fabs(f + 100e3) < 400 && std::fabs(amp - 0.5) < 0.02, "HF2 at 25 Msps: tone at %.0f Hz, amplitude %.3f", f, amp);
        t.radio.erase("hfadc");
        // a frequency above half the converter's rate is said so
        t.centerHz = 30e6;
        CHECK(src->retune(t, e) && e.find("weak or absent") != std::string::npos, "30 MHz on the HF input: %s", e.c_str());
        e.clear();
        // back to the RF input, live: the tuner again, at the smallest rate of the radio that is fast enough
        t.centerHz = 145e6;
        CHECK(src->retune(t, e), "back to RF failed: %s", e.c_str());
        CHECK(st("fobos.direct") == 0 && st("fobos.freq") == 145e6 && st("fobos.rate") == 8e6 && src->sampleRate() == 8e6, "RF again: direct %.0f freq %.0f rate %.0f", st("fobos.direct"), st("fobos.freq"), st("fobos.rate"));
        n = drain(ring, 100, mean);   // (what the HF input left in the ring)
        n = drain(ring, 200, mean);
        CHECK(n > 3000 && std::fabs(mean.real() - 0.5f) < 0.02f && std::fabs(mean.imag() + 0.25f) < 0.02f, "RF samples after HF: %zu, mean %.3f %.3f", n, mean.real(), mean.imag());
        // the HF inputs switched off: the tuner is asked for its lowest frequency, and the user is told
        t.radio["hf"] = "off"; t.centerHz = 7.1e6;
        CHECK(src->retune(t, e) && st("fobos.direct") == 0 && st("fobos.freq") == 7.1e6 && e.find("switched off") != std::string::npos, "HF off at 7.1 MHz: direct %.0f freq %.0f, note: %s", st("fobos.direct"), st("fobos.freq"), e.c_str());
        e.clear();
        t.radio.erase("hf");
        hazards("HF inputs");

        src->stop();
        const size_t left = ring.available();
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
        CHECK(ring.available() == left, "samples kept arriving after stop()");
        CHECK(st("fobos.streaming") == 0 && st("fobos.open") == 0 && st("fobos.opens") == st("fobos.closes"), "not stopped and closed (streaming %.0f open %.0f)", st("fobos.streaming"), st("fobos.open"));
        hazards("stop");
    }

    t = TuneSettings();
    t.centerHz = 522e6; t.sampleRate = 10e6; t.gainDb = 30; t.bandwidthMhz = 8;
    auto timedStop = [&](IqSource& s) {
        const auto t0 = std::chrono::steady_clock::now();
        s.stop();
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    };
    // a start at an HF frequency goes straight to the HF input (nothing set while streaming), and a rev. 4 radio runs its converter at 64 Msps
    {
        set("fobos.rev4", 1);
        set("fobos.tone1", 14.1e6);
        auto src = makeSource(dev);
        t.centerHz = 14.074e6; t.sampleRate = 250e3; t.bandwidthMhz = 0.003;
        const bool ok = src && src->start(t, ring, e);
        CHECK(ok, "start on HF failed: %s", e.c_str());
        if (ok) {
            CHECK(st("fobos.direct") == 1 && st("fobos.rate") == 64e6 && src->sampleRate() == 250e3, "direct %.0f, converter %.0f, output %.0f", st("fobos.direct"), st("fobos.rate"), src->sampleRate());
            const double f = tone(ring, 250e3, amp);
            CHECK(std::fabs(f - 26e3) < 100 && std::fabs(amp - 0.5) < 0.02, "20 m: tone at %.0f Hz, amplitude %.3f (+26000 and 0.5 expected)", f, amp);
            src->stop();
        }
        set("fobos.rev4", 0);
        set("fobos.tone1", 0);
        hazards("start on HF");
        t.centerHz = 522e6; t.sampleRate = 10e6; t.bandwidthMhz = 8;
    }
    // a stop while the library's stream is still starting: its cancel is lost then, and the driver has to ask again
    {
        set("fobos.startDelayMs", 300);
        auto src = makeSource(dev);
        const bool ok = src && src->start(t, ring, e);
        CHECK(ok, "start failed: %s", e.c_str());
        if (ok) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            const double secs = timedStop(*src);
            CHECK(secs < 2.0, "stop during the stream's start took %.1f s", secs);
            CHECK(st("fobos.streaming") == 0 && st("fobos.open") == 0, "left running (streaming %.0f open %.0f)", st("fobos.streaming"), st("fobos.open"));
        }
        set("fobos.startDelayMs", 0);
        hazards("stop while starting");
    }
    // a stream that delivers nothing still stops
    {
        set("fobos.stall", 1);
        auto src = makeSource(dev);
        const bool ok = src && src->start(t, ring, e);
        CHECK(ok, "start failed: %s", e.c_str());
        if (ok) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            const double secs = timedStop(*src);
            CHECK(secs < 2.0, "stop of a silent stream took %.1f s", secs);
            CHECK(st("fobos.streaming") == 0 && st("fobos.open") == 0, "left running (streaming %.0f open %.0f)", st("fobos.streaming"), st("fobos.open"));
        }
        set("fobos.stall", 0);
        hazards("stop of a silent stream");
    }
    // destroyed while streaming (quit)
    if (auto s = makeSource(dev)) {
        s->start(t, ring, e);
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        s.reset();
        CHECK(st("fobos.streaming") == 0 && st("fobos.open") == 0, "quit while running: left open");
        hazards("quit while running");
    }
    // unplugged while streaming: the stream ends, the radio is opened again when it is back, and the samples go on
    {
        auto src = makeSource(dev);
        const bool ok = src && src->start(t, ring, e);
        CHECK(ok, "start failed: %s", e.c_str());
        if (ok) {
            drain(ring, 100, mean);
            set("fobos.gone", 1);
            std::this_thread::sleep_for(std::chrono::milliseconds(400));
            CHECK(st("fobos.streaming") == 0 && st("fobos.open") == 0, "unplugged: still streaming %.0f / open %.0f", st("fobos.streaming"), st("fobos.open"));
            const size_t quiet = drain(ring, 100, mean);
            CHECK(quiet == 0 || ring.available() == 0, "samples while unplugged");
            set("fobos.gone", 0);
            size_t n = 0;
            for (int i = 0; i < 40 && n < 3000; i++) n += drain(ring, 100, mean);
            CHECK(n >= 3000, "no samples after the radio came back (%zu)", n);
            CHECK(st("fobos.freq") == 522e6 && st("fobos.rate") == 10e6, "reopened with freq %.0f rate %.0f", st("fobos.freq"), st("fobos.rate"));
            // unplugged, and stopped while it is away
            set("fobos.gone", 1);
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            const double secs = timedStop(*src);
            CHECK(secs < 2.5, "stop while the radio is away took %.1f s", secs);
            set("fobos.gone", 0);
        }
        CHECK(st("fobos.open") == 0 && st("fobos.opens") == st("fobos.closes"), "left open after the unplugs (opens %.0f closes %.0f)", st("fobos.opens"), st("fobos.closes"));
        hazards("unplug");
    }
    // another Fobos SDR in front of it in the library's list: the serial finds the radio at its new position
    {
        set("fobos.shift", 1);
        auto src = makeSource(dev);
        const bool ok = src && src->start(t, ring, e);
        CHECK(ok && st("fobos.openIndex") == 1, "second radio in front: opened position %.0f (%s)", st("fobos.openIndex"), e.c_str());
        if (src) src->stop();
        set("fobos.shift", 0);
    }
    // gone before the start: said so, and nothing is left open
    {
        set("fobos.gone", 1);
        auto src = makeSource(dev);
        e.clear();
        CHECK(src && !src->start(t, ring, e) && e.find("FOBOS0001 not found") != std::string::npos, "start without the radio: %s", e.c_str());
        set("fobos.gone", 0);
        CHECK(st("fobos.open") == 0 && st("fobos.opens") == st("fobos.closes"), "left open after a failed start");
    }
    hazards("end");
    fakeState = nullptr;
}
// ---- end fobos

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
    for (const char* stem : {"rtlsdr", "airspy", "bladeRF", "LimeSuite", "iio", "uhd", "airspyhf", "fobos_sdr"})
        if (!copyFile(fake, std::string(dir) + "/lib" + stem + ext)) { printf("cannot copy the fake library\n"); return 2; }
    // the SDRplay API installs libsdrplay_api.so.3 on macOS too (not .dylib); the driver looks for libsdrplay_api.so in the test folder
    if (!copyFile(fake, std::string(dir) + "/libsdrplay_api.so")) { printf("cannot copy the fake library\n"); return 2; }
    setenv("DECT2_NATIVE_LIBDIR", dir, 1);
    auto fileOf = [&](const std::string& board) {
        if (board == "sdrplay") return std::string("libsdrplay_api.so");
        const std::string stem = board == "bladerf" ? "bladeRF" : board == "lime" ? "LimeSuite" : board == "pluto" ? "iio" : board == "usrp" ? "uhd" : board == "fobos" ? "fobos_sdr" : board;
        return "lib" + stem + ext;
    };

    if (argc > 2 && std::string(argv[2]) == "--nobias") {   // the fake libraries lack the bias-tee functions
        biasTeeMissingChecks();
        printf("%d failure(s)\n", failures);
        return failures ? 1 : 0;
    }

    std::string err;
    const auto list = listNativeDevices(err);
    printf("native radios found: %zu\n", list.size());
    for (const auto& d : list) printf("  %-8s %s\n", d.board.c_str(), d.name.c_str());
    {   // a radio with several antenna inputs is listed once per input (the same board)
        std::set<std::string> boards;
        for (const auto& d : list) boards.insert(d.board);
        CHECK(boards.size() == 9, "expected 9 radios, found %zu", boards.size());
    }

    struct Case { const char* board; double wantRate; double wantMinRate; };
    const Case cases[] = {{"rtlsdr", 2.4e6, 0}, {"airspy", 10e6, 0}, {"bladerf", 10e6, 0}, {"lime", 10e6, 0}, {"pluto", 10e6, 0}, {"usrp", 10e6, 0}, {"sdrplay", 10e6, 0}, {"fobos", 10e6, 0}};
    for (const auto& c : cases) {
        printf("%s:\n", c.board);
        const DeviceInfo* dev = nullptr;
        for (const auto& d : list) if (d.board == c.board && !dev) dev = &d;   // the first entry: the input OnAir always used
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
            void* h = dlopen((std::string(dir) + "/" + fileOf(b)).c_str(), RTLD_NOW | RTLD_NOLOAD);
            fakeState = h ? (double (*)(const char*))dlsym(h, "fake_state") : nullptr;
            CHECK(fakeState != nullptr, "the fake library of %s is not loaded", c.board);
        }
        if (b == "rtlsdr") { CHECK(st("rtl.freq") == 522e6, "freq %.0f", st("rtl.freq")); CHECK(st("rtl.gain") == 270, "gain %.0f (the 30 dB request should pick the 27 dB step)", st("rtl.gain")); CHECK(st("rtl.gainmode") == 1, "not manual gain"); }
        if (b == "airspy") { CHECK(st("airspy.freq") == 522e6, "freq"); CHECK(st("airspy.rate") == 10e6, "rate %.0f", st("airspy.rate")); CHECK(st("airspy.gain") == 21, "gain %.0f (clipped to the 0..21 steps)", st("airspy.gain")); CHECK(st("airspy.type") == 0, "not float32 I/Q"); }
        if (b == "bladerf") { CHECK(st("blade.freq") == 522e6, "freq"); CHECK(st("blade.rate") == 10e6, "rate"); CHECK(st("blade.gain") == 30, "gain"); CHECK(st("blade.gainmode") == 1, "not manual"); CHECK(st("blade.enabled") == 1, "not enabled"); CHECK(st("blade.format") == 0, "format"); CHECK(st("blade.openSerialGiven") == 1, "serial not passed"); }
        if (b == "lime") { CHECK(st("lime.freq") == 522e6, "freq"); CHECK(st("lime.rate") == 10e6, "rate"); CHECK(st("lime.gain") == 30, "gain"); CHECK(st("lime.antenna") == 2, "antenna index %.0f (LNAL expected for 522 MHz)", st("lime.antenna")); CHECK(st("lime.calibrated") == 1, "not calibrated"); CHECK(st("lime.fmt") == 0, "not float32"); CHECK(st("lime.chanEnabled") == 1, "channel not enabled"); }
        if (b == "pluto") { CHECK(st("pluto.connected") == 1, "uri"); CHECK(st("pluto.phy:altvoltage0.frequency") == 522e6, "freq"); CHECK(st("pluto.phy:voltage0.sampling_frequency") == 10e6, "rate"); CHECK(st("pluto.phy:voltage0.hardwaregain") == 30, "gain"); CHECK(st("pluto.phy:voltage0.gain_control_mode=manual") == 1, "not manual"); }
        if (b == "usrp") { CHECK(st("usrp.freq") == 522e6, "freq"); CHECK(st("usrp.rate") == 10e6, "rate"); CHECK(st("usrp.gain") == 30, "gain"); CHECK(st("usrp.cpuFmtFc32") == 1, "not fc32"); CHECK(st("usrp.cmd") == 97, "stream not started"); }
        if (b == "sdrplay") {
            CHECK(dev->name.find("RSP1B") != std::string::npos && dev->serial == "2305012345", "name/serial: %s", dev->name.c_str());
            CHECK(dev->gainMaxDb == 103 && dev->maxRateHz == 10e6 && dev->minRateHz == 2e6, "ranges: gain to %.0f, rate %.0f..%.0f", dev->gainMaxDb, dev->minRateHz, dev->maxRateHz);
            CHECK(st("sdrplay.listLocked") == 1, "the device list was not read under LockDeviceApi");
            CHECK(st("sdrplay.selected") == 1, "not selected");
            CHECK(st("sdrplay.fs") == 10e6, "fsHz %.0f", st("sdrplay.fs"));
            CHECK(st("sdrplay.rf") == 522e6, "rfHz %.0f", st("sdrplay.rf"));
            CHECK(st("sdrplay.bw") == 8000, "IF bandwidth %.0f kHz (8000 expected for an 8 MHz channel)", st("sdrplay.bw"));
            CHECK(st("sdrplay.if") == 0, "not zero IF");
            CHECK(st("sdrplay.decim") == 0, "decimation %.0f at 10 Msps", st("sdrplay.decim"));
            CHECK(st("sdrplay.agc") == 0, "the API's AGC is on");
            CHECK(st("sdrplay.dc") == 1 && st("sdrplay.iq") == 1, "DC/IQ correction off");
            // RSP1B, 420-1000 MHz LNA table {0 7 13 19 20 27 33 39 45 64}: 30 of 103 dB = 93 dB reduction = LNAstate 8 (45 dB) + gRdB 48
            CHECK(st("sdrplay.lna") == 8 && st("sdrplay.grdb") == 48, "gain 30 dB: LNAstate %.0f gRdB %.0f (8 and 48 expected)", st("sdrplay.lna"), st("sdrplay.grdb"));
            CHECK(st("sdrplay.overloadAcks") >= 1, "the power overload report was not acknowledged");
            std::string e2;   // a rescan while streaming keeps the radio in the list (the API no longer lists a selected one)
            bool still = false;
            for (const auto& d : listNativeDevices(e2)) if (d.board == "sdrplay" && d.serial == "2305012345") still = true;
            CHECK(still, "the radio left the list while streaming");
        }

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
        if (b == "fobos") { CHECK(st("fobos.freq") == 474e6, "retune freq"); CHECK(st("fobos.lna") == 3 && st("fobos.vga") == 4, "retune gain: LNA %.0f VGA %.0f", st("fobos.lna"), st("fobos.vga")); }
        if (b == "sdrplay") {   // one live update: frequency and gain (40 of 103 dB = 83 dB reduction = LNAstate 6 (33 dB) + gRdB 50)
            const unsigned why = (unsigned)st("sdrplay.reasons");
            CHECK((why & 0x00020000) && (why & 0x00008000), "update reasons 0x%08x (Tuner_Frf and Tuner_Gr expected)", why);
            CHECK(st("sdrplay.updTuner") == 1, "update not for tuner A");
            CHECK(st("sdrplay.rf") == 474e6, "retune rfHz %.0f", st("sdrplay.rf"));
            CHECK(st("sdrplay.lna") == 6 && st("sdrplay.grdb") == 50, "gain 40 dB: LNAstate %.0f gRdB %.0f (6 and 50 expected)", st("sdrplay.lna"), st("sdrplay.grdb"));
            CHECK(st("sdrplay.inits") == 1, "the stream was restarted for a retune");
        }

        // any frequency: a very low (9 MHz) and a very high (6 GHz) one reach each driver unchanged, with no error and no clamp (a note at most)
        {
            auto has = [](const std::string& s, const char* what) { return s.find(what) != std::string::npos; };
            const char* fkey = b == "rtlsdr" ? "rtl.freq" : b == "airspy" ? "airspy.freq" : b == "bladerf" ? "blade.freq" : b == "lime" ? "lime.freq" : b == "pluto" ? "pluto.phy:altvoltage0.frequency" : b == "usrp" ? "usrp.freq" : b == "sdrplay" ? "sdrplay.rf" : "fobos.freq";
            for (double fq : {9e6, 6e9}) {
                if (b == "fobos" && fq < 49.5e6) continue;   // the Fobos HF inputs (a mixer below 49.5 MHz) have their own checks further down
                t.centerHz = fq; e.clear();
                const bool okf = src->retune(t, e);
                // a 32-bit Hz call (RTL-SDR, Airspy) holds up to 4.29 GHz: the one thing cut, and said; the Fobos takes HF below 49.5 MHz through its own mixer
                const double want = (b == "rtlsdr" || b == "airspy") && fq > 4.29e9 ? 4.29e9 : fq;
                const bool viaMixer = b == "fobos" && fq < 49.5e6;
                CHECK(okf && (viaMixer ? st("fobos.direct") == 1 : st(fkey) == want), "%s at %.0f Hz: ok %d, driver got %.0f (%s)", c.board, fq, (int)okf, st(fkey), e.c_str());
                CHECK(e.empty() || has(e, "weak or absent") || has(e, "4.29 GHz"), "%s at %.0f Hz: unexpected message: %s", c.board, fq, e.c_str());
                CHECK(drain(ring, 200, mean) > 1000, "%s: no stream at %.0f Hz", c.board, fq);
            }
            t.centerHz = 474e6; e.clear();
            CHECK(src->retune(t, e), "%s: back to 474 MHz failed: %s", c.board, e.c_str());
            drain(ring, 100, mean);
        }

        // a different sample rate while running
        t.sampleRate = 2.5e6;
        CHECK(src->retune(t, e) || !e.empty(), "rate change failed");
        const size_t n3 = drain(ring, 200, mean);
        CHECK(n3 > 1000, "stream stopped after a rate change (%zu samples)", n3);
        if (b == "airspy") CHECK(st("airspy.rate") == 2.5e6, "airspy rate %.0f (the 2.5 Msps step expected)", st("airspy.rate"));
        if (b == "bladerf") CHECK(st("blade.rate") == 2.5e6, "rate");
        if (b == "usrp") CHECK(st("usrp.rate") == 2.5e6, "rate");
        if (b == "fobos") CHECK(st("fobos.rate") == 8e6 && src->sampleRate() == 8e6, "fobos rate %.0f (the 8 Msps step, its slowest, expected)", st("fobos.rate"));
        if (b == "pluto") CHECK(st("pluto.phy:voltage0.sampling_frequency") == 2.5e6, "pluto rate %.0f", st("pluto.phy:voltage0.sampling_frequency"));
        if (b == "sdrplay") {   // restarted at the new rate; the 8 MHz channel no longer fits, so the widest filter below 2.5 Msps
            CHECK(st("sdrplay.fs") == 2.5e6 && st("sdrplay.decim") == 0, "fsHz %.0f decimation %.0f", st("sdrplay.fs"), st("sdrplay.decim"));
            CHECK(st("sdrplay.bw") == 1536, "IF bandwidth %.0f kHz at 2.5 Msps (1536 expected)", st("sdrplay.bw"));
            CHECK(st("sdrplay.inits") == 2 && st("sdrplay.rf") == 474e6, "restart: %.0f inits, rfHz %.0f", st("sdrplay.inits"), st("sdrplay.rf"));
        }

        src->stop();
        const size_t drained = ring.available();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        CHECK(ring.available() == drained, "samples kept arriving after stop()");
        if (b == "sdrplay") CHECK(st("sdrplay.init") == 0 && st("sdrplay.selected") == 0, "not stopped and released (init %.0f, selected %.0f)", st("sdrplay.init"), st("sdrplay.selected"));
        src.reset();
    }
    // ---- airspyhf: 768 kHz, the gain -> attenuation / preamp mapping, a rate change, a request above the fastest rate
    {
        printf("airspyhf:\n");
        const DeviceInfo* dev = nullptr;
        for (const auto& d : list) if (d.board == "airspyhf") dev = &d;
        CHECK(dev != nullptr, "radio not listed");
        void* h = dlopen((std::string(dir) + "/libairspyhf" + ext).c_str(), RTLD_NOW | RTLD_NOLOAD);
        fakeState = h ? (double (*)(const char*))dlsym(h, "fake_state") : nullptr;
        CHECK(fakeState != nullptr, "the fake library of airspyhf is not loaded");
        if (dev && fakeState) {
            CHECK(dev->kind == DeviceInfo::Native && dev->isRadio() && dev->isGeneric(), "wrong kind");
            CHECK(dev->name == "Airspy HF+ 3652A8D8C9E1F001" && dev->serial == "3652A8D8C9E1F001", "name/serial: %s / %s", dev->name.c_str(), dev->serial.c_str());
            CHECK(dev->minFreqHz == 9e3 && dev->maxFreqHz == 260e6, "tuning range %.0f..%.0f", dev->minFreqHz, dev->maxFreqHz);
            CHECK(dev->gainMinDb == 0 && dev->gainMaxDb == 48, "gain range %.0f..%.0f", dev->gainMinDb, dev->gainMaxDb);
            CHECK(dev->minRateHz == 192e3 && dev->maxRateHz == 912e3, "rates %.0f..%.0f (from the radio's list)", dev->minRateHz, dev->maxRateHz);
            CHECK(st("airspyhf.opens") >= 1 && st("airspyhf.closes") == st("airspyhf.opens") && st("airspyhf.open") == 0, "listing left the radio open or never read it (opens %.0f closes %.0f)", st("airspyhf.opens"), st("airspyhf.closes"));

            // the tone is at 1/8 of the sample rate: this reads the rate the radio really runs at
            auto toneHz = [&](IqRing& ring, double rate, double& amp) {
                std::vector<cf32> buf(1 << 14), got;
                while (ring.read(buf.data(), buf.size())) {}   // what the earlier rate left behind
                const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
                while (std::chrono::steady_clock::now() < end && got.size() < 4096) {
                    const size_t n = ring.read(buf.data(), buf.size());
                    got.insert(got.end(), buf.begin(), buf.begin() + n);
                    if (!n) std::this_thread::sleep_for(std::chrono::milliseconds(2));
                }
                if (got.size() < 64) return -1e300;
                std::complex<double> a = 0;
                double p = 0;
                for (size_t i = 0; i + 1 < got.size(); i++) { a += std::complex<double>(got[i + 1]) * std::conj(std::complex<double>(got[i])); p += std::norm(std::complex<double>(got[i])); }
                amp = std::sqrt(p / (got.size() - 1));
                return std::arg(a) * rate / (2 * M_PI);
            };

            auto src = makeSource(*dev);
            CHECK(src != nullptr, "no source");
            if (src) {
                IqRing ring(1 << 22);
                TuneSettings t;
                t.centerHz = 7.1e6; t.sampleRate = 768e3; t.gainDb = 30; t.bandwidthMhz = 0.02;
                std::string e;
                const bool ok = src->start(t, ring, e);
                CHECK(ok, "start failed: %s", e.c_str());
                if (ok) {
                    CHECK(src->realtimeHardware() && src->sampleRate() == 768e3, "rate %.0f", src->sampleRate());
                    CHECK(e.empty(), "a note for a rate the radio has: %s", e.c_str());
                    CHECK(st("airspyhf.serialOk") == 1, "the radio was not opened by its serial");
                    CHECK(st("airspyhf.rate") == 768e3 && st("airspyhf.freq") == 7.1e6, "rate %.0f freq %.0f", st("airspyhf.rate"), st("airspyhf.freq"));
                    CHECK(st("airspyhf.agc") == 0 && st("airspyhf.dsp") == 1, "hf agc %.0f lib dsp %.0f (0 and 1 expected)", st("airspyhf.agc"), st("airspyhf.dsp"));
                    CHECK(st("airspyhf.att") == 3 && st("airspyhf.lna") == 0, "gain 30 of 48: att step %.0f lna %.0f (3 = 18 dB, no preamp)", st("airspyhf.att"), st("airspyhf.lna"));
                    double amp = 0;
                    double f = toneHz(ring, 768e3, amp);
                    CHECK(std::fabs(f - 96e3) < 500 && std::fabs(amp - 0.5) < 0.02, "tone %.0f Hz amplitude %.3f (96000 Hz, 0.5 expected)", f, amp);

                    // live: frequency and gain; the top of the slider is no attenuation and the preamp
                    t.centerHz = 14.2e6; t.gainDb = 48;
                    CHECK(src->retune(t, e), "retune failed: %s", e.c_str());
                    CHECK(st("airspyhf.freq") == 14.2e6, "retune freq %.0f", st("airspyhf.freq"));
                    CHECK(st("airspyhf.att") == 0 && st("airspyhf.lna") == 1, "gain 48: att %.0f lna %.0f (0 and 1 expected)", st("airspyhf.att"), st("airspyhf.lna"));
                    t.gainDb = 40;
                    CHECK(src->retune(t, e), "retune failed: %s", e.c_str());
                    CHECK(st("airspyhf.att") == 1 && st("airspyhf.lna") == 0, "gain 40: att %.0f lna %.0f (1 and 0 expected)", st("airspyhf.att"), st("airspyhf.lna"));
                    t.gainDb = 0; t.centerHz = 1e6;
                    CHECK(src->retune(t, e), "retune failed: %s", e.c_str());
                    CHECK(st("airspyhf.att") == 8 && st("airspyhf.lna") == 0 && st("airspyhf.freq") == 1e6, "gain 0: att %.0f lna %.0f freq %.0f", st("airspyhf.att"), st("airspyhf.lna"), st("airspyhf.freq"));
                    t.centerHz = 1e3; e.clear();
                    CHECK(src->retune(t, e), "retune failed: %s", e.c_str());
                    CHECK(st("airspyhf.freq") == 1e3 && e.find("weak or absent") != std::string::npos, "1 kHz request: radio set to %.0f (1000 expected, not the 9 kHz limit), note: %s", st("airspyhf.freq"), e.c_str());
                    t.gainDb = 200; t.centerHz = 100e6;
                    CHECK(src->retune(t, e), "retune failed: %s", e.c_str());
                    CHECK(st("airspyhf.att") == 0 && st("airspyhf.lna") == 1, "gain beyond the slider: att %.0f lna %.0f", st("airspyhf.att"), st("airspyhf.lna"));
                    f = toneHz(ring, 768e3, amp);
                    CHECK(std::fabs(f - 96e3) < 500, "stream changed after a live retune: tone %.0f Hz", f);
                    CHECK(st("airspyhf.rateWhileStreaming") == -1e300, "the rate was set while streaming");

                    // a slower rate: the radio changes it only while stopped, so the stream restarts
                    t.sampleRate = 192e3;
                    CHECK(src->retune(t, e), "rate change failed: %s", e.c_str());
                    CHECK(st("airspyhf.rate") == 192e3 && src->sampleRate() == 192e3 && st("airspyhf.freq") == 100e6, "rate %.0f freq %.0f after the change", st("airspyhf.rate"), st("airspyhf.freq"));
                    f = toneHz(ring, 192e3, amp);
                    CHECK(std::fabs(f - 24e3) < 200, "tone %.0f Hz at 192 kHz (24000 expected)", f);
                    CHECK(st("airspyhf.rateWhileStreaming") == -1e300, "the rate was set while streaming");

                    // a TV channel does not fit in 912 kHz: refused, and the stream keeps running as it was
                    t.sampleRate = 10e6; t.bandwidthMhz = 8;
                    CHECK(!src->retune(t, e) && e == "Airspy HF+ gives at most 912 kHz; 10 MHz needs a faster radio", "retune to 10 Msps: \"%s\"", e.c_str());
                    f = toneHz(ring, 192e3, amp);
                    CHECK(std::fabs(f - 24e3) < 200 && src->sampleRate() == 192e3, "stream after the refused rate: tone %.0f Hz", f);

                    src->stop();
                    const size_t drained = ring.available();
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    CHECK(ring.available() == drained, "samples kept arriving after stop()");
                    CHECK(st("airspyhf.streaming") == 0 && st("airspyhf.open") == 0, "not stopped and closed (streaming %.0f open %.0f)", st("airspyhf.streaming"), st("airspyhf.open"));
                }
                src.reset();
                CHECK(st("airspyhf.opens") == st("airspyhf.closes"), "opens %.0f, closes %.0f", st("airspyhf.opens"), st("airspyhf.closes"));
            }

            // a request above the fastest rate for a wide channel fails at start, and nothing stays open
            if (auto s2 = makeSource(*dev)) {
                IqRing ring(1 << 20);
                TuneSettings t;
                t.centerHz = 522e6; t.sampleRate = 10e6; t.bandwidthMhz = 8; t.gainDb = 30;
                std::string e;
                CHECK(!s2->start(t, ring, e), "start at 10 Msps worked");
                CHECK(e == "Airspy HF+ gives at most 912 kHz; 10 MHz needs a faster radio", "error: \"%s\"", e.c_str());
                CHECK(st("airspyhf.open") == 0 && st("airspyhf.opens") == st("airspyhf.closes"), "the radio stayed open after the failed start");
            }
            // a narrow channel asked for at a high rate (what the app asks for DRM, FM, AIS) runs at the fastest rate, with a note
            if (auto s3 = makeSource(*dev)) {
                IqRing ring(1 << 22);
                TuneSettings t;
                t.centerHz = 9.5e6; t.sampleRate = 2e6; t.bandwidthMhz = 0.02; t.gainDb = 30;
                std::string e;
                const bool ok = s3->start(t, ring, e);
                CHECK(ok, "narrow channel at 2 Msps refused: %s", e.c_str());
                if (ok) {
                    CHECK(s3->sampleRate() == 912e3 && st("airspyhf.rate") == 912e3, "rate %.0f (912000 expected)", s3->sampleRate());
                    CHECK(!e.empty(), "no note about the lower rate");
                    s3->stop();
                }
                s3.reset();
                CHECK(st("airspyhf.opens") == st("airspyhf.closes"), "opens %.0f, closes %.0f", st("airspyhf.opens"), st("airspyhf.closes"));
            }
            // Stop in every order (crash report from 0.2.2: Windows, Stop pressed, libusb under libairspyhf). The fake counts what crashes the
            // real library: a close or start with cancelled transfers in flight, a call on a closed handle, a stop of a stopped stream, two
            // threads in the library at once, a listing opening the radio that streams.
            auto hfSet = h ? (void (*)(const char*, double))dlsym(h, "fake_set") : nullptr;
            CHECK(hfSet != nullptr, "fake_set missing in the airspyhf fake");
            if (hfSet) {
                auto hazards = [&](const char* what) {
                    for (const char* k : {"airspyhf.inFlight", "airspyhf.deadCalls", "airspyhf.extraStops", "airspyhf.overlap", "airspyhf.openTwice",
                                          "airspyhf.listWhileOpen"})
                        CHECK(st(k) == -1e300, "%s: %s = %.0f", what, k, st(k));
                    CHECK(st("airspyhf.open") == 0 && st("airspyhf.opens") == st("airspyhf.closes"), "%s: left open (opens %.0f closes %.0f)", what,
                          st("airspyhf.opens"), st("airspyhf.closes"));
                };
                TuneSettings t;
                t.centerHz = 7.1e6; t.sampleRate = 768e3; t.gainDb = 30; t.bandwidthMhz = 0.02;
                std::string e;
                IqRing ring(1 << 20);
                // stop at once after start, stop twice, then the destructor
                if (auto s = makeSource(*dev)) {
                    if (s->start(t, ring, e)) { s->stop(); s->stop(); }
                    s.reset();
                    hazards("stop right after start, twice");
                }
                // a rate change (the stream restarts), a listing while streaming, retunes racing the stop
                if (auto s = makeSource(*dev)) {
                    if (s->start(t, ring, e)) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(50));
                        TuneSettings t2 = t; t2.sampleRate = 192e3;
                        s->retune(t2, e);
                        std::string le;
                        bool listed = false;
                        for (const auto& d : listNativeDevices(le)) listed |= d.board == "airspyhf";
                        CHECK(listed, "the HF+ that streams is missing from a listing");
                        std::atomic<bool> go{true};
                        std::thread tuner([&] { TuneSettings t3 = t2; std::string e3; for (int i = 0; go && i < 200; i++) { t3.centerHz = 7e6 + i * 1e3; s->retune(t3, e3); } });
                        std::this_thread::sleep_for(std::chrono::milliseconds(30));
                        s->stop();
                        go = false;
                        tuner.join();
                    }
                    s.reset();
                    hazards("rate change, listing, retunes during stop");
                }
                // Stop pressed to change modes (issue #78, 0.2.4: Windows, HF+ Discovery): stopped from this (the UI) thread while the
                // library's callback thread delivers samples, with listings and retunes from other threads, then started again; no
                // sample reaches the ring once stop() has returned
                for (int round = 0; round < 6; round++) {
                    auto s = makeSource(*dev);
                    if (!s) break;
                    IqRing r2(1 << 20);
                    if (!s->start(t, r2, e)) { CHECK(false, "HF+ start %d: %s", round, e.c_str()); break; }
                    std::atomic<bool> go{true};
                    std::thread lister([&] { std::string le; while (go) { listNativeDevices(le); std::this_thread::sleep_for(std::chrono::milliseconds(3)); } });
                    std::thread tuner([&] { TuneSettings t3 = t; std::string e3; for (int i = 0; go; i++) { t3.centerHz = 7e6 + (i % 50) * 1e3; s->retune(t3, e3); } });
                    std::this_thread::sleep_for(std::chrono::milliseconds(10 + 15 * round));
                    s->stop();
                    const uint64_t after = r2.available() + r2.dropped();
                    std::this_thread::sleep_for(std::chrono::milliseconds(30));
                    CHECK(r2.available() + r2.dropped() == after, "HF+ round %d: samples arrived after stop() returned", round);
                    go = false;
                    lister.join();
                    tuner.join();
                    s->stop();
                    s.reset();
                    hazards("Stop to change modes, callbacks running");
                }
                // unplugged while streaming, Stop pressed while it is away (the driver is trying to open it again), then plugged in again and stopped
                if (auto s = makeSource(*dev)) {
                    if (s->start(t, ring, e)) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(30));
                        hfSet("airspyhf.unplug", 1);
                        std::this_thread::sleep_for(std::chrono::milliseconds(300));
                        hfSet("airspyhf.unplug", 0);
                        std::this_thread::sleep_for(std::chrono::milliseconds(1300));   // (reopened within a second)
                        s->stop();
                    }
                    s.reset();
                    hazards("unplug, replug, stop");
                }
                if (auto s = makeSource(*dev)) {
                    if (s->start(t, ring, e)) {
                        hfSet("airspyhf.unplug", 1);
                        std::this_thread::sleep_for(std::chrono::milliseconds(250));
                        s->stop();   // while it is away
                    }
                    hfSet("airspyhf.unplug", 0);
                    s.reset();
                    hazards("unplug, stop while away");
                }
                // destroyed while streaming (quit)
                if (auto s = makeSource(*dev)) {
                    s->start(t, ring, e);
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                    s.reset();
                    hazards("quit while running");
                }
            }
        }
        fakeState = nullptr;
    }
    fobosChecks(dir, ext);   // ---- fobos
    radiosChecks(dir, ext);   // ---- radios
    biasTeeChecks(dir, ext);   // ---- bias-tee
    radioSettingsChecks(dir, ext);   // ---- radio settings
    nativeRobustChecks(dir, ext, list);   // ---- native-robust
    // with the feature switched off nothing is listed
    setenv("DECT2_NO_NATIVE", "1", 1);
    (void)listNativeDevices(err);   // (the libraries are already loaded, the switch is read when they are first touched)
    printf(failures ? "FAILED (%d)\n" : "native radio drivers: all checks passed\n", failures);
    return failures ? 1 : 0;
}
