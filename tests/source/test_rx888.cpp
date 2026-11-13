// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The RX888 MkII driver (core/src/source/source_rx888.cpp) against a FAKE FX3: no radio is involved. What this checks is OnAir's own side: the USB
// requests in the order the SDDC driver sends them (written from renardspark/SDDC_Driver), the firmware upload to the boot loader and the
// wait for the radio to come back, discovery (a LimeSDR-USB shares 04b4:00f1), the clock / decimation / input choices, and that a known tone
// comes out of the real-to-complex conversion where it should (HF directly, VHF through the 4.57 MHz IF with the spectrum inverted).
// What it cannot say: whether a real RX888 MkII answers these requests as the fake does, or delivers its VHF IF where the fake puts it.
#include "../../core/src/source/native_common.h"
#include "../../core/src/source/native_realddc.h"
#include "../../core/src/source/rx888_usb.h"
#include "dect2/source/source.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <thread>
#include <unistd.h>
#include "dect2/common/endian.h"
#ifdef _WIN32
// Windows has no setenv / unsetenv: _putenv_s, where an empty value removes the variable
static int setenv(const char* n, const char* v, int) { return _putenv_s(n, v); }
static int unsetenv(const char* n) { return _putenv_s(n, ""); }
#endif

using namespace dect2;
using namespace dect2::native;

static int failures = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("  FAIL: "); printf(__VA_ARGS__); printf("\n"); failures++; } } while (0)

// ------------------------------------------------------------------------------------------------------------------ the fake FX3

struct Ev { bool in; uint8_t req; uint16_t val, idx; std::vector<uint8_t> data; };
constexpr uint8_t kMarkQueued = 0xFF;   // not a request: the fake's bulk transfers were queued

struct FakeUsb : Rx888Usb {
    struct Dev {
        uint16_t pid; int bus, addr; std::string man, prod, serial; bool super = true; uint8_t model = kModelRx888Mk2;
        int nameError = 0;         // sticky: list() reports it and open() fails with it
        int nameErrorsLeft = 0;   // list() reports NOT_SUPPORTED this many times, then the name reads
    };
    std::mutex mu;
    std::vector<Dev> devs;
    std::vector<Ev> log;                       // the requests of application-mode radios
    std::map<uint32_t, std::vector<uint8_t>> ram;   // what the boot loader was given, by address
    uint32_t entry = 0;
    int jumps = 0, opens = 0, lists = 0;
    int lastOpenBus = 0, lastOpenAddr = 0;
    int muteAfterJump = 0, muteLeft = 0;       // lists that return nothing once the firmware has jumped
    int nameErrorAfterJump = 0, nameErrorsAfterJump = 0;
    bool dropOnJump = false;                   // the chip leaves and does not come back
    int standbyPipeLeft = 0;                   // fail the next N TUNERSTDBY requests with PIPE, then succeed
    bool standbyPipeAlways = false;
    int standbyAttempts = 0;
    std::atomic<bool> gone{false};
    std::atomic<double> toneHz{7.2e6};
    std::atomic<uint32_t> adc{0}, gpio{0};
    int blockSamples = 65536;

    std::vector<Rx888UsbDevice> list() override {
        std::lock_guard<std::mutex> g(mu);
        lists++;
        std::vector<Rx888UsbDevice> v;
        if (gone) return v;
        if (muteLeft > 0) { muteLeft--; return v; }
        for (auto& d : devs) {
            Rx888UsbDevice u;
            u.vid = 0x04b4; u.pid = d.pid; u.bus = d.bus; u.address = d.addr; u.manufacturer = d.man; u.product = d.prod; u.serial = d.serial;
            if (!u.bootloader() && d.nameErrorsLeft > 0) { d.nameErrorsLeft--; u.nameError = -12; u.product.clear(); u.serial.clear(); }
            else if (!u.bootloader() && d.nameError) { u.nameError = d.nameError; u.product.clear(); u.serial.clear(); }
            else if (!u.bootloader() && !rx888ProductIsMk2(u.product)) continue;   // what the libusb listing does
            v.push_back(u);
        }
        return v;
    }
    std::unique_ptr<Rx888Link> open(const Rx888UsbDevice& d, int& code) override;
    void reset() {
        std::lock_guard<std::mutex> g(mu);
        devs.clear(); log.clear(); ram.clear(); entry = 0; jumps = 0; opens = 0; lists = 0; gone = false; adc = 0; gpio = 0; toneHz = 7.2e6;
        lastOpenBus = lastOpenAddr = 0; muteAfterJump = muteLeft = 0; nameErrorAfterJump = nameErrorsAfterJump = 0; dropOnJump = false;
        standbyPipeLeft = 0; standbyPipeAlways = false; standbyAttempts = 0;
    }
    Dev* find(int bus, int addr) { for (auto& d : devs) if (d.bus == bus && d.addr == addr) return &d; return nullptr; }
    size_t count(uint8_t req) { std::lock_guard<std::mutex> g(mu); size_t n = 0; for (const auto& e : log) n += e.req == req; return n; }
    size_t indexOf(uint8_t req, size_t from = 0) { std::lock_guard<std::mutex> g(mu); for (size_t i = from; i < log.size(); i++) if (log[i].req == req) return i; return (size_t)-1; }
    size_t size() { std::lock_guard<std::mutex> g(mu); return log.size(); }
    Ev at(size_t i) { std::lock_guard<std::mutex> g(mu); return log[i]; }
    // the value of the last SETARG with this index (-1: none), after log position `from`
    int lastArg(uint16_t arg, size_t from = 0) {
        std::lock_guard<std::mutex> g(mu);
        int v = -1;
        for (size_t i = from; i < log.size(); i++) if (log[i].req == kSetArg && log[i].idx == arg) v = log[i].val;
        return v;
    }
};

static uint64_t le(const Ev& e) { uint64_t v = 0; for (size_t i = 0; i < e.data.size() && i < 8; i++) v |= (uint64_t)e.data[i] << (8 * i); return v; }

