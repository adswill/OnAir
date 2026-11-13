// Native driver for the RX888 MkII (a Cypress FX3 USB 3 board: an LTC2208-class 16-bit converter that samples the HF input directly, up to
// 130 Msps, and an R828D tuner behind the VHF input). No vendor library is used: the radio is driven over USB (libusb) with the protocol of the
// SDDC firmware, written from the MIT-licensed renardspark/SDDC_Driver (Interface.h, Core/usb/*, Core/radio/RX888R2Radio.cpp), which is ik1xpv's
// ExtIO_sddc / libsddc. See rx888_usb.h for the requests. NOT tested on a real radio: tests/source/test_rx888.cpp runs it against a fake FX3.
//
// - The FX3 has no firmware of its own: it enumerates as 04b4:00f3 until the image of the SDDC project (SDDC_FX3.img) is written into its
//   RAM, then as 04b4:00f1 "RX888mk2". OnAir does not ship the image (it contains Cypress SDK code under a licence of its own): the driver
//   looks for it in the OnAir codecs folders and in ONAIR_RX888_FIRMWARE (rx888FirmwarePaths()). Loading that image makes the chip drop off
//   the bus and come back at a new address. On Windows the Cypress driver from the official RX-888 setup (HDSDR / ExtIO) binds 04b4:00f1;
//   OnAir uses libusb and needs WinUSB for that device. OnAir does not ship a driver.
// - The converter delivers REAL 16-bit samples at the ADC clock (64 Msps typically, 128 usual maximum, 130 rated, up to 140 accepted as ExtIO_sddc
//   does). Output rates up to half the clock are decimated by 2 or more; above that the output is not decimated (rate = the clock: the real
//   band with its mirror image, see docs/RX888-RATES-4.1.md). HF (below half the clock): the wanted
//   frequency is shifted to zero, filtered and decimated by RealDdc (native_realddc.h, the same code as the Fobos SDR's HF inputs).
//   VHF (the R828D tuner, from half the clock up to 1.75 GHz): the tuner puts the channel on an IF of 4.57 MHz, spectrum inverted
//   (SDDC's "sideband"), so the DDC is centred on the IF and its output conjugated.
// - The firmware starts the sample stream with STARTFX3 after the host has queued its bulk transfers, and stops it with STOPFX3.
#include "dect2/common/thread_name.h"
#include "native_common.h"
#include "native_realddc.h"
#include "rx888_usb.h"
#include <cstring>
#include <deque>
#include <fstream>
#include <map>
#include <libusb.h>
#ifdef _WIN32
#include <setupapi.h>
#endif

