// USB radios handed over as descriptors (core/include/dect2/source/usb_fd.h, core/src/source/usb_fd_io.h): the registry, the transfers on a FAKE usbfs
// (chunking of bulk reads, errno -> libusb codes, string descriptors) and, when the Rust USB layer is built, the RTL-SDR driver's table filled from
// the registry. No radio is involved: whether a real dongle answers through a real Android descriptor is not tested here.
#include "../../core/src/source/usb_fd_io.h"
#include "dect2/source/usb_fd.h"
#ifdef DECT2_RUST_USB
#include "../../core/src/source/rtl_fd_shim.h"
#endif
#include <cstdio>
#include <vector>

using namespace dect2;
static int failures = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

#include <chrono>
#include <thread>
static double rtlfd_now() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

struct FakeSys : UsbFdSys {
    std::vector<int> bulkLens;           // sizes asked
    int bulkFail = 0;                    // -errno for the call number failAt (1 based), 0 = none
    int failAt = 0, calls = 0, shortAt = 0;
    int claimRet = 0, claimed = 0, released = 0, detached = 0;
    uint8_t lastType = 0;
    int control(int, uint8_t type, uint8_t req, uint16_t val, uint16_t, void* data, uint16_t len, uint32_t) override {
        lastType = type;
        uint8_t* d = static_cast<uint8_t*>(data);
        if (req == 6 && (val >> 8) == 3) {                   // GET_DESCRIPTOR string
            if ((val & 0xff) == 0) { d[0] = 4; d[1] = 3; d[2] = 0x09; d[3] = 0x04; return 4; }
            const char* s = "Blog V4";
            int n = 2;
            for (const char* c = s; *c; c++) { d[n++] = (uint8_t)*c; d[n++] = 0; }
            d[0] = (uint8_t)n; d[1] = 3;
            return n;
        }
        if (req == 0x99) return -ETIMEDOUT;
        return len;
    }
    int bulk(int, uint8_t ep, void*, int len, uint32_t) override {
        bulkLens.push_back(len);
        calls++;
        if (ep != 0x81) return -EINVAL;
        if (failAt && calls == failAt) return -bulkFail;
        if (shortAt && calls == shortAt) return len / 2;
        return len;
    }
    int claim(int, unsigned) override { if (claimRet == 0) claimed++; return claimRet; }
    int release(int, unsigned) override { released++; return 0; }
    int resets = 0, clearHalts = 0;
    bool halted = false, stuck = false;   // an endpoint that halted (EPIPE) until clearHalt; stuck: it stays halted whatever is done
    double stallUntil = 0;                 // steady-clock seconds: streamRead times out until then
    int reset(int) override { resets++; return 0; }
    int clearHalt(int, uint8_t) override { clearHalts++; if (!stuck) halted = false; return 0; }
    int detachKernel(int, unsigned) override { detached++; return -ENODATA; }
    // a fake URB queue: a ring of buffers filled with a counter, ending with -streamEnd (an errno) after streamLimit buffers when set
    int streamLimit = 0, streamEnd = 0, streamStarts = 0, streamStops = 0, streamRecycled = 0, streamServed = 0, streamNoSys = 0, inQueue = 0;
    std::vector<std::vector<uint8_t>> bufs;
    int streamStart(int, uint8_t ep, int n, int len, void** h) override {
        if (streamNoSys) return -ENOSYS;
        if (ep != 0x81) return -EINVAL;
        streamStarts++; bufs.assign((size_t)n, std::vector<uint8_t>((size_t)len)); inQueue = n; *h = this; return 0;
    }
    int streamRead(void*, uint8_t** d, int* len, uint32_t) override {
        if (halted) return -EPIPE;
        if (stallUntil > 0 && rtlfd_now() < stallUntil) { std::this_thread::sleep_for(std::chrono::milliseconds(20)); return -ETIMEDOUT; }
        if (streamLimit && streamServed >= streamLimit) return streamEnd ? -streamEnd : -ETIMEDOUT;
        auto& b = bufs[(size_t)(streamServed % (int)bufs.size())];
        for (auto& x : b) x = (uint8_t)streamServed;
        streamServed++; *d = b.data(); *len = (int)b.size(); return 0;
    }
    int streamRecycle(void*, uint8_t*) override { streamRecycled++; return 0; }
    void streamStop(void*) override { streamStops++; }
};