struct FakeLink : Rx888Link {
    FakeLink(FakeUsb* f, int bus, int addr) : f_(f), bus_(bus), addr_(addr) {}
    int control(bool in, uint8_t req, uint16_t val, uint16_t idx, uint8_t* data, uint16_t len) override {
        std::lock_guard<std::mutex> g(f_->mu);
        if (f_->gone) return -4;
        FakeUsb::Dev* d = f_->find(bus_, addr_);
        if (!d) return -4;
        if (d->pid == kRx888PidBoot) {
            if (req != kEzUsbRw) return -9;
            const uint32_t addr = (uint32_t)val | ((uint32_t)idx << 16);
            if (!in && len == 0) {   // the jump: the boot loader drops off the bus and the application shows up under a new address
                f_->entry = addr; f_->jumps++;
                f_->muteLeft = f_->muteAfterJump;
                if (f_->dropOnJump) { f_->devs.clear(); return -1; }   // LIBUSB_ERROR_IO: the chip left and has not come back
                d->pid = kRx888PidApp; d->addr += 1; d->man = "sdr prototypes"; d->prod = "RX888mk2"; d->serial = "0123456789ABCDEF";
                d->nameError = f_->nameErrorAfterJump; d->nameErrorsLeft = f_->nameErrorsAfterJump;
                return 0;
            }
            if (!in) { f_->ram[addr].assign(data, data + len); return len; }
            // read back: whatever chunk contains the range
            for (auto& r : f_->ram) if (addr >= r.first && addr + len <= r.first + r.second.size()) { memcpy(data, r.second.data() + (addr - r.first), len); return len; }
            return -9;
        }
        if (req == kTunerStandby && !in) {
            f_->standbyAttempts++;
            if (f_->standbyPipeAlways) return -9;   // LIBUSB_ERROR_PIPE
            if (f_->standbyPipeLeft > 0) { f_->standbyPipeLeft--; return -9; }
        }
        Ev e{in, req, val, idx, std::vector<uint8_t>(data, data + (in ? 0 : len))};
        f_->log.push_back(e);
        if (req == kTestFx3 && in) { data[0] = d->model; data[1] = 2; data[2] = 2; data[3] = 0; return 4; }
        if (req == kStartAdc && len == 4) f_->adc = (uint32_t)le(e);
        if (req == kGpioFx3 && len == 4) f_->gpio = (uint32_t)le(e);
        return len;
    }
    bool superSpeed() const override { std::lock_guard<std::mutex> g(f_->mu); FakeUsb::Dev* d = f_->find(bus_, addr_); return d ? d->super : true; }
    void interrupt() override {}
    int stream(size_t bytes, int queued, const std::function<void(const uint8_t*, size_t)>& onData, const std::function<bool()>& keepGoing,
               const std::function<void()>& onStarted) override {
        (void)queued;
        { std::lock_guard<std::mutex> g(f_->mu); f_->log.push_back(Ev{false, kMarkQueued, 0, 0, {}}); }
        onStarted();
        std::vector<int16_t> blk(bytes / 2);
        double ph = 0;
        while (keepGoing()) {
            if (f_->gone) return -4;
            const double fs = f_->adc ? (double)f_->adc : 64e6, w = 2 * M_PI * f_->toneHz / fs;
            const bool rnd = (f_->gpio & kGpioRand) != 0;
            for (auto& s : blk) {
                int16_t v = (int16_t)std::lround(16384.0 * std::cos(ph));
                ph += w; if (ph > 2 * M_PI) ph -= 2 * M_PI;
                if (rnd && (v & 1)) v = (int16_t)(v ^ -2);   // the converter's randomiser (it is its own inverse)
                s = v;
            }
            onData(reinterpret_cast<const uint8_t*>(blk.data()), bytes);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return 0;
    }
    FakeUsb* f_;
    int bus_, addr_;
};

std::unique_ptr<Rx888Link> FakeUsb::open(const Rx888UsbDevice& d, int& code) {
    std::lock_guard<std::mutex> g(mu);
    opens++;
    code = 0;
    Dev* found = find(d.bus, d.address);
    if (gone || !found) { code = -4; return nullptr; }
    lastOpenBus = d.bus; lastOpenAddr = d.address;
    if (found->nameError || found->nameErrorsLeft) { code = found->nameError ? found->nameError : -12; return nullptr; }
    return std::make_unique<FakeLink>(this, d.bus, d.address);
}

// ------------------------------------------------------------------------------------------------------------------ helpers

static FakeUsb fake;

static void addApp(int bus = 1, int addr = 5, const char* serial = "0123456789ABCDEF", const char* prod = "RX888mk2") {
    fake.devs.push_back({kRx888PidApp, bus, addr, "sdr prototypes", prod, serial});
}

// the little-endian 32-bit words of an image
static void put32(std::vector<uint8_t>& v, uint32_t w) { dect2::endian::putLe32(v, w); }
static std::vector<uint8_t> makeImage(uint32_t words, uint32_t addr, uint32_t entryAddr, bool badSum = false) {
    std::vector<uint8_t> v = {'C', 'Y', 0x00, 0xB0};
    put32(v, words);
    put32(v, addr);
    uint32_t sum = 0;
    for (uint32_t i = 0; i < words; i++) { const uint32_t w = 0x01000193u * (i + 1) + 7; put32(v, w); sum += w; }
    put32(v, 0);
    put32(v, entryAddr);
    put32(v, badSum ? sum + 1 : sum);
    return v;
}
static std::string writeTemp(const std::vector<uint8_t>& v, const char* name) {
    const std::string p = (std::filesystem::temp_directory_path() / (std::string(name) + "_" + std::to_string(getpid()) + ".img")).string();
    std::ofstream(p, std::ios::binary).write(reinterpret_cast<const char*>(v.data()), (std::streamsize)v.size());
    return p;
}

static DeviceInfo findRx888(std::string* all = nullptr) {
    std::string err;
    const auto v = listNativeDevices(err);
    DeviceInfo r;
    for (const auto& d : v) if (d.board == "rx888") { r = d; break; }
    if (all) for (const auto& d : v) *all += d.board + " ";
    return r;
}

static void drainRing(IqRing& ring) { std::vector<cf32> b(1 << 16); while (ring.read(b.data(), b.size())) {} }
static std::vector<cf32> collect(IqRing& ring, size_t want, int timeoutMs) {
    std::vector<cf32> out, b(1 << 16);
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (out.size() < want && std::chrono::steady_clock::now() < end) {
        const size_t n = ring.read(b.data(), b.size());
        if (n) out.insert(out.end(), b.begin(), b.begin() + (std::ptrdiff_t)n);
        else std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return out;
}
// |mean of y[n] exp(-j 2 pi f n / fs)| over the samples after the first `skip`
static double toneLevel(const std::vector<cf32>& y, double fs, double f, size_t skip) {
    std::complex<double> a = 0;
    size_t n = 0;
    for (size_t i = skip; i < y.size(); i++, n++) a += std::complex<double>(y[i]) * std::polar(1.0, -2 * M_PI * f * (double)i / fs);
    return n ? std::abs(a) / (double)n : 0;
}
static void sleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

// the settled output after a change: drain, wait, take n samples
static std::vector<cf32> settled(IqRing& ring, double rate, size_t n = 30000) {
    sleepMs(400);
    drainRing(ring);
    return collect(ring, n, (int)(2000 + n / rate * 1000 * 4));
}

// ------------------------------------------------------------------------------------------------------------------ the tests

static void gainChecks() {
    printf("gains:\n");
    const Rx888Gains lo = rx888Gains(0, 0), hi = rx888Gains(90, 0), mid = rx888Gains(30, 12);
    CHECK(lo.ifIndex == 0 && lo.ad8340 == 1, "slider 0: HF IF index %d, AD8340 value %d (want 0 and 1)", lo.ifIndex, lo.ad8340);
    CHECK(hi.ifIndex == 126 && hi.ad8340 == (0x80 | 111), "slider 90: HF IF index %d, AD8340 value 0x%x (want 126 and 0xef)", hi.ifIndex, hi.ad8340);
    CHECK(std::fabs(rx888HfIfGainDb(0) - 20 * std::log10(0.059)) < 1e-9 && std::fabs(rx888HfIfGainDb(126) - 20 * std::log10(0.409 * 111)) < 1e-9, "the AD8340 table ends");
    CHECK(rx888Gains(0, 0).lna == 0 && rx888Gains(0, 0).vga == 0 && hi.lna == 28 && hi.vga > 10, "VHF steps at both ends: lna %d/%d vga %d/%d", lo.lna, hi.lna, lo.vga, hi.vga);
    CHECK(mid.dat31 == 24 && rx888Gains(0, 31.5).dat31 == 63 && rx888Gains(0, 99).dat31 == 63 && rx888Gains(0, -3).dat31 == 0, "HF attenuator steps of 0.5 dB: %d", mid.dat31);
    int prevIf = -1, prevLna = -1, prevVga = -1;
    bool mono = true;
    for (double g = 0; g <= 90; g += 0.5) {
        const Rx888Gains x = rx888Gains(g, 0);
        if (x.ifIndex < prevIf || x.lna < prevLna || x.vga < prevVga) mono = false;
        prevIf = x.ifIndex; prevLna = x.lna; prevVga = x.vga;
        const int b = x.ad8340;
        if ((b & 0x80) ? ((b & 0x7f) < 4 || (b & 0x7f) > 111) : (b < 1 || b > 19)) mono = false;
    }
    CHECK(mono, "the gain steps never go down as the slider goes up, and stay in the AD8340's range");
}

static void firmwareChecks() {
    printf("firmware image:\n");
    const std::vector<uint8_t> good = makeImage(300, 0x40000000, 0x40000100);
    CHECK(rx888FirmwareProblem(good).empty(), "a good image is refused: %s", rx888FirmwareProblem(good).c_str());
    CHECK(!rx888FirmwareProblem(makeImage(300, 0x40000000, 0x40000100, true)).empty(), "a wrong checksum is accepted");
    std::vector<uint8_t> t = good; t.resize(t.size() - 40);
    CHECK(!rx888FirmwareProblem(t).empty(), "a truncated image is accepted");
    t = good; t[0] = 'X';
    CHECK(!rx888FirmwareProblem(t).empty(), "no CY signature is accepted");
    t = good; t[3] = 0xB1;
    CHECK(!rx888FirmwareProblem(t).empty(), "a security image is accepted");
    t = good; t[4] = 0xFF; t[5] = 0xFF; t[6] = 0xFF; t[7] = 0x7F;   // a section length that runs far past the file
    CHECK(!rx888FirmwareProblem(t).empty(), "a section past the end is accepted");
    CHECK(!rx888FirmwareProblem({}).empty() && !rx888FirmwareProblem({'C', 'Y'}).empty(), "empty images are accepted");
}

static void discoveryChecks() {
    printf("discovery:\n");
    unsetenv("ONAIR_RX888_FIRMWARE");
    fake.reset();
    CHECK(rx888ProductIsMk2("RX888mk2") && !rx888ProductIsMk2("LimeSDR-USB") && !rx888ProductIsMk2("RX888") && !rx888ProductIsMk2("HF103") && !rx888ProductIsMk2(""), "which product strings are an RX888 MkII");
    rx888UseUsb(&fake);
    std::string all;
    CHECK(findRx888(&all).board.empty(), "a radio is listed with no FX3 attached");
    addApp(1, 5, "0123456789ABCDEF");
    addApp(2, 7, "LIME0001", "LimeSDR-USB");   // 04b4:00f1 as well: not ours
    DeviceInfo d = findRx888(&all);
    CHECK(d.board == "rx888" && d.kind == DeviceInfo::Native, "the RX888 is not listed (%s)", all.c_str());
    CHECK(d.serial == "0123456789ABCDEF" && d.nativeArgs == "1:5", "serial '%s' args '%s'", d.serial.c_str(), d.nativeArgs.c_str());
    CHECK(d.name.find("RX888 MkII") == 0 && d.name.find("89ABCDEF") != std::string::npos, "name '%s'", d.name.c_str());
    CHECK(d.minRateHz == 0.25e6 && d.maxRateHz == 64e6 && d.rateRanges.empty(), "rates %g .. %g", d.minRateHz, d.maxRateHz);
    CHECK(d.gainMaxDb == kRx888GainMaxDb && d.hasBiasTee && d.minFreqHz == 10e3 && d.maxFreqHz == 1.75e9, "gain, bias tee and tuning range");
    std::set<std::string> keys;
    for (const auto& s : d.settings) keys.insert(s.key);
    CHECK(keys.count("ppm") && keys.count("input") && keys.count("adc") && keys.count("hfatt") && keys.count("dither") && keys.count("rand") && keys.count("pga"), "settings: %zu", d.settings.size());
    int n = 0;
    std::string e;
    for (const auto& x : listNativeDevices(e)) n += x.board == "rx888";
    CHECK(n == 1, "%d RX888 entries for one RX888 and one LimeSDR-USB", n);
    // in the boot loader: listed, with what is missing in its name
    fake.reset();
    fake.devs.push_back({kRx888PidBoot, 3, 9, "", "", ""});
    setenv("ONAIR_RX888_NO_CODEC_SEARCH", "1", 1);   // an SDDC_FX3.img in the build's codecs folder must not hide the "no image" case
    d = findRx888();
    CHECK(d.board == "rx888" && d.name.find("needs SDDC_FX3.img") != std::string::npos && d.serial.empty(), "boot loader entry: '%s'", d.name.c_str());
    const std::string img = writeTemp(makeImage(64, 0x40000000, 0x40000010), "rx888_list");
    setenv("ONAIR_RX888_FIRMWARE", img.c_str(), 1);
    d = findRx888();
    CHECK(d.name.find("firmware is loaded on start") != std::string::npos, "boot loader entry with an image: '%s'", d.name.c_str());
    unsetenv("ONAIR_RX888_FIRMWARE");
    unlink(img.c_str());
}

static void firmwareUploadChecks() {
    printf("firmware upload and start:\n");
    TuneSettings s;
    s.centerHz = 7.0e6; s.sampleRate = 2e6;
    // no image anywhere: refused with a message the Fix-it card matches, and nothing written
    unsetenv("ONAIR_RX888_FIRMWARE");
    setenv("ONAIR_RX888_NO_CODEC_SEARCH", "1", 1);
    fake.reset();
    fake.devs.push_back({kRx888PidBoot, 3, 9, "", "", ""});
    {
        auto src = makeNativeSource(findRx888());
        IqRing ring(1 << 20);
        std::string err;
        CHECK(src && !src->start(s, ring, err), "started without a firmware image");
        CHECK(err.find("SDDC_FX3.img") != std::string::npos, "message: %s", err.c_str());
        CHECK(fake.ram.empty() && fake.jumps == 0, "something was written to the boot loader");
        CHECK(fake.opens == 0, "the radio was opened %d times without an image to load (retries of a final error)", fake.opens);
    }
    // a damaged image
    {
        const std::string bad = writeTemp(makeImage(64, 0x40000000, 0x40000010, true), "rx888_bad");
        setenv("ONAIR_RX888_FIRMWARE", bad.c_str(), 1);
        auto src = makeNativeSource(findRx888());
        IqRing ring(1 << 20);
        std::string err;
        CHECK(!src->start(s, ring, err) && err.find("SDDC_FX3.img") != std::string::npos && err.find("checksum") != std::string::npos, "damaged image: %s", err.c_str());
        CHECK(fake.ram.empty() && fake.jumps == 0, "a damaged image was written");
        unlink(bad.c_str());
    }
    // a good image: written in 4 KiB pieces (6000 bytes here: two), the jump, the radio comes back under a new address, streaming goes on
    {
        const std::vector<uint8_t> image = makeImage(1500, 0x40000000, 0x40000123);
        const std::string path = writeTemp(image, "rx888_good");
        setenv("ONAIR_RX888_FIRMWARE", path.c_str(), 1);
        auto src = makeNativeSource(findRx888());
        IqRing ring(1 << 22);
        std::string err;
        const auto t0 = std::chrono::steady_clock::now();
        const bool ok = src->start(s, ring, err);
        const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        CHECK(ok, "start with the image failed: %s", err.c_str());
        CHECK(fake.jumps == 1 && fake.entry == 0x40000123, "jumps %d entry 0x%x", fake.jumps, fake.entry);
        size_t bytes = 0;
        bool same = true;
        uint32_t expect = 0x40000000;
        for (const auto& r : fake.ram) { same = same && r.first == expect; expect += (uint32_t)r.second.size(); bytes += r.second.size(); }
        CHECK(fake.ram.size() == 2 && bytes == 6000 && same, "RAM: %zu pieces, %zu bytes, contiguous %d (want 2, 6000, 1)", fake.ram.size(), bytes, (int)same);
        std::vector<uint8_t> flat;
        for (const auto& r : fake.ram) flat.insert(flat.end(), r.second.begin(), r.second.end());
        CHECK(flat.size() == 6000 && memcmp(flat.data(), image.data() + 12, 6000) == 0, "the RAM does not hold the image's section");
        CHECK(fake.devs.size() == 1 && fake.devs[0].pid == kRx888PidApp, "the radio did not come back as 04b4:00f1");
        CHECK(fake.count(kStopFx3) >= 1 && fake.count(kStartAdc) == 1, "the application got no requests (stop %zu, adc %zu)", fake.count(kStopFx3), fake.count(kStartAdc));
        CHECK(secs < 3, "the upload and the wait took %.2f s (a radio that is already back is not held for the old half-second)", secs);
        const auto y = collect(ring, 20000, 3000);
        CHECK(y.size() >= 20000, "no samples after the upload (%zu)", y.size());
        src->stop();
        unlink(path.c_str());
        CHECK(fake.lastOpenAddr == 10, "the running radio was opened at %d (the boot loader was at 9)", fake.lastOpenAddr);
    }
    // gone for many polls after the jump, then back at a new address: the old bus address is not what gets opened
    {
        fake.reset();
        fake.devs.push_back({kRx888PidBoot, 3, 9, "", "", ""});
        fake.muteAfterJump = 40;
        rx888SetReenumPollMs(25);
        const std::string path = writeTemp(makeImage(64, 0x40000000, 0x40000010), "rx888_late");
        setenv("ONAIR_RX888_FIRMWARE", path.c_str(), 1);
        auto src = makeNativeSource(findRx888());
        IqRing ring(1 << 20);
        std::string err;
        CHECK(src->start(s, ring, err), "late re-enumeration: %s", err.c_str());
        CHECK(fake.jumps == 1 && fake.lastOpenAddr == 10 && fake.devs.size() == 1 && fake.devs[0].pid == kRx888PidApp && fake.devs[0].addr == 10,
              "opened %d:%d jumps %d pid %u addr %d", fake.lastOpenBus, fake.lastOpenAddr, fake.jumps, fake.devs.empty() ? 0 : fake.devs[0].pid, fake.devs.empty() ? 0 : fake.devs[0].addr);
        src->stop();
        unlink(path.c_str());
        rx888SetReenumPollMs(0);
    }
    // a few failed name reads (the device is back, Windows has not bound it yet), then the name arrives
    {
        fake.reset();
        fake.devs.push_back({kRx888PidBoot, 3, 9, "", "", ""});
        fake.nameErrorsAfterJump = 4;
        rx888SetReenumPollMs(20);
        const std::string path = writeTemp(makeImage(64, 0x40000000, 0x40000010), "rx888_name");
        setenv("ONAIR_RX888_FIRMWARE", path.c_str(), 1);
        auto src = makeNativeSource(findRx888());
        IqRing ring(1 << 20);
        std::string err;
        CHECK(src->start(s, ring, err), "name not ready at first: %s", err.c_str());
        CHECK(fake.lastOpenAddr == 10 && fake.jumps == 1, "opened %d jumps %d", fake.lastOpenAddr, fake.jumps);
        src->stop();
        unlink(path.c_str());
        rx888SetReenumPollMs(0);
    }
    // the chip leaves and never comes back: say so, and do not replace it with "not found" by retrying the boot loader address
    {
        fake.reset();
        fake.devs.push_back({kRx888PidBoot, 3, 9, "", "", ""});
        fake.dropOnJump = true;
        rx888SetReenumWaitMs(300);
        rx888SetReenumPollMs(50);
        const std::string path = writeTemp(makeImage(64, 0x40000000, 0x40000010), "rx888_gone");
        setenv("ONAIR_RX888_FIRMWARE", path.c_str(), 1);
        auto src = makeNativeSource(findRx888());
        IqRing ring(1 << 20);
        std::string err;
        CHECK(!src->start(s, ring, err) && err.find("the firmware was loaded") != std::string::npos, "gone: %s", err.c_str());
        CHECK(err.find("not found (unplugged") == std::string::npos, "the retry replaced the firmware error: %s", err.c_str());
        CHECK(fake.jumps == 1, "the upload ran again (%d)", fake.jumps);
        unlink(path.c_str());
        rx888SetReenumWaitMs(0);
        rx888SetReenumPollMs(0);
    }
    // back as 04b4:00f1, but opening it to read the name is refused (on Windows that is the Cypress driver, which libusb cannot use)
    {
        fake.reset();
        fake.devs.push_back({kRx888PidBoot, 3, 9, "", "", ""});
        fake.nameErrorAfterJump = -12;
        rx888SetReenumWaitMs(1500);
        rx888SetReenumPollMs(50);
        const std::string path = writeTemp(makeImage(64, 0x40000000, 0x40000010), "rx888_drv");
        setenv("ONAIR_RX888_FIRMWARE", path.c_str(), 1);
        auto src = makeNativeSource(findRx888());
        IqRing ring(1 << 20);
        std::string err;
        CHECK(!src->start(s, ring, err) && err.find("NOT_SUPPORTED") != std::string::npos, "driver: %s", err.c_str());
        CHECK(err.find("not found (unplugged") == std::string::npos, "reported unplugged: %s", err.c_str());
        if (kOnWindows) CHECK(err.find("Cypress driver") != std::string::npos && err.find("does not ship a driver") != std::string::npos, "Windows: %s", err.c_str());
        CHECK(fake.jumps == 1 && fake.lastOpenAddr == 10, "opened the new device %d, jumps %d", fake.lastOpenAddr, fake.jumps);
        CHECK(findRx888().board.empty(), "an unreadable 04b4:00f1 was listed as an RX888");
        unlink(path.c_str());
        rx888SetReenumWaitMs(0);
        rx888SetReenumPollMs(0);
    }
    unsetenv("ONAIR_RX888_FIRMWARE");
}

static void refusalChecks() {
    printf("refusals:\n");
    TuneSettings s;
    s.centerHz = 7.0e6; s.sampleRate = 2e6;
    IqRing ring(1 << 20);
    {   // USB 2
        fake.reset();
        addApp();
        fake.devs[0].super = false;
        auto src = makeNativeSource(findRx888());
        std::string err;
        CHECK(!src->start(s, ring, err) && err.find("on a USB 2 connection") != std::string::npos, "USB 2: %s", err.c_str());
        CHECK(fake.opens == 1, "a final error was retried (%d opens)", fake.opens);
    }
    {   // another SDDC board (model 3 = RX888 first edition)
        fake.reset();
        addApp();
        fake.devs[0].model = 3;
        auto src = makeNativeSource(findRx888());
        std::string err;
        CHECK(!src->start(s, ring, err) && err.find("RX888 MkII only") != std::string::npos, "model 3: %s", err.c_str());
    }
    {   // unplugged between the list and the start
        fake.reset();
        addApp();
        const DeviceInfo d = findRx888();
        fake.devs.clear();
        auto src = makeNativeSource(d);
        std::string err;
        CHECK(!src->start(s, ring, err) && err.find("not found (unplugged") != std::string::npos, "unplugged: %s", err.c_str());
    }
}

static void streamChecks() {
    printf("start, stream, retune, stop:\n");
    fake.reset();
    addApp();
    auto src = makeNativeSource(findRx888());
    IqRing ring(1 << 22);
    TuneSettings s;
    s.centerHz = 7.0e6; s.sampleRate = 2e6; s.gainDb = 30;
    std::string err;
    CHECK(src->start(s, ring, err), "start: %s", err.c_str());
    sleepMs(300);
    // the request order of the SDDC driver: STOPFX3, TESTFX3, then the clock, the HF input, gains; the stream starts after the transfers are queued
    const size_t stop0 = fake.indexOf(kStopFx3), test0 = fake.indexOf(kTestFx3), adc0 = fake.indexOf(kStartAdc), sby = fake.indexOf(kTunerStandby);
    const size_t queued = fake.indexOf(kMarkQueued), start = fake.indexOf(kStartFx3);
    CHECK(stop0 == 0 && test0 == 1 && sby != (size_t)-1 && sby > test0 && adc0 > sby,
          "order: stop %zu test %zu standby %zu adc %zu (SDDC: HF path before ADC)", stop0, test0, sby, adc0);
    CHECK(queued != (size_t)-1 && start != (size_t)-1 && queued < start, "STARTFX3 (%zu) must come after the transfers are queued (%zu)", start, queued);
    CHECK(le(fake.at(adc0)) == 64000000 && fake.at(adc0).data.size() == 4, "ADC clock %llu", (unsigned long long)le(fake.at(adc0)));
    CHECK(fake.count(kGpioFx3) == 0, "GPIO written for the default HF setup (%zu)", fake.count(kGpioFx3));
    const Rx888Gains g30 = rx888Gains(30, 0);
    CHECK(fake.lastArg(kArgDat31) == 0 && fake.lastArg(kArgAd8340) == g30.ad8340, "HF gains: attenuator %d IF %d (want 0, %d)", fake.lastArg(kArgDat31), fake.lastArg(kArgAd8340), g30.ad8340);
    CHECK(src->sampleRate() == 2e6, "sample rate %g", src->sampleRate());
    // the tone at 7.2 MHz seen from 7.0 MHz: +200 kHz, at its amplitude (0.5 of full scale), nothing at -200 kHz
    auto y = settled(ring, 2e6);
    CHECK(y.size() >= 30000, "samples: %zu", y.size());
    double up = toneLevel(y, 2e6, 200e3, 4000), down = toneLevel(y, 2e6, -200e3, 4000);
    CHECK(std::fabs(up - 0.5) < 0.03 && down < 0.01, "HF 7.0 MHz: tone at +200 kHz %.3f (want 0.5), at -200 kHz %.4f", up, down);
    // a live retune: no restart, no new clock
    const size_t adcCount = fake.count(kStartAdc);
    s.centerHz = 7.1e6;
    CHECK(src->retune(s, err), "retune: %s", err.c_str());
    y = settled(ring, 2e6);
    up = toneLevel(y, 2e6, 100e3, 4000);
    CHECK(std::fabs(up - 0.5) < 0.03 && toneLevel(y, 2e6, 200e3, 4000) < 0.01, "HF 7.1 MHz: tone at +100 kHz %.3f", up);
    CHECK(fake.count(kStartAdc) == adcCount && fake.count(kStartFx3) == 1, "a frequency change restarted the stream (clock %zu, starts %zu)", fake.count(kStartAdc), fake.count(kStartFx3));
    // a gain change: only the IF gain argument
    s.gainDb = 55; s.radio["hfatt"] = "6";
    const size_t before = fake.size();
    CHECK(src->retune(s, err), "retune gain: %s", err.c_str());
    const Rx888Gains g55 = rx888Gains(55, 6);
    CHECK(fake.lastArg(kArgDat31, before) == g55.dat31 && fake.lastArg(kArgAd8340, before) == g55.ad8340 && g55.dat31 == 12, "gain 55 dB, 6 dB attenuator: %d %d (want %d %d)", fake.lastArg(kArgDat31, before), fake.lastArg(kArgAd8340, before), g55.dat31, g55.ad8340);
    // the randomiser: GPIO bit, and the driver undoes it (the fake randomises what it sends)
    s.radio["rand"] = "1";
    CHECK(src->retune(s, err), "retune rand: %s", err.c_str());
    CHECK(fake.gpio == kGpioRand, "GPIO word 0x%x (want randomizer only)", fake.gpio.load());
    y = settled(ring, 2e6);
    up = toneLevel(y, 2e6, 100e3, 4000);
    CHECK(std::fabs(up - 0.5) < 0.03, "tone through the randomiser: %.3f", up);
    s.radio["rand"] = "0";
    CHECK(src->retune(s, err) && fake.gpio == 0, "randomiser off: GPIO 0x%x", fake.gpio.load());
    // VHF: the fake puts the 100 MHz channel's tone 150 kHz above the centre on a 4.57 MHz IF, inverted: at 4.57 MHz - 150 kHz
    fake.toneHz = 4.57e6 - 150e3;
    s.centerHz = 100e6; s.gainDb = 40;
    s.biasTee = true;
    const size_t v0 = fake.size();
    CHECK(src->retune(s, err), "retune to VHF: %s", err.c_str());
    {
        const size_t a = fake.indexOf(kTunerInit, v0), t = fake.indexOf(kTunerTune, v0), gp = fake.indexOf(kGpioFx3, v0);
        CHECK(a != (size_t)-1 && le(fake.at(a)) == 16000000, "R828D reference clock request missing or wrong");
        CHECK(t != (size_t)-1 && le(fake.at(t)) == 100000000 && fake.at(t).data.size() == 8 && a < t, "tuner frequency request missing or wrong");
        CHECK(gp != (size_t)-1 && le(fake.at(gp)) == (kGpioVhfEn | kGpioBiasVhf) && gp < a, "GPIO for VHF + bias tee: 0x%llx", gp != (size_t)-1 ? (unsigned long long)le(fake.at(gp)) : 0ULL);
        // the HF path is shut first: attenuator at its maximum, then the AD8340 at its low step, and 0x83 after the antenna switch
        const size_t d31 = fake.indexOf(kSetArg, v0);
        CHECK(d31 != (size_t)-1 && fake.at(d31).idx == kArgDat31 && fake.at(d31).val == 63, "the HF attenuator is not set to maximum first");
        CHECK(fake.lastArg(kArgAd8340, v0) == 0x83, "AD8340 final value %d (want 0x83)", fake.lastArg(kArgAd8340, v0));
        const Rx888Gains gv = rx888Gains(40, 6);
        CHECK(fake.lastArg(kArgR82xxLna, v0) == gv.lna && fake.lastArg(kArgR82xxVga, v0) == gv.vga, "VHF gains %d %d (want %d %d)", fake.lastArg(kArgR82xxLna, v0), fake.lastArg(kArgR82xxVga, v0), gv.lna, gv.vga);
    }
    y = settled(ring, 2e6);
    up = toneLevel(y, 2e6, 150e3, 4000);
    down = toneLevel(y, 2e6, -150e3, 4000);
    CHECK(std::fabs(up - 0.5) < 0.03 && down < 0.01, "VHF: tone at +150 kHz %.3f (want 0.5), at -150 kHz %.4f", up, down);
    CHECK(src->sampleRate() == 2e6 && fake.count(kStartFx3) == 1, "the input switch restarted the stream");
    // a different VHF frequency: only the tuner request
    s.centerHz = 433.92e6;
    const size_t t0 = fake.size();
    CHECK(src->retune(s, err) && fake.indexOf(kTunerTune, t0) != (size_t)-1 && fake.indexOf(kTunerInit, t0) == (size_t)-1, "a VHF retune re-initialised the tuner");
    CHECK(le(fake.at(fake.indexOf(kTunerTune, t0))) == 433920000, "tuned to %llu", (unsigned long long)le(fake.at(fake.indexOf(kTunerTune, t0))));
    // back to HF: standby, VHF off, the HF gains sent again
    s.centerHz = 7.1e6; s.biasTee = false; s.gainDb = 30; s.radio["hfatt"] = "0";
    fake.toneHz = 7.2e6;
    const size_t h0 = fake.size();
    CHECK(src->retune(s, err), "retune back to HF: %s", err.c_str());
    CHECK(fake.indexOf(kTunerStandby, h0) != (size_t)-1, "no tuner standby on the way back to HF");
    const size_t gp = fake.indexOf(kGpioFx3, h0);
    CHECK(gp != (size_t)-1 && le(fake.at(gp)) == 0, "GPIO not cleared on the way back to HF");
    CHECK(fake.lastArg(kArgDat31, h0) == 0 && fake.lastArg(kArgAd8340, h0) == g30.ad8340, "HF gains not sent again: %d %d", fake.lastArg(kArgDat31, h0), fake.lastArg(kArgAd8340, h0));
    y = settled(ring, 2e6);
    CHECK(std::fabs(toneLevel(y, 2e6, 100e3, 4000) - 0.5) < 0.03, "HF tone after the way back");
    // stop: the stream is stopped and the radio is left
    const size_t st0 = fake.count(kStopFx3);
    src->stop();
    CHECK(fake.count(kStopFx3) > st0, "no STOPFX3 at stop");
    CHECK(src->sampleRate() == 2e6, "rate after stop");
}

static void rateChecks() {
    printf("clock and decimation:\n");
    struct Case { double want; const char* adcMode; uint32_t adc; double out; };
    const Case cases[] = {
        {2e6, "auto", 64000000, 2e6},                          // 64 MHz / 32
        {2.048e6, "auto", 64000000, 64e6 / 31},                // 64 / 31 = 2.0645: never below what is asked for
        {20e6, "auto", 64000000, 64e6 / 3},                    // 21.33 Msps
        {18285714.2857, "auto", 64000000, 64e6 / 3},           // DVB-T2 8 MHz at twice its native rate
        {32e6, "auto", 64000000, 32e6},
        {40e6, "auto", 128000000, 128e6 / 3},                  // above 32 Msps the clock goes to 128: D = floor(128 / 40) = 3
        {2e6, "128", 128000000, 2e6},
        {2.048e6, "exact", 65536000, 2.048e6},                 // the clock follows the rate: 32 x 2.048
        {9142857.142857, "exact", 64000000, 64e6 / 7},         // rounds to the nearest hertz of the clock: 7 x 9.142857
        {60e6, "auto", 128000000, 64e6},                       // D = 2
    };
    for (const auto& c : cases) {
        fake.reset();
        addApp();
        auto src = makeNativeSource(findRx888());
        IqRing ring(1 << 22);
        TuneSettings s;
        s.centerHz = 7.0e6; s.sampleRate = c.want;
        s.radio["adc"] = c.adcMode;
        std::string err;
        const bool ok = src->start(s, ring, err);
        const double want = c.out;
        const double got = src->sampleRate();
        CHECK(ok, "start at %g (%s): %s", c.want, c.adcMode, err.c_str());
        CHECK(fake.adc == c.adc, "%g Hz '%s': ADC clock %u (want %u)", c.want, c.adcMode, fake.adc.load(), c.adc);
        CHECK(std::fabs(got - want) < 1.5 && got >= c.want * 0.999 - 1, "%g Hz '%s': output rate %.3f (want %.3f)", c.want, c.adcMode, got, want);
        src->stop();
    }
    // an ADC setting changed while it runs restarts the stream with the new clock
    fake.reset();
    addApp();
    auto src = makeNativeSource(findRx888());
    IqRing ring(1 << 22);
    TuneSettings s;
    s.centerHz = 7.0e6; s.sampleRate = 2e6;
    std::string err;
    CHECK(src->start(s, ring, err), "start: %s", err.c_str());
    sleepMs(200);
    s.radio["adc"] = "128";
    CHECK(src->retune(s, err), "retune the clock: %s", err.c_str());
    sleepMs(200);
    CHECK(fake.adc == 128000000 && fake.count(kStartFx3) == 2 && src->sampleRate() == 2e6, "clock 128: ADC %u starts %zu rate %g", fake.adc.load(), fake.count(kStartFx3), src->sampleRate());
    // a rate change too
    s.sampleRate = 4e6;
    CHECK(src->retune(s, err), "retune the rate: %s", err.c_str());
    sleepMs(200);
    CHECK(src->sampleRate() == 4e6 && fake.count(kStartFx3) == 3, "rate 4 Msps: %g, starts %zu", src->sampleRate(), fake.count(kStartFx3));
    src->stop();
    // forced inputs
    fake.reset();
    addApp();
    src = makeNativeSource(findRx888());
    s = TuneSettings();
    s.centerHz = 100e6; s.sampleRate = 2e6;
    s.radio["input"] = "hf";
    CHECK(src->start(s, ring, err) && fake.count(kTunerInit) == 0, "input 'hf' at 100 MHz used the tuner");
    CHECK(err.find("half the ADC clock") != std::string::npos, "no range note for 100 MHz on the HF input: '%s'", err.c_str());
    src->stop();
    // cold start on VHF: HF path and ADC before the R828D is brought up (SDDC open order)
    fake.reset();
    addApp();
    src = makeNativeSource(findRx888());
    s = TuneSettings();
    s.centerHz = 100e6; s.sampleRate = 2e6; s.gainDb = 40;
    fake.toneHz = 4.57e6 - 150e3;
    const size_t n0 = fake.size();
    CHECK(src->start(s, ring, err), "cold VHF start: %s", err.c_str());
    const size_t sby0 = fake.indexOf(kTunerStandby, n0), adc1 = fake.indexOf(kStartAdc, n0), init0 = fake.indexOf(kTunerInit, n0);
    CHECK(sby0 != (size_t)-1 && adc1 != (size_t)-1 && init0 != (size_t)-1 && sby0 < adc1 && adc1 < init0,
          "cold VHF order: standby %zu adc %zu tuner init %zu", sby0, adc1, init0);
    src->stop();
}

static void tunerStandbyUsbChecks() {
    printf("tuner standby USB:\n");
    TuneSettings s;
    s.centerHz = 7.0e6; s.sampleRate = 2e6;
    IqRing ring(1 << 20);
    std::string err;
    {   // WinUSB-style PIPE bursts then OK: retries must get through
        fake.reset();
        addApp();
        fake.standbyPipeLeft = 4;
        auto src = makeNativeSource(findRx888());
        CHECK(src->start(s, ring, err), "PIPE retries: %s", err.c_str());
        CHECK(fake.standbyAttempts >= 5 && fake.count(kTunerStandby) >= 1, "standby tries %d log %zu", fake.standbyAttempts, fake.count(kTunerStandby));
        src->stop();
    }
    {   // cold start (VHF tune): persistent PIPE is ignored so the R828D path can come up
        fake.reset();
        addApp();
        fake.standbyPipeAlways = true;
        s.centerHz = 144e6; s.gainDb = 40;
        fake.toneHz = 4.57e6 - 150e3;
        auto src = makeNativeSource(findRx888());
        CHECK(src->start(s, ring, err), "cold VHF with PIPE standby: %s", err.c_str());
        CHECK(fake.count(kTunerInit) == 1, "tuner init %zu", fake.count(kTunerInit));
        src->stop();
    }
    {   // warm HF after VHF: standby must not be skipped
        fake.reset();
        addApp();
        fake.standbyPipeAlways = true;
        s.centerHz = 100e6; s.sampleRate = 2e6; s.gainDb = 40;
        auto src = makeNativeSource(findRx888());
        CHECK(src->start(s, ring, err), "start VHF: %s", err.c_str());
        s.centerHz = 7.1e6; s.gainDb = 30;
        CHECK(!src->retune(s, err) && err.find("tuner standby") != std::string::npos && err.find("2.2") != std::string::npos,
              "HF after VHF must not skip PIPE standby: %s", err.c_str());
        src->stop();
    }
}

static void reconnectChecks() {
    printf("unplug and replug:\n");
    fake.reset();
    addApp();
    auto src = makeNativeSource(findRx888());
    IqRing ring(1 << 22);
    TuneSettings s;
    s.centerHz = 7.0e6; s.sampleRate = 2e6;
    std::string err;
    CHECK(src->start(s, ring, err), "start: %s", err.c_str());
    CHECK(collect(ring, 10000, 3000).size() >= 10000, "no samples before the unplug");
    fake.gone = true;
    sleepMs(1500);
    fake.gone = false;
    drainRing(ring);
    const size_t n = collect(ring, 20000, 8000).size();
    CHECK(n >= 20000, "no samples after the replug (%zu)", n);
    CHECK(fake.count(kStartFx3) >= 1, "no new stream after the replug");
    src->stop();
}

static void benchmark() {
    printf("cost of the real-to-complex conversion (RealDdc, this computer, one thread; not a pass/fail check):\n");
    struct B { double fs; int decim; };
    for (const B b : {B{64e6, 32}, B{64e6, 3}, B{128e6, 2}, B{128e6, 16}}) {
        RealDdc ddc;
        ddc.design(b.decim);
        ddc.tune(b.fs, 7e6, 1.0 / 32768);
        std::vector<int16_t> blk(65536);
        for (size_t i = 0; i < blk.size(); i++) blk[i] = (int16_t)std::lround(16384.0 * std::cos(2 * M_PI * 7.2e6 * (double)i / b.fs));
        std::vector<cf32> out;
        const size_t total = (size_t)(b.fs / 4);   // a quarter of a second of input
        const auto t0 = std::chrono::steady_clock::now();
        for (size_t done = 0; done < total; done += blk.size()) { out.clear(); ddc.processS16(blk.data(), blk.size(), false, out); }
        const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        printf("  %.0f Msps ADC, decimation %2d: %.2f s of CPU per second of signal\n", b.fs / 1e6, b.decim, secs * 4);
    }
}

int main() {
    gainChecks();
    firmwareChecks();
    discoveryChecks();
    firmwareUploadChecks();
    refusalChecks();
    streamChecks();
    rateChecks();
    tunerStandbyUsbChecks();
    reconnectChecks();
    benchmark();
    rx888UseUsb(nullptr);
    if (failures) { printf("test_rx888: %d FAILED\n", failures); return 1; }
    printf("test_rx888: all passed\n");
    return 0;
}