namespace dect2 {
namespace native {

// ---------------------------------------------------------------------------------------------------------------- libusb

namespace {

constexpr int kControlTimeoutMs = 5000;
constexpr int kBulkTimeoutMs = 5000;

// One lock for the context, the device list and every open handle. list() and open() hold it for the whole libusb call, so a rescan
// after the firmware jump cannot destroy the context under them.
std::mutex gUsbMu;
libusb_context* gCtx = nullptr;
int gCtxRc = 1;
int gOpenLinks = 0;
int gReenumWaitOverrideMs = 0;
int gReenumPollOverrideMs = 0;

libusb_context* usbContext(std::string& err) {
    // gUsbMu is held by the caller
    if (!gCtx && gCtxRc != 0) {
        gCtxRc = libusb_init(&gCtx);
        if (gCtxRc != 0) { gCtx = nullptr; err = std::string("libusb_init: ") + libusb_error_name(gCtxRc); }
    }
    return gCtx;
}

int reenumWaitMs() { return gReenumWaitOverrideMs > 0 ? gReenumWaitOverrideMs : (kOnWindows ? 15000 : 8000); }
int reenumPollMs() { return gReenumPollOverrideMs > 0 ? gReenumPollOverrideMs : 200; }

int statusCode(libusb_transfer_status s) {
    switch (s) {
    case LIBUSB_TRANSFER_TIMED_OUT: return LIBUSB_ERROR_TIMEOUT;
    case LIBUSB_TRANSFER_STALL: return LIBUSB_ERROR_PIPE;
    case LIBUSB_TRANSFER_NO_DEVICE: return LIBUSB_ERROR_NO_DEVICE;
    case LIBUSB_TRANSFER_OVERFLOW: return LIBUSB_ERROR_OVERFLOW;
    default: return LIBUSB_ERROR_IO;
    }
}

class LibusbLink : public Rx888Link {
public:
    LibusbLink(libusb_context* ctx, libusb_device_handle* h, bool claimed, bool ss, uint8_t ep) : ctx_(ctx), h_(h), claimed_(claimed), ss_(ss), ep_(ep) {}
    ~LibusbLink() override {
        std::lock_guard<std::mutex> g(gUsbMu);
        if (claimed_ && !gone_) libusb_release_interface(h_, 0);
        libusb_close(h_);
        if (gOpenLinks > 0) gOpenLinks--;
    }
    void noteDisconnected() override { gone_ = true; }
    int control(bool in, uint8_t request, uint16_t value, uint16_t index, uint8_t* data, uint16_t length) override {
        const uint8_t type = (uint8_t)((in ? LIBUSB_ENDPOINT_IN : LIBUSB_ENDPOINT_OUT) | LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE);
        return libusb_control_transfer(h_, type, request, value, index, data, length, kControlTimeoutMs);
    }
    bool superSpeed() const override { return ss_; }
    void interrupt() override { libusb_interrupt_event_handler(ctx_); }
    int stream(size_t bytes, int queued, const std::function<void(const uint8_t*, size_t)>& onData, const std::function<bool()>& keepGoing,
               const std::function<void()>& onStarted) override {
        if (!ep_) return LIBUSB_ERROR_NOT_FOUND;
        struct State {
            const std::function<void(const uint8_t*, size_t)>* onData;
            const std::function<bool()>* keepGoing;
            std::atomic<int> inflight{0};
            std::atomic<int> fail{0};
        } st;
        st.onData = &onData;
        st.keepGoing = &keepGoing;
        auto cb = [](libusb_transfer* t) {
            auto* s = static_cast<State*>(t->user_data);
            if (t->status == LIBUSB_TRANSFER_COMPLETED) {
                if (t->actual_length > 0) (*s->onData)(t->buffer, (size_t)t->actual_length);
                if (!s->fail && (*s->keepGoing)()) {
                    const int r = libusb_submit_transfer(t);
                    if (r == 0) return;
                    s->fail = r;
                }
            } else if (t->status != LIBUSB_TRANSFER_CANCELLED && !s->fail) {
                s->fail = statusCode(t->status);
            }
            s->inflight--;
        };
        std::vector<std::vector<uint8_t>> bufs((size_t)queued, std::vector<uint8_t>(bytes));
        std::vector<libusb_transfer*> xfers((size_t)queued, nullptr);
        for (int i = 0; i < queued; i++) {
            xfers[(size_t)i] = libusb_alloc_transfer(0);
            libusb_fill_bulk_transfer(xfers[(size_t)i], h_, ep_, bufs[(size_t)i].data(), (int)bytes, cb, &st, kBulkTimeoutMs);
        }
        for (int i = 0; i < queued && !st.fail; i++) {
            st.inflight++;
            const int r = libusb_submit_transfer(xfers[(size_t)i]);
            if (r != 0) { st.inflight--; st.fail = r; }
        }
        if (!st.fail) onStarted();
        bool cancelled = false;
        while (st.inflight > 0) {
            if (!cancelled && (st.fail || !keepGoing())) {
                for (auto* t : xfers) libusb_cancel_transfer(t);   // (a transfer that is not in flight answers NOT_FOUND: fine)
                cancelled = true;
            }
            timeval tv{0, 50000};
            const int r = libusb_handle_events_timeout_completed(ctx_, &tv, nullptr);
            if (r != 0 && r != LIBUSB_ERROR_TIMEOUT && r != LIBUSB_ERROR_INTERRUPTED && !st.fail) st.fail = r;
        }
        for (auto* t : xfers) libusb_free_transfer(t);
        return st.fail;
    }

private:
    libusb_context* ctx_;
    libusb_device_handle* h_;
    bool claimed_;
    bool gone_ = false;
    bool ss_;
    uint8_t ep_;
};

std::string stringOf(libusb_device_handle* h, uint8_t idx) {
    unsigned char b[256] = {0};
    if (!idx || libusb_get_string_descriptor_ascii(h, idx, b, sizeof b - 1) <= 0) return {};
    return reinterpret_cast<char*>(b);
}

class LibusbRx888 : public Rx888Usb {
public:
    std::vector<Rx888UsbDevice> list() override {
        std::lock_guard<std::mutex> g(gUsbMu);
        std::vector<Rx888UsbDevice> out;
        std::string err;
        libusb_context* ctx = usbContext(err);
        if (!ctx) { logOnce("RX888: " + err); return out; }
        libusb_device** devs = nullptr;
        const ssize_t n = libusb_get_device_list(ctx, &devs);
        if (n < 0) return out;
        for (ssize_t i = 0; i < n; i++) {
            libusb_device_descriptor d{};
            if (libusb_get_device_descriptor(devs[i], &d) != 0 || d.idVendor != kRx888Vid) continue;
            if (d.idProduct != kRx888PidApp && d.idProduct != kRx888PidBoot) continue;
            Rx888UsbDevice u;
            u.vid = d.idVendor; u.pid = d.idProduct;
            u.bus = libusb_get_bus_number(devs[i]); u.address = libusb_get_device_address(devs[i]);
            if (!u.bootloader()) {
                // 04b4:00f1 is also what a LimeSDR-USB shows (LimeSuite's): the strings say which it is. Read once per device: a radio that is
                // open (Windows: one handle at a time) could not be asked again. A failed or empty read is not cached: the name often appears
                // only after Windows has finished binding the driver of a radio that just re-enumerated.
                const int key = u.bus * 256 + u.address;
                auto it = known_.find(key);
                if (it != known_.end() && it->second.pid == u.pid && it->second.nameError == 0 && !it->second.product.empty()) {
                    u = it->second;
                } else {
                    libusb_device_handle* h = nullptr;
                    const int r = libusb_open(devs[i], &h);
                    if (r != 0) {
                        u.nameError = r;
                        logOnce("RX888: a Cypress FX3 (04b4:00f1) could not be opened to read its name: " + usbErrorText(r));
                    } else {
                        u.manufacturer = stringOf(h, d.iManufacturer);
                        u.product = stringOf(h, d.iProduct);
                        u.serial = stringOf(h, d.iSerialNumber);
                        libusb_close(h);
                        if (!u.product.empty()) known_[key] = u;
                    }
                }
                if (u.nameError == 0 && !rx888ProductIsMk2(u.product)) continue;
            }
            out.push_back(u);
        }
        libusb_free_device_list(devs, 1);
        return out;
    }
    std::unique_ptr<Rx888Link> open(const Rx888UsbDevice& d, int& code) override {
        std::lock_guard<std::mutex> g(gUsbMu);
        code = 0;
        std::string err;
        libusb_context* ctx = usbContext(err);
        if (!ctx) { code = LIBUSB_ERROR_OTHER; return nullptr; }
        libusb_device** devs = nullptr;
        const ssize_t n = libusb_get_device_list(ctx, &devs);
        if (n < 0) { code = (int)n; return nullptr; }
        libusb_device* dev = nullptr;
        for (ssize_t i = 0; i < n; i++) {
            libusb_device_descriptor dd{};
            if (libusb_get_device_descriptor(devs[i], &dd) == 0 && dd.idVendor == d.vid && dd.idProduct == d.pid &&
                libusb_get_bus_number(devs[i]) == d.bus && libusb_get_device_address(devs[i]) == d.address) { dev = devs[i]; break; }
        }
        if (!dev) { libusb_free_device_list(devs, 1); code = LIBUSB_ERROR_NO_DEVICE; return nullptr; }
        libusb_device_handle* h = nullptr;
        int r = libusb_open(dev, &h);
        if (r != 0) { libusb_free_device_list(devs, 1); code = r; return nullptr; }
#ifndef _WIN32
        if (libusb_kernel_driver_active(h, 0) == 1) libusb_detach_kernel_driver(h, 0);
#endif
        r = libusb_claim_interface(h, 0);
        if (r != 0) { libusb_close(h); libusb_free_device_list(devs, 1); code = r; return nullptr; }
        // the speed (unknown counts as fine), and the bulk IN endpoint of the application firmware
        const int speed = libusb_get_device_speed(dev);
        const bool ss = speed == LIBUSB_SPEED_UNKNOWN || speed >= LIBUSB_SPEED_SUPER;
        uint8_t ep = 0;
        libusb_config_descriptor* cfg = nullptr;
        if (!d.bootloader() && libusb_get_active_config_descriptor(dev, &cfg) == 0) {
            for (int i = 0; i < cfg->bNumInterfaces && !ep; i++)
                for (int a = 0; a < cfg->interface[i].num_altsetting && !ep; a++)
                    for (int e = 0; e < cfg->interface[i].altsetting[a].bNumEndpoints; e++) {
                        const libusb_endpoint_descriptor& x = cfg->interface[i].altsetting[a].endpoint[e];
                        if ((x.bmAttributes & 0x03) == LIBUSB_TRANSFER_TYPE_BULK && (x.bEndpointAddress & 0x80)) { ep = x.bEndpointAddress; break; }
                    }
            libusb_free_config_descriptor(cfg);
        }
        libusb_free_device_list(devs, 1);
        gOpenLinks++;
        return std::make_unique<LibusbLink>(ctx, h, true, ss, ep);
    }
    void rescan() override {
        std::lock_guard<std::mutex> g(gUsbMu);
        known_.clear();
        // The Windows backend keeps the device list of this context. After the FX3 re-enumerates, that list still describes the boot
        // loader that has gone, and the new 04b4:00f1 does not show up until the context is created again. No handle is open here: the
        // boot loader's was closed before this runs.
        if (gOpenLinks == 0 && gCtx) {
            libusb_exit(gCtx);
            gCtx = nullptr;
            gCtxRc = 1;
        }
    }

private:
    void logOnce(const std::string& m) {
        if (m == lastLog_) return;
        lastLog_ = m;
        fprintf(stderr, "%s\n", m.c_str());
        fflush(stderr);
    }
    std::map<int, Rx888UsbDevice> known_;
    std::string lastLog_;
};

Rx888Usb* gUsb = nullptr;

} // namespace

Rx888Usb& rx888Usb() {
    static LibusbRx888 real;
    return gUsb ? *gUsb : real;
}
void rx888UseUsb(Rx888Usb* usb) { gUsb = usb; }
void rx888SetReenumWaitMs(int ms) { gReenumWaitOverrideMs = ms; }
void rx888SetReenumPollMs(int ms) { gReenumPollOverrideMs = ms; }

// ---------------------------------------------------------------------------------------------------------------- firmware image

std::vector<std::string> rx888FirmwarePaths() {
    std::vector<std::string> v;
    if (const char* e = getenv("ONAIR_RX888_FIRMWARE")) if (*e) v.push_back(e);
    if (const char* n = getenv("ONAIR_RX888_NO_CODEC_SEARCH")) if (*n) return v;   // (tests: a firmware image next to the build must not decide the "no image" cases)
    for (const auto& dir : codecFolders()) v.push_back(dir + "SDDC_FX3.img");
    return v;
}

namespace {
uint32_t word(const std::vector<uint8_t>& b, size_t at) { uint32_t w; memcpy(&w, b.data() + at, 4); return w; }   // (little endian hosts)
}

std::string rx888FirmwareProblem(const std::vector<uint8_t>& im) {
    if (im.size() < 16) return "the file is too short to be a firmware image";
    if (im[0] != 'C' || im[1] != 'Y') return "it has no Cypress \"CY\" signature";
    if (im[3] == 0xB1) return "security binary images are not supported";
    if (im[3] == 0xB2) return "VID:PID images are not supported";
    if (im[3] != 0xB0) return "it is not a normal firmware binary image (type " + std::to_string(im[3]) + ")";
    size_t at = 4;
    uint32_t sum = 0;
    for (;;) {
        if (at + 8 > im.size()) return "the section list ends inside the file";
        const uint32_t len = word(im, at);
        at += 8;
        if (len == 0) break;
        if ((uint64_t)len * 4 > im.size() - at) return "a section runs past the end of the file";
        for (uint32_t i = 0; i < len; i++) sum += word(im, at + (size_t)i * 4);
        at += (size_t)len * 4;
    }
    if (at + 4 > im.size()) return "the checksum is missing";
    if (word(im, at) != sum) return "the checksum does not match (damaged download?)";
    return "";
}

bool rx888LoadFirmware(Rx888Link& link, const std::vector<uint8_t>& im, std::string& err) {
    const std::string bad = rx888FirmwareProblem(im);
    if (!bad.empty()) { err = "the firmware image is not usable: " + bad; return false; }
    size_t at = 4;
    uint32_t entry = 0;
    for (;;) {
        const uint32_t len = word(im, at);
        uint32_t addr = word(im, at + 4);
        at += 8;
        if (len == 0) { entry = addr; break; }
        const uint8_t* p = im.data() + at;
        size_t left = (size_t)len * 4;
        at += left;
        while (left > 0) {
            const size_t n = std::min<size_t>(4096, left);
            uint8_t back[4096];
            if (link.control(false, kEzUsbRw, (uint16_t)(addr & 0xFFFF), (uint16_t)(addr >> 16), const_cast<uint8_t*>(p), (uint16_t)n) != (int)n ||
                link.control(true, kEzUsbRw, (uint16_t)(addr & 0xFFFF), (uint16_t)(addr >> 16), back, (uint16_t)n) != (int)n) {
                err = "the firmware could not be written to the FX3's RAM (USB transfer failed)";
                return false;
            }
            if (memcmp(back, p, n) != 0) { err = "the firmware read back from the FX3's RAM differs from what was written"; return false; }
            p += n; addr += (uint32_t)n; left -= n;
        }
    }
    const int r = link.control(false, kEzUsbRw, (uint16_t)(entry & 0xFFFF), (uint16_t)(entry >> 16), nullptr, 0);
    if (r != 0 && r != LIBUSB_ERROR_IO) {   // (the radio drops off the bus as it starts: an I/O error here is the normal case)
        err = "the FX3 did not start the firmware (" + std::string(usbErrorName(r)) + ")";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------------------------------------------- gains

namespace {
// the SDDC tables (RX888R2Radio.cpp): R828D LNA + mixer and VGA gains in dB per step; the AD8340 follows a formula
const double kVhfRfDb[29] = {0.0, 0.9, 1.4, 2.7, 3.7, 7.7, 8.7, 12.5, 14.4, 15.7, 16.6, 19.7, 20.7, 22.9, 25.4, 28.0, 29.7, 32.8,
                             33.8, 36.4, 37.2, 38.6, 40.2, 42.1, 43.4, 43.9, 44.5, 48.0, 49.6};
const double kVhfIfDb[16] = {-4.7, -2.1, 0.5, 3.5, 7.7, 11.2, 13.6, 14.9, 16.3, 19.5, 23.1, 26.5, 30.0, 33.7, 37.2, 40.8};
constexpr int kAd8340Steps = 127, kGainSweetPoint = 18;

int nearest(const double* t, int n, double want) {
    int best = 0;
    for (int i = 1; i < n; i++) if (std::fabs(t[i] - want) < std::fabs(t[best] - want)) best = i;
    return best;
}
}

double rx888HfIfGainDb(int index) {
    index = std::clamp(index, 0, kAd8340Steps - 1);
    return index > kGainSweetPoint ? 20.0 * std::log10(0.409 * (index - kGainSweetPoint + 3)) : 20.0 * std::log10(0.059 * (index + 1));
}

Rx888Gains rx888Gains(double slider, double hfAtt) {
    Rx888Gains g;
    slider = std::clamp(slider, 0.0, kRx888GainMaxDb);
    g.dat31 = (int)std::clamp(std::lround(hfAtt * 2), 0L, 63L);
    // HF: the attenuator is a setting of its own; the slider is the AD8340 IF gain, from its lowest step upwards
    double best = 1e9;
    const double target = rx888HfIfGainDb(0) + slider;
    for (int i = 0; i < kAd8340Steps; i++) {
        const double d = std::fabs(rx888HfIfGainDb(i) - target);
        if (d < best) { best = d; g.ifIndex = i; }
    }
    g.ad8340 = g.ifIndex > kGainSweetPoint ? (0x80 | (g.ifIndex - kGainSweetPoint + 3)) : (g.ifIndex + 1);
    g.hfIfDb = rx888HfIfGainDb(g.ifIndex);
    // VHF: the tuner's LNA and mixer first (the lowest noise), the VGA with what is left
    g.lna = nearest(kVhfRfDb, 29, std::min(slider, kVhfRfDb[28]));
    g.vga = nearest(kVhfIfDb, 16, kVhfIfDb[0] + std::max(0.0, slider - kVhfRfDb[28]));
    g.vhfRfDb = kVhfRfDb[g.lna];
    g.vhfIfDb = kVhfIfDb[g.vga];
    return g;
}

// ---------------------------------------------------------------------------------------------------------------- the radio

namespace {

constexpr double kHfMinHz = 10e3, kVhfMaxHz = 1.75e9;
constexpr double kAdcLowHz = 64e6, kAdcHighHz = 128e6;
constexpr double kIfHz = 4.57e6, kR828dRefHz = 16e6;   // the IF carrier of the R828D path and its reference clock (R828D_IF_CARRIER, R828D_FREQ)
constexpr double kVhfIfBwHz = 8e6;   // about what the R828D's IF filter passes at most: the VHF input gives no more than this whatever the rate
constexpr int kMaxDecim = 4096;
constexpr size_t kTransferBytes = 131072;   // a multiple of 16 bursts of 1 KiB, as SDDC (transferSize)

std::string mhzText(double hz) {
    char b[32];
    snprintf(b, sizeof b, "%.6g MHz", hz / 1e6);
    return b;
}
std::string mText(double v) {   // a rate in millions without the unit
    char b[32];
    snprintf(b, sizeof b, "%.6g", v / 1e6);
    return b;
}

std::vector<RadioSetting> rx888Settings() {
    return {
        ppmSetting(0.1),
        choiceSetting("input", "Input",
                      "Auto: the HF input below half the ADC clock (32 MHz at 64 Msps), the VHF input (the R828D tuner, up to 1.75 GHz) above it.\n"
                      "HF or VHF: that input whatever the frequency.",
                      {"auto", "hf", "vhf"}, {"Auto", "HF", "VHF"}, "auto"),
        choiceSetting("adc", "ADC clock",
                      "Auto: 64 Msps (128 Msps when a rate above 32 Msps is chosen, twice the rate up to 130 for 64 to 65 Msps, the rate itself\n"
                      "from 128 to 140); the output rate is the clock divided by a whole number, never below the rate asked for. Above half the\n"
                      "clock the output is not decimated: the HF span is still only half the clock (64 MHz at 128 Msps), with a mirror image.\n"
                      "64 / 128: that clock always (128 Msps needs a fast USB 3 port and a fast computer: 256 MB/s).\n"
                      "Match the rate (experimental): the clock is set to the rate asked for times a whole number, so the output rate is exact;\n"
                      "the clock is then not one of the usual ones.",
                      {"auto", "64", "128", "exact"}, {"Auto", "64 Msps", "128 Msps", "Match the rate (experimental)"}, "auto", true),
        choiceSetting("hfatt", "HF attenuator",
                      "The attenuator in front of the HF input, 0 to 31.5 dB. The gain slider acts on the amplifier behind it (HF) or on the\n"
                      "R828D (VHF). Raise the attenuation when the converter overloads on strong shortwave stations.",
                      {"0", "6", "12", "18", "24", "31.5"}, {"0 dB", "6 dB", "12 dB", "18 dB", "24 dB", "31.5 dB"}, "0"),
        boolSetting("dither", "Dither", "The converter's dither noise: less distortion from weak signals, a little more noise.", false),
        boolSetting("rand", "Randomizer", "The converter's output randomiser: keeps its own clock out of the spectrum. OnAir undoes it.", false),
        boolSetting("pga", "PGA", "The converter's programmable gain stage (full-scale range).", false)};
}

} // namespace

// The Windows driver bound to a USB device, or "" when it is not there. libusb only sees WinUSB / libusbK / libusb0, so a radio that
// re-enumerated onto the Cypress driver (the official RX-888 installer) is missing from list() even though Windows has it.
// Loaded by name: the Windows build does not link setupapi.
static std::string windowsUsbService(uint16_t vid, uint16_t pid) {
#ifndef _WIN32
    (void)vid; (void)pid;
    return {};
#else
    HMODULE mod = LoadLibraryW(L"setupapi.dll");
    if (!mod) return {};
    struct Guard { HMODULE m; ~Guard() { if (m) FreeLibrary(m); } } guard{mod};
    using Get_t = HDEVINFO (WINAPI *)(const GUID*, PCWSTR, HWND, DWORD);
    using Enum_t = BOOL (WINAPI *)(HDEVINFO, DWORD, PSP_DEVINFO_DATA);
    using Id_t = BOOL (WINAPI *)(HDEVINFO, PSP_DEVINFO_DATA, PWSTR, DWORD, PDWORD);
    using Prop_t = BOOL (WINAPI *)(HDEVINFO, PSP_DEVINFO_DATA, DWORD, PDWORD, PBYTE, DWORD, PDWORD);
    using Destroy_t = BOOL (WINAPI *)(HDEVINFO);
    auto pGet = (Get_t)GetProcAddress(mod, "SetupDiGetClassDevsW");
    auto pEnum = (Enum_t)GetProcAddress(mod, "SetupDiEnumDeviceInfo");
    auto pId = (Id_t)GetProcAddress(mod, "SetupDiGetDeviceInstanceIdW");
    auto pProp = (Prop_t)GetProcAddress(mod, "SetupDiGetDeviceRegistryPropertyW");
    auto pDestroy = (Destroy_t)GetProcAddress(mod, "SetupDiDestroyDeviceInfoList");
    if (!pGet || !pEnum || !pId || !pProp || !pDestroy) return {};
    HDEVINFO info = pGet(nullptr, L"USB", nullptr, DIGCF_PRESENT | DIGCF_ALLCLASSES);
    if (info == INVALID_HANDLE_VALUE) return {};
    char needle[32];
    snprintf(needle, sizeof needle, "VID_%04X&PID_%04X", vid, pid);
    std::string found;
    SP_DEVINFO_DATA dev{};
    dev.cbSize = sizeof dev;
    for (DWORD i = 0; pEnum(info, i, &dev); i++) {
        WCHAR id[512];
        if (!pId(info, &dev, id, 512, nullptr)) continue;
        char ascii[512];
        size_t n = 0;
        for (; n + 1 < sizeof ascii && id[n]; n++) ascii[n] = (char)toupper((unsigned char)id[n]);
        ascii[n] = 0;
        if (!strstr(ascii, needle)) continue;
        WCHAR svc[128] = {};
        if (!pProp(info, &dev, SPDRP_SERVICE, nullptr, (PBYTE)svc, sizeof svc, nullptr)) { found = "present"; continue; }
        std::string s;
        for (int k = 0; svc[k] && k < 127; k++) s.push_back((char)tolower((unsigned char)svc[k]));
        if (s.empty()) { found = "present"; continue; }
        found = s;
        if (s.find("cyusb") != std::string::npos || s.find("cypress") != std::string::npos) break;
    }
    pDestroy(info);
    return found;
#endif
}

static bool windowsLibusbDriverBound() {
#ifndef _WIN32
    return false;
#else
    const std::string svc = windowsUsbService(kRx888Vid, kRx888PidApp);
    return svc == "winusb" || svc == "libusbk" || svc == "libusb0" || svc.find("libusb") != std::string::npos;
#endif
}

// What start() shows when the firmware was written and the new device never became an RX888 MkII we can name.
// Kept free of the words start() treats as "try again" ("not found", "unplugged"): a retry looks up the boot loader's old
// address, which has gone, and would replace this with "RX888 MkII not found".
static std::string reenumTimeoutMessage() {
    const std::string svc = windowsUsbService(kRx888Vid, kRx888PidApp);
    const bool cypress = svc.find("cyusb") != std::string::npos || svc.find("cypress") != std::string::npos;
    if (cypress)
        return "RX888 MkII: the firmware was loaded and the radio left the bus, but OnAir cannot see it come back. Windows has bound USB\\VID_04B4&PID_00F1 to the Cypress driver, which OnAir cannot use. OnAir does not ship a driver. Install the WinUSB driver for 04B4:00F1 with Zadig (zadig.akeo.ie); the Cypress driver stays what HDSDR and ExtIO use.";
    const bool libusbDriver = svc == "winusb" || svc == "libusbk" || svc == "libusb0" || svc == "libusb-win32";
    if (!svc.empty() && svc != "present" && !libusbDriver)
        return "RX888 MkII: the firmware was loaded and the radio left the bus, but OnAir cannot see it come back. Windows has bound USB\\VID_04B4&PID_00F1 to the driver \"" + svc + "\". OnAir uses libusb, needs the WinUSB driver for that device (Zadig, zadig.akeo.ie), and does not ship a driver.";
    std::string e = "RX888 MkII: the firmware was loaded and the radio left the bus, but it did not come back in time. Unplug it, plug it in again and try once more.";
    if (kOnWindows && !libusbDriver)
        e += " On Windows the USB package from the official RX-888 setup binds USB\\VID_04B4&PID_00F1 and hides the radio from OnAir. OnAir needs the WinUSB driver for that device (Zadig, zadig.akeo.ie) and does not ship a driver.";
    return e;
}

class Rx888Source : public NativeSource {
public:
    Rx888Source(std::string args, std::string serial) : args_(std::move(args)), serial_(std::move(serial)) {}
    ~Rx888Source() override { stop(); }

protected:
    bool openDevice(const TuneSettings& s, std::string& err) override {
        std::lock_guard<std::mutex> lk(api_);
        if (link_) return false;
        Rx888UsbDevice dev;
        if (!resolve(dev)) { err = "RX888 MkII" + (serial_.empty() ? std::string() : " " + serial_) + " not found (unplugged?)"; return false; }
        if (dev.bootloader() && !uploadFirmware(dev, err)) return false;
        int code = 0;
        link_ = rx888Usb().open(dev, code);
        if (!link_) {
            err = "RX888 MkII: cannot open the radio: " + usbErrorText(code);
            if (code == LIBUSB_ERROR_ACCESS || code == LIBUSB_ERROR_NOT_SUPPORTED) openFailure_ = OpenFailure::Final;
            if (kOnWindows && code == LIBUSB_ERROR_NOT_SUPPORTED)
                err += " The Cypress driver from the official RX-888 setup is not usable by OnAir, and OnAir does not ship a driver. After the firmware loads, the radio is USB\\VID_04B4&PID_00F1 and needs the WinUSB driver: install it once with Zadig (zadig.akeo.ie), choosing that device and the driver WinUSB.";
            return false;
        }
        usb2_ = !link_->superSpeed();   // a warning, not a refusal (4.1): USB 2 carries ~40 MB/s, far less than the ADC delivers
        if (usb2_) { fprintf(stderr, "RX888 MkII: on a USB 2 connection: most samples will be lost\n"); fflush(stderr); }
        std::this_thread::sleep_for(std::chrono::milliseconds(kOnWindows ? 25 : 5));
        setU8(kStopFx3, 0);   // as the SDDC driver: a stream left running by another program is stopped
        uint8_t info[4] = {0};
        const int r = link_->control(true, kTestFx3, 0, 0, info, sizeof info);
        if (r != (int)sizeof info && r != LIBUSB_ERROR_PIPE) { err = "RX888 MkII: the radio did not answer (" + std::string(usbErrorName(r)) + ")"; link_.reset(); return false; }
        if (info[0] != kModelRx888Mk2) {
            err = "RX888: the radio reports SDDC model " + std::to_string(info[0]) + " (not an RX888 MkII, which is 4): OnAir drives the RX888 MkII only";
            link_.reset();
            openFailure_ = OpenFailure::Final;
            return false;
        }
        fwMajor_ = info[1];
        fwMinor_ = info[2];
        fprintf(stderr, "RX888 MkII: opened %s, SDDC firmware %d.%d\n", dev.serial.c_str(), fwMajor_, fwMinor_);
        fflush(stderr);
        gpios_ = 0; vhf_ = -1; adc_ = 0; dat31_ = ad8340_ = lna_ = vga_ = -1; tuneHz_ = 0;
        streamed_ = false;
        const bool ok = apply(s, err, false);
        if (ok && usb2_)
            addNote(err, "RX888 MkII: the radio is on a USB 2 connection, which carries about 40 MB/s; the ADC sends " + mText(adc_ * 2.0) +
                         " MB/s, so most samples will be lost. Use a USB 3 (SuperSpeed) port and cable");
        return ok;
    }
    // A retune that needs another clock or decimation stops the stream for it, as a change of the sample rate does (NativeSource::retune)
    bool configure(const TuneSettings& s, std::string& err, bool live) override {
        if (live) {
            bool restart = false;
            {
                std::lock_guard<std::mutex> lk(api_);
                if (!link_) return false;
                const Plan p = plan(s);
                restart = p.adc != adc_ || p.decim != decim_;
            }
            if (restart) {
                stopStream();
                bool ok;
                { std::lock_guard<std::mutex> lk(api_); ok = apply(s, err, false); }
                startStream();
                return ok;
            }
        }
        std::lock_guard<std::mutex> lk(api_);
        return apply(s, err, live);
    }
    void closeDevice() override {
        std::lock_guard<std::mutex> lk(api_);
        if (!link_) return;
        if (streamed_) setU8(kStopFx3, 0);
        link_.reset();
    }
    void streamLoop() override {
        Rx888Link* link;
        { std::lock_guard<std::mutex> lk(api_); link = link_.get(); }
        if (!link || !run_) return;
        // The libusb callback only copies the block: the conversion (the DDC) runs on a thread of its own, so a slow block does not keep
        // the transfers from being queued again.
        std::mutex qMu;
        std::condition_variable qCv;
        std::deque<std::vector<int16_t>> q;
        std::vector<std::vector<int16_t>> pool;
        bool done = false;
        size_t dropped = 0;
        const Rx888TransferPlan tp = rx888TransferPlan(adc_ ? (double)adc_ : kAdcLowHz);
        auto lastReport = std::chrono::steady_clock::now();
        size_t reported = 0;
        std::thread worker([&] {
            dect2::setThreadName("onair-rx888");
            for (;;) {
                std::vector<int16_t> b;
                {
                    std::unique_lock<std::mutex> lk(qMu);
                    qCv.wait(lk, [&] { return !q.empty() || done; });
                    if (q.empty()) return;
                    b = std::move(q.front());
                    q.pop_front();
                }
                convert(b.data(), b.size());
                std::lock_guard<std::mutex> lk(qMu);
                pool.push_back(std::move(b));
            }
        });
        std::atomic<bool> startFailed{false};
        const int r = link->stream(
            tp.bytes, tp.queued,
            [&](const uint8_t* p, size_t n) {
                n &= ~(size_t)1;
                std::vector<int16_t> b;
                {
                    std::lock_guard<std::mutex> lk(qMu);
                    if (!pool.empty()) { b = std::move(pool.back()); pool.pop_back(); }
                }
                b.resize(n / 2);
                memcpy(b.data(), p, n);   // (little endian hosts)
                {
                    std::lock_guard<std::mutex> lk(qMu);
                    if (q.size() >= tp.maxBlocks) {
                        droppedSamples_ += q.front().size();
                        pool.push_back(std::move(q.front())); q.pop_front(); dropped++;
                    }
                    q.push_back(std::move(b));
                }
                if (dropped != reported) {   // at most every 5 s: the computer cannot keep up with this clock
                    const auto now = std::chrono::steady_clock::now();
                    if (now - lastReport > std::chrono::seconds(5)) {
                        fprintf(stderr, "RX888 MkII: the computer is falling behind at %s: %zu blocks (%llu samples) dropped so far (a lower ADC clock helps)\n",
                                mhzText(adc_).c_str(), dropped, (unsigned long long)droppedSamples_.load());
                        fflush(stderr);
                        lastReport = now; reported = dropped;
                    }
                }
                qCv.notify_one();
            },
            [&] { return run_.load() && !startFailed.load(); },
            [&] {
                std::lock_guard<std::mutex> lk(api_);
                streamed_ = true;
                if (!setU8(kStartFx3, 0)) startFailed = true;
            });
        { std::lock_guard<std::mutex> lk(qMu); done = true; }
        qCv.notify_all();
        worker.join();
        { std::lock_guard<std::mutex> lk(api_); if (link_) setU8(kStopFx3, 0); }
        if (dropped) { fprintf(stderr, "RX888 MkII: the computer fell behind: %zu blocks (%llu samples) were dropped (a lower ADC clock helps)\n", dropped, (unsigned long long)droppedSamples_.load()); fflush(stderr); }
        if (startFailed) { fprintf(stderr, "RX888 MkII: the radio did not take the start command\n"); fflush(stderr); }
        else if (run_) { fprintf(stderr, "RX888 MkII: the stream ended (%s)\n", r ? usbErrorName(r) : "stopped"); fflush(stderr); }
    }
    void interrupt() override { if (link_) link_->interrupt(); }

public:
    // the status line: samples lost because the computer (or a USB 2 link) could not keep up
    std::string statusNote() const override {
        const unsigned long long n = droppedSamples_.load();
        std::string t;
        if (n) t = "RX888: " + std::to_string(n) + " samples dropped (computer behind; try a lower ADC clock)";
        if (usb2_) t += std::string(t.empty() ? "" : "; ") + "RX888 on USB 2: needs USB 3";
        return t;
    }

private:
    struct Plan {
        bool vhf = false;
        uint32_t adc = 0;      // the ADC clock
        int decim = 2;
        double outHz = 0;
    };
    Plan plan(const TuneSettings& s) const {
        Plan p;
        const Rx888RatePlan r = rx888RatePlan(s.sampleRate, radioOption(s, "adc", "auto"));
        p.adc = r.adcHz; p.decim = r.decim; p.outHz = r.outHz;
        const std::string in = radioOption(s, "input", "auto");
        p.vhf = in == "vhf" || (in == "auto" && s.centerHz >= p.adc / 2.0);
        return p;
    }

    bool resolve(Rx888UsbDevice& out) {
        const std::vector<Rx888UsbDevice> all = rx888Usb().list();
        int bus = -1, addr = -1;
        if (sscanf(args_.c_str(), "%d:%d", &bus, &addr) != 2) bus = addr = -1;
        const Rx888UsbDevice* hit = nullptr;
        if (!serial_.empty()) for (const auto& d : all) if (d.serial == serial_) { hit = &d; break; }
        if (!hit) for (const auto& d : all) if (d.bus == bus && d.address == addr) { hit = &d; break; }
        if (!hit && all.size() == 1 && (serial_.empty() || all[0].bootloader())) hit = &all[0];   // (the bootloader has no serial and a new address after unplugging)
        if (!hit) return false;
        out = *hit;
        return true;
    }

    // The FX3 is in its boot loader: write the image (found as rx888FirmwarePaths() says), wait for the radio to come back as 04b4:00f1
    bool uploadFirmware(Rx888UsbDevice& dev, std::string& err) {
        std::vector<uint8_t> image;
        std::string path;
        for (const auto& p : rx888FirmwarePaths()) {
            std::ifstream f(p, std::ios::binary);
            if (!f) continue;
            image.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
            path = p;
            break;
        }
        if (image.empty()) {
            err = "RX888 MkII: the FX3 firmware image (SDDC_FX3.img) was not found. The radio needs it each time it is plugged in; put the file in OnAir's codecs folder";
            openFailure_ = OpenFailure::Final;
            return false;
        }
        const std::string bad = rx888FirmwareProblem(image);
        if (!bad.empty()) {
            err = "RX888 MkII: the FX3 firmware image (SDDC_FX3.img, " + path + ") is not usable: " + bad;
            openFailure_ = OpenFailure::Final;
            return false;
        }
        set_before_.clear();
        for (const auto& d : rx888Usb().list()) if (!d.bootloader()) set_before_.push_back(d.bus * 256 + d.address);
        {
            int code = 0;
            std::unique_ptr<Rx888Link> l = rx888Usb().open(dev, code);
            if (!l) { err = "RX888 MkII: cannot open the radio's boot loader: " + usbErrorText(code); if (code == LIBUSB_ERROR_ACCESS || code == LIBUSB_ERROR_NOT_SUPPORTED) openFailure_ = OpenFailure::Final; return false; }
            std::string e;
            if (!rx888LoadFirmware(*l, image, e)) { err = "RX888 MkII: " + e; return false; }
            fprintf(stderr, "RX888 MkII: firmware loaded from %s\n", path.c_str());
            fflush(stderr);
            l->noteDisconnected();   // the jump drops the chip off the bus; releasing the interface on it can stall
        }
        rx888Usb().rescan();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(reenumWaitMs());
        int pendingKey = -1;
        auto pendingSince = std::chrono::steady_clock::now();
        auto take = [&](const Rx888UsbDevice& d) {
            dev = d;
            args_ = std::to_string(d.bus) + ":" + std::to_string(d.address);   // the boot loader's address is gone
            if (!d.serial.empty()) serial_ = d.serial;
            fprintf(stderr, "RX888 MkII: radio back at %d:%d\n", d.bus, d.address);
            fflush(stderr);
            return true;
        };
        // The new device has a new USB address. Keep looking until it is an RX888 MkII, or until Windows has had time to bind its
        // driver (several seconds; the old 6 s cap, then a retry of the boot loader address, reported "not found").
        for (;;) {
            const auto now = std::chrono::steady_clock::now();
            const std::vector<Rx888UsbDevice> nowList = rx888Usb().list();
            const Rx888UsbDevice* pick = nullptr;
            const Rx888UsbDevice* unread = nullptr;
            for (const auto& d : nowList) {
                const int key = d.bus * 256 + d.address;
                if (d.bootloader()) continue;
                // An app-mode radio that was already there is not the one that just left the bus. The boot loader's own address is not in
                // this set, so the running radio is taken even when Windows gives it that address again.
                if (std::find(set_before_.begin(), set_before_.end(), key) != set_before_.end()) continue;
                if (d.nameError == 0 && rx888ProductIsMk2(d.product)) { pick = &d; break; }
                if (d.nameError != 0 && !unread) unread = &d;
            }
            if (pick) return take(*pick);
            if (unread) {
                const int key = unread->bus * 256 + unread->address;
                if (key != pendingKey) { pendingKey = key; pendingSince = now; }
                const bool driver = usbDriverMissing(unread->nameError);
                const bool graceOver = now - pendingSince >= std::chrono::seconds(1);
                if (now >= deadline || (driver && graceOver)) return take(*unread);   // open() reports the driver, or the name arrives on a retry
            }
            if (now >= deadline) break;
            auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
            auto step = std::chrono::milliseconds(reenumPollMs());
            if (step > left) step = left;
            if (step.count() > 0) std::this_thread::sleep_for(step);
        }
        err = reenumTimeoutMessage();
        openFailure_ = OpenFailure::Final;   // a retry would resolve the boot loader and report "not found (unplugged?)"
        return false;
    }

    // Vendor control transfers: retry PIPE/IO/TIMEOUT/BUSY (common on Windows WinUSB right after open).
    bool ctl(bool in, uint8_t request, uint16_t val, uint16_t idx, uint8_t* data, uint16_t len, int want, int tries = 0, int delayMs = 0) {
        if (!link_) { lastUsbErr_ = LIBUSB_ERROR_NO_DEVICE; return false; }
        const int n = tries > 0 ? tries : (kOnWindows ? 5 : 3);
        const int delay = delayMs > 0 ? delayMs : (kOnWindows ? 20 : 8);
        for (int a = 0; a < n; a++) {
            if (a) std::this_thread::sleep_for(std::chrono::milliseconds(delay));
            const int r = link_->control(in, request, val, idx, data, len);
            if (r == want) { lastUsbErr_ = 0; return true; }
            if (in && request == kTestFx3 && want == 4 && r == LIBUSB_ERROR_PIPE) { lastUsbErr_ = 0; return true; }
            lastUsbErr_ = r;
            if (r != LIBUSB_ERROR_PIPE && r != LIBUSB_ERROR_IO && r != LIBUSB_ERROR_TIMEOUT && r != LIBUSB_ERROR_BUSY) break;
        }
        return false;
    }
    std::string firmwareTag() const {
        if (fwMajor_ < 0) return {};
        return ", SDDC firmware " + std::to_string(fwMajor_) + "." + std::to_string(fwMinor_);
    }
    std::string usbStepErr(const std::string& step) const {
        std::string e = "RX888 MkII: " + step + " (" + usbErrorText(lastUsbErr_) + ")" + firmwareTag();
        if (!kOnWindows) return e;
        const bool pipeIo = lastUsbErr_ == LIBUSB_ERROR_PIPE || lastUsbErr_ == LIBUSB_ERROR_IO;
        if (windowsLibusbDriverBound()) {
            if (pipeIo)
                e += " WinUSB is already bound to this radio. Close HDSDR, ExtIO or any other program that may have the RX888 open, unplug the USB cable and plug it in again, then retry. If it keeps failing, replace SDDC_FX3.img in OnAir's codecs folder with the image from SDDC_Driver v2.0.0 (see third_party/sddc_fx3 in the source tree).";
            else if (lastUsbErr_ == LIBUSB_ERROR_NOT_SUPPORTED)
                e += " WinUSB is bound but this request was refused: the RAM firmware may be too old — load SDDC_FX3.img from SDDC_Driver v2.0.0 (unplug and replug so the boot loader at 04b4:00f3 appears, or use the official RX-888 tools once).";
        } else if (pipeIo || lastUsbErr_ == LIBUSB_ERROR_NOT_SUPPORTED)
            e += " If another SDR program has the radio open, close it. Otherwise install WinUSB for USB\\VID_04B4&PID_00F1 with Zadig (zadig.akeo.ie); the Cypress driver from the official RX-888 setup cannot be used by OnAir.";
        return e;
    }
    // RX888R2Radio::SetRFMode(HFMODE): Control(TUNERSTDBY) then VHF off. WinUSB often stalls the first EP0 write after open.
    bool setTunerStandby() {
        if (kOnWindows) std::this_thread::sleep_for(std::chrono::milliseconds(15));
        uint8_t z = 0;
        if (ctl(false, kTunerStandby, 0, 0, &z, 1, 1, kOnWindows ? 12 : 6, kOnWindows ? 35 : 12)) return true;
        // Cold start (tuner never brought up this session): some Windows stacks keep returning PIPE even though standby is already the default.
        if (vhf_ < 0 && lastUsbErr_ == LIBUSB_ERROR_PIPE) {
            fprintf(stderr, "RX888 MkII: tuner standby stalled with PIPE on a cold start (continuing%s)\n", firmwareTag().c_str());
            fflush(stderr);
            lastUsbErr_ = 0;
            return true;
        }
        return false;
    }
    bool setU8(uint8_t request, uint8_t v) { return ctl(false, request, 0, 0, &v, 1, 1); }
    bool setU32(uint8_t request, uint32_t v) { uint8_t b[4]; memcpy(b, &v, 4); return ctl(false, request, 0, 0, b, 4, 4); }
    bool setU64(uint8_t request, uint64_t v) { uint8_t b[8]; memcpy(b, &v, 8); return ctl(false, request, 0, 0, b, 8, 8); }
    bool setArg(uint16_t arg, uint16_t value) { uint8_t z = 0; return ctl(false, kSetArg, value, arg, &z, 1, 1); }
    bool setGpios(uint32_t w) {
        if (w == gpios_) return true;
        if (!setU32(kGpioFx3, w)) return false;
        gpios_ = w;
        return true;
    }
    uint32_t gpioWord(const TuneSettings& s, bool vhf) const {
        uint32_t w = 0;
        if (radioFlag(s, "dither")) w |= kGpioDither;
        if (radioFlag(s, "rand")) w |= kGpioRand;
        if (radioFlag(s, "pga")) w |= kGpioPgaEn;
        if (s.biasTee) w |= vhf ? kGpioBiasVhf : kGpioBiasHf;
        if (vhf) w |= kGpioVhfEn;
        return w;
    }

    // (under api_) the HF <-> VHF switch, in the order of the SDDC driver (RX888R2Radio::SetRFMode)
    bool switchInput(const TuneSettings& s, bool vhf, std::string& err) {
        if (vhf) {
            // the HF path is shut (maximum attenuation, lowest gain), the VHF antenna switched in, the tuner started
            if (!setArg(kArgDat31, 63)) { err = usbStepErr("the VHF input could not be switched on: HF attenuator"); return false; }
            if (!setArg(kArgAd8340, 1)) { err = usbStepErr("the VHF input could not be switched on: HF IF gain"); return false; }
            if (!setGpios(gpioWord(s, true))) { err = usbStepErr("the VHF input could not be switched on: antenna switch"); return false; }
            if (!setArg(kArgAd8340, 0x80 | 3)) { err = usbStepErr("the VHF input could not be switched on: HF VGA"); return false; }
            if (!setU32(kTunerInit, (uint32_t)kR828dRefHz)) { err = usbStepErr("the VHF input could not be switched on: R828D reference clock"); return false; }
            dat31_ = 63; ad8340_ = 0x83; lna_ = vga_ = -1; tuneHz_ = 0;
        } else {
            if (!setTunerStandby()) { err = usbStepErr("the HF input could not be switched on: tuner standby"); return false; }
            if (!setGpios(gpioWord(s, false))) { err = usbStepErr("the HF input could not be switched on: antenna switch"); return false; }
            dat31_ = ad8340_ = -1;
        }
        vhf_ = vhf ? 1 : 0;
        fprintf(stderr, "RX888 MkII: %s input\n", vhf ? "VHF (R828D tuner)" : "HF");
        fflush(stderr);
        return true;
    }

    // (under api_) live: the stream is running, only the frequency, the gains and the switches may change
    bool apply(const TuneSettings& s, std::string& err, bool live) {
        if (!link_) return false;
        const Plan p = plan(s);
        if (!live) {
            // SDDC RadioHandler::Open: HF path (tuner standby, VHF off) before STARTADC. Starting the ADC first breaks RF switching on hardware.
            if (vhf_ < 0 && !switchInput(s, false, err)) return false;
            if (!setU32(kStartAdc, p.adc)) {
                err = usbStepErr("the ADC clock of " + mhzText(p.adc) + " was refused");
                return false;
            }
            std::lock_guard<std::mutex> dl(ddcMu_);
            adc_ = p.adc; decim_ = p.decim;
            ddc_.design(decim_);
            rate_ = p.outHz;
            requested_ = s.sampleRate;
            checkRate(s.sampleRate, err);
            if (p.adc > kRx888AdcRatedHz * 1.0001)
                addNote(err, "RX888 MkII: an ADC clock of " + mhzText(p.adc) + " is above the converter's rating of 130 Msps (overclocking): it may not work on every board");
            if (p.decim == 1)
                addNote(err, "RX888 MkII: " + mText(p.outHz) + " Msps is the ADC clock itself (no decimation): the HF input still covers only 0 to " +
                             mhzText(p.adc / 2.0) + " (half the clock); the rest of the span is its mirror image");
        }
        if (vhf_ != (p.vhf ? 1 : 0) && !switchInput(s, p.vhf, err)) return false;
        const bool vhf = vhf_ == 1;
        if (!setGpios(gpioWord(s, vhf))) { err = "RX888 MkII: the switches (dither, randomizer, PGA, bias tee) could not be set (USB transfer failed)"; return false; }
        const double f = std::max(s.centerHz, 0.0);
        const double tuneHz = ppmCorrectedHz(f, radioPpm(s));
        {
            std::lock_guard<std::mutex> dl(ddcMu_);
            vhfSpectrum_ = vhf;
            rand_ = radioFlag(s, "rand");
            if (vhf) ddc_.tune(adc_, kIfHz, 1.0 / 32768.0);
            else ddc_.tune(adc_, std::max(tuneHz, 0.0), 1.0 / 32768.0);   // a mixer: above half the ADC clock it aliases, as the hardware would
        }
        if (vhf) {
            const uint64_t hz = (uint64_t)std::llround(std::max(tuneHz, 0.0));
            if (hz != tuneHz_) {
                if (!setU64(kTunerTune, hz)) { err = "RX888 MkII: the tuner was not told " + mhzText(f) + " (USB transfer failed)"; return false; }
                tuneHz_ = hz;
            }
            if (!live && rate_ > 32e6 * 1.001)   // (only the wide rates: a TV channel at 18 or 20 Msps fits)
                addNote(err, "RX888 MkII: on the VHF input the R828D tuner passes only about " + mText(kVhfIfBwHz) + " MHz around its IF, whatever the sample rate (" +
                             mText(rate_) + " Msps here); the rest of the span is empty");
            if (f < 24e6 || f > kVhfMaxHz)
                addNote(err, "the VHF input (R828D tuner) of this RX888 MkII is specified for 24 MHz to 1750 MHz; the driver may not reach " + mhzText(f) + " or the signal may be weak or absent");
        } else if (f < kHfMinHz || f > adc_ / 2.0) {
            addNote(err, "the HF input of this RX888 MkII is specified for " + mhzText(kHfMinHz) + " to " + mhzText(adc_ / 2.0) + " (half the ADC clock), the VHF input starts there; the driver may not reach " +
                  mhzText(f) + " or the signal may be weak or absent");
        }
        const Rx888Gains g = rx888Gains(s.gainDb, radioNumber(s, "hfatt", 0));
        bool ok = true;
        if (vhf) {
            if (g.lna != lna_) { if (setArg(kArgR82xxLna, (uint16_t)g.lna)) lna_ = g.lna; else ok = false; }
            if (g.vga != vga_) { if (setArg(kArgR82xxVga, (uint16_t)g.vga)) vga_ = g.vga; else ok = false; }
        } else {
            if (g.dat31 != dat31_) { if (setArg(kArgDat31, (uint16_t)g.dat31)) dat31_ = g.dat31; else ok = false; }
            if (g.ad8340 != ad8340_) { if (setArg(kArgAd8340, (uint16_t)g.ad8340)) ad8340_ = g.ad8340; else ok = false; }
        }
        if (!ok && err.empty()) err = "RX888 MkII: a gain setting was not accepted (USB transfer failed)";
        return true;
    }

    void convert(const int16_t* x, size_t n) {
        std::lock_guard<std::mutex> dl(ddcMu_);
        conv_.clear();
        ddc_.processS16(x, n, rand_, conv_);
        if (vhfSpectrum_) for (auto& c : conv_) c = std::conj(c);   // the R828D path delivers the spectrum inverted
        push(conv_.data(), conv_.size());
    }

    std::string args_, serial_;
    std::unique_ptr<Rx888Link> link_;
    std::mutex api_;                 // the control calls of this radio (retune, open, close, start, stop); the stream thread is in stream() meanwhile
    uint32_t gpios_ = 0, adc_ = 0;
    int vhf_ = -1, decim_ = 2;
    int dat31_ = -1, ad8340_ = -1, lna_ = -1, vga_ = -1;   // what the radio is set to (-1: not yet)
    int lastUsbErr_ = 0;
    int fwMajor_ = -1, fwMinor_ = -1;
    uint64_t tuneHz_ = 0;
    bool streamed_ = false;
    std::vector<int> set_before_;    // (uploadFirmware) the app-mode radios there were before it
    std::mutex ddcMu_;               // the conversion state below: the stream against a retune
    RealDdc ddc_;
    bool vhfSpectrum_ = false, rand_ = false;
    std::atomic<bool> usb2_{false};
    std::atomic<unsigned long long> droppedSamples_{0};
};

void listRx888(std::vector<DeviceInfo>& out) {
    if (nativeDisabled()) return;
    const std::vector<Rx888UsbDevice> all = rx888Usb().list();
    bool haveImage = false;
    for (const auto& p : rx888FirmwarePaths()) { std::ifstream f(p, std::ios::binary); if (f) { haveImage = true; break; } }
    for (const auto& u : all) {
        if (u.nameError != 0) continue;   // no name: 04b4:00f1 is also a LimeSDR, so it is not offered as an RX888
        DeviceInfo d;
        d.kind = DeviceInfo::Native;
        d.board = "rx888";
        d.serial = u.serial;
        d.nativeArgs = std::to_string(u.bus) + ":" + std::to_string(u.address);
        const std::string tail = u.serial.size() > 8 ? u.serial.substr(u.serial.size() - 8) : u.serial;
        d.name = "RX888 MkII" + (tail.empty() ? std::string() : " " + tail);
        if (u.bootloader()) d.name += haveImage ? " (firmware is loaded on start, experimental)" : " (needs SDDC_FX3.img, experimental)";
        else d.name += " (experimental)";
#ifdef _WIN32
        d.name += " — Zadig WinUSB 00F3+00F1, replug USB";
#endif
        d.minRateHz = kRx888MinOutHz; d.maxRateHz = kRx888MaxOutHz;   // any rate in between (and custom ones above, up to the 140 MHz clock): the clock is divided by a whole number, never below what is asked
        d.gainMinDb = 0; d.gainMaxDb = kRx888GainMaxDb;
        d.minFreqHz = kHfMinHz; d.maxFreqHz = kVhfMaxHz;
        d.hasBiasTee = true;
        d.settings = rx888Settings();
        out.push_back(d);
    }
}

Rx888RatePlan rx888RatePlan(double wantHz, const std::string& mode) {
    Rx888RatePlan p;
    const double want = std::max(wantHz > 0 ? wantHz : 8e6, kRx888MinOutHz);   // (no upper clamp: above what the clock gives, checkRate says so)
    double adc;
    if (mode == "exact") {
        int d = std::max(1, (int)std::ceil(kAdcLowHz / want * (1 - 1e-9)));   // (64 MHz / 9.142857 MHz is 7 and a bit of rounding)
        while (want * d > kRx888AdcMaxHz && d > 1) d--;
        p.decim = std::min(d, kMaxDecim);
        adc = want * p.decim;
    } else if (mode == "64" || mode == "128") {
        adc = mode == "64" ? kAdcLowHz : kAdcHighHz;
        p.decim = (int)std::clamp(std::floor(adc / want * 1.001), 1.0, (double)kMaxDecim);
    } else if (want <= 32e6 * 1.001) {
        adc = kAdcLowHz;
        p.decim = (int)std::clamp(std::floor(adc / want * 1.001), 2.0, (double)kMaxDecim);
    } else if (want <= 64e6 * 1.001) {
        adc = kAdcHighHz;
        p.decim = (int)std::clamp(std::floor(adc / want * 1.001), 2.0, (double)kMaxDecim);
    } else if (want <= kRx888AdcRatedHz / 2) {   // 64 to 65 Msps: twice the rate, decimation 2 (the clock up to its rating)
        adc = want * 2;
        p.decim = 2;
    } else if (want <= kAdcHighHz) {   // no decimation: the usual 128 MHz clock gives the most unique spectrum (64 MHz)
        adc = kAdcHighHz;
        p.decim = 1;
    } else {                           // 128 to 140: the clock is the rate (above 130 is overclocking)
        adc = want;
        p.decim = 1;
    }
    p.adcHz = (uint32_t)std::lround(std::min(adc, kRx888AdcMaxHz));
    p.outHz = (double)p.adcHz / p.decim;
    return p;
}

Rx888TransferPlan rx888TransferPlan(double adcHz) {
    Rx888TransferPlan t;
    t.bytes = kTransferBytes;
    const double bytesPerSec = std::max(adcHz, 1e6) * 2;   // 16-bit real samples
    t.queued = (int)std::clamp(std::ceil(bytesPerSec * 0.032 / (double)t.bytes), 32.0, 128.0);
    t.maxBlocks = (size_t)std::max(256.0, std::ceil(bytesPerSec * 0.25 / (double)t.bytes));
    return t;
}

std::unique_ptr<IqSource> makeRx888(const DeviceInfo& d) { return std::make_unique<Rx888Source>(d.nativeArgs, d.serial); }

} // namespace native
} // namespace dect2