int main() {
    // registry
    UsbFdRadio a; a.name = "/dev/bus/usb/001/004"; a.kind = "rtlsdr"; a.fd = 7; a.vid = 0x0bda; a.pid = 0x2838; a.product = "Blog V4";
    CHECK(usbFdAttach(a));
    UsbFdRadio bad = a; bad.fd = -1;
    CHECK(!usbFdAttach(bad));
    bad = a; bad.name = "";
    CHECK(!usbFdAttach(bad));
    UsbFdRadio h; h.name = "/dev/bus/usb/001/005"; h.kind = "hackrf"; h.fd = 8;
    CHECK(usbFdAttach(h));
    CHECK(usbFdList().size() == 2 && usbFdList("rtlsdr").size() == 1 && usbFdList("airspy").empty());
    a.fd = 9;
    CHECK(usbFdAttach(a) && usbFdList().size() == 2 && usbFdList("rtlsdr")[0].fd == 9);   // same name replaces
    usbFdDetach("/dev/bus/usb/001/005");
    CHECK(usbFdList().size() == 1);
    usbFdDetach("nothing");
    CHECK(usbFdList().size() == 1);
    CHECK(usbFdKindForId(0x0bda, 0x2838) == "rtlsdr" && usbFdKindForId(0x1d50, 0x6089) == "hackrf" && usbFdKindForId(0x1d50, 0x60a1) == "airspy" &&
          usbFdKindForId(0x03eb, 0x800c) == "airspyhf" && usbFdKindForId(0x1234, 1).empty());

    // errno mapping
    CHECK(usbErrnoToLibusb(ETIMEDOUT) == kUsbTimeout && usbErrnoToLibusb(ENODEV) == kUsbNoDevice && usbErrnoToLibusb(ESHUTDOWN) == kUsbNoDevice &&
          usbErrnoToLibusb(EPIPE) == kUsbPipe && usbErrnoToLibusb(EBUSY) == kUsbBusy && usbErrnoToLibusb(EACCES) == kUsbAccess && usbErrnoToLibusb(0) == 0);

    // transfers
    FakeSys sys;
    {
        UsbFdDevice d(sys, 7);
        CHECK(d.claimInterface(0) == 0 && sys.claimed == 1 && sys.detached == 1);   // a failed kernel detach is ignored
        uint8_t buf[70000];
        CHECK(d.bulkRead(0x01, buf, 40000, 100) == 40000);   // the endpoint's direction bit is set for the caller
        CHECK((sys.bulkLens == std::vector<int>{16384, 16384, 7232}));
        sys.bulkLens.clear(); sys.calls = 0; sys.shortAt = 2;
        CHECK(d.bulkRead(0x81, buf, 40000, 100) == 16384 + 8192);   // a short piece ends the read
        sys.shortAt = 0; sys.calls = 0; sys.failAt = 2; sys.bulkFail = ETIMEDOUT;
        CHECK(d.bulkRead(0x81, buf, 40000, 100) == 16384);          // bytes read before the error are kept
        sys.calls = 0; sys.failAt = 1; sys.bulkFail = ENODEV;
        CHECK(d.bulkRead(0x81, buf, 100, 100) == kUsbNoDevice);
        sys.failAt = 0;
        CHECK(d.controlIn(0x40, 1, 0, 0, buf, 4, 100) == 4 && (sys.lastType & 0x80));
        CHECK(d.controlOut(0xc0, 1, 0, 0, buf, 4, 100) == 4 && !(sys.lastType & 0x80));
        CHECK(d.controlIn(0x40, 0x99, 0, 0, buf, 4, 100) == kUsbTimeout);
        uint8_t s[32];
        CHECK(d.stringDescriptor(2, s, sizeof s) == 7 && std::string((char*)s) == "Blog V4");
        CHECK(d.stringDescriptor(0, s, sizeof s) == kUsbInvalid);
        const uint8_t wide[] = {8, 3, 'A', 0, 0x3a, 0x04, 'B', 0};   // a Cyrillic letter in the middle becomes '?'
        CHECK(UsbFdDevice::parseStringDescriptor(wide, 8, s, sizeof s) == 3 && std::string((char*)s) == "A?B");
        CHECK(UsbFdDevice::parseStringDescriptor(wide, 1, s, sizeof s) == kUsbIo);
    }
    CHECK(sys.released == 1);   // destructor releases the interface
    {
        FakeSys busy; busy.claimRet = -EBUSY;
        UsbFdDevice d(busy, 7);
        CHECK(d.claimInterface(0) == kUsbBusy);
    }

#ifdef DECT2_RUST_USB
    // the driver's table: enumeration from the registry, and a clean failure when the dongle does not speak the RTL2832U protocol
    {
        using namespace dect2::native;
        rtlfd::sysOverride() = &sys;
        char v[256], p[256], sn[256];
        CHECK(rtlfd::getDeviceCount() == 1 && rtlfd::getUsbStrings(0, v, p, sn) == 0 && std::string(p) == "Blog V4");
        CHECK(rtlfd::getUsbStrings(3, v, p, sn) < 0);
        sys.claimRet = -EBUSY;
        void* dev = nullptr;
        CHECK(rtlfd::open(&dev, 0) == kUsbBusy && dev == nullptr);
        sys.claimRet = 0;
        dev = nullptr;
        const int r = rtlfd::open(&dev, 0);   // the fake answers every control read with zeros: whatever comes of it, no crash
        printf("open on a silent fake: %d\n", r);
        if (r == 0 && dev) rtlfd::close(dev);
        CHECK(rtlfd::open(&dev, 5) < 0);

        // the sample stream: transfers kept in flight, blocks of at least 64 KiB handed to the callback, the loop ending on cancel / unplug
        struct Ctx { size_t bytes = 0, calls = 0, minCall = ~(size_t)0; rtlfd::Dev* dev; size_t stopAt; };
        Ctx cx; rtlfd::Dev dv(usbFdList("rtlsdr")[0]); cx.dev = &dv; cx.stopAt = 300000;
        auto cb = [](unsigned char*, uint32_t n, void* c) {
            auto* x = static_cast<Ctx*>(c);
            x->bytes += n; x->calls++;
            if (x->stopAt && x->bytes >= x->stopAt) rtlfd::cancelAsync(x->dev);
        };
        CHECK(rtlfd::readAsync(&dv, cb, &cx, 0, 0) == 0 && cx.bytes >= 300000 && cx.calls >= 1 && sys.streamStarts == 1 && sys.streamStops == 1);
        CHECK(sys.streamRecycled == sys.streamServed && cx.bytes / cx.calls >= 65536);   // every buffer given back; no tiny callbacks
        sys.streamLimit = sys.streamServed + 10; sys.streamEnd = ENODEV; cx.stopAt = 0;   // the radio is unplugged
        CHECK(rtlfd::readAsync(&dv, cb, &cx, 0, 0) == kUsbNoDevice && sys.streamStops == 2);
        sys.streamLimit = sys.streamServed; sys.streamEnd = 0; sys.streamNoSys = 1; sys.calls = 0; sys.failAt = 0;   // no queue available: block-by-block reads
        cx.stopAt = cx.bytes + 100000;
        CHECK(rtlfd::readAsync(&dv, cb, &cx, 0, 65536) == 0 && sys.streamStarts == 2 && sys.calls > 0);
        // 3.5: the stream recovers by itself (the Android freeze). A halted endpoint ("broken pipe") is cleared and the transfers restarted ...
        {
            sys.streamNoSys = 0; sys.streamLimit = 0; sys.streamEnd = 0; sys.halted = true; sys.stuck = false;
            const int starts0 = sys.streamStarts, clears0 = sys.clearHalts;
            cx.bytes = 0; cx.calls = 0; cx.stopAt = 200000;
            CHECK(rtlfd::readAsync(&dv, cb, &cx, 0, 0) == 0 && cx.bytes >= 200000);
            CHECK(sys.clearHalts == clears0 + 1 && sys.streamStarts == starts0 + 2 && !sys.halted);   // one clear, one queue restart (plus the first start)
        }
        // ... an endpoint that stays halted climbs the ladder (clear, clear, reset) and then gives the radio up for a reopen with the broken-pipe code
        {
            sys.halted = true; sys.stuck = true;
            const int resets0 = sys.resets, clears0 = sys.clearHalts;
            cx.bytes = 0; cx.calls = 0; cx.stopAt = 0;
            CHECK(rtlfd::readAsync(&dv, cb, &cx, 0, 0) == kUsbPipe);
            CHECK(sys.resets == resets0 + 1 && sys.clearHalts >= clears0 + 3);
            sys.halted = false; sys.stuck = false;
        }
        // ... and a stream that goes silent for more than 2 s (no error at all) is restarted instead of waiting 10 s
        {
            sys.stallUntil = rtlfd_now() + 2.3;
            const int starts0 = sys.streamStarts;
            cx.bytes = 0; cx.calls = 0; cx.stopAt = 100000;
            const double t0 = rtlfd_now();
            CHECK(rtlfd::readAsync(&dv, cb, &cx, 0, 0) == 0 && cx.bytes >= 100000);
            CHECK(sys.streamStarts >= starts0 + 2 && rtlfd_now() - t0 < 6.0);
            sys.stallUntil = 0;
        }
    }
#endif
    printf(failures ? "usb_fd: %d FAILED\n" : "usb_fd: ok\n", failures);
    return failures ? 1 : 0;
}
