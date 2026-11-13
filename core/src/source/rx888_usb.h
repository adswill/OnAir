// The USB side of the RX888 MkII driver (source_rx888.cpp), behind two small interfaces so that the driver can be tested against a fake
// radio (tests/source/test_rx888.cpp): rx888Usb() is libusb unless a test put another one in with rx888UseUsb().
//
// The radio is a Cypress FX3 with the SDDC firmware (ik1xpv's ExtIO_sddc, libsddc; the protocol below is that of the MIT-licensed
// renardspark/SDDC_Driver, Core/usb and Core/radio/RX888R2Radio.cpp, Interface.h). The FX3 enumerates as 04b4:00f3 (boot loader) until the
// firmware image is loaded into its RAM, then as 04b4:00f1 ("sdr prototypes" / "RX888mk2").
#pragma once
#include "native_common.h"
#include <functional>

namespace dect2 {
namespace native {

constexpr uint16_t kRx888Vid = 0x04b4, kRx888PidApp = 0x00f1, kRx888PidBoot = 0x00f3;

// the vendor requests of the SDDC firmware (Interface.h: FX3Command)
enum Rx888Cmd : uint8_t {
    kStartFx3 = 0xAA,     // OUT, 1 byte: start the sample stream
    kStopFx3 = 0xAB,      // OUT, 1 byte: stop it
    kTestFx3 = 0xAC,      // IN, 4 bytes: model, firmware major, minor, 0 (wValue = 0)
    kGpioFx3 = 0xAD,      // OUT, 4 bytes: the whole GPIO word
    kStartAdc = 0xB2,     // OUT, 4 bytes: the ADC clock in Hz
    kTunerInit = 0xB4,    // OUT, 4 bytes: the R828D reference clock in Hz
    kTunerTune = 0xB5,    // OUT, 8 bytes: the frequency in Hz
    kSetArg = 0xB6,       // OUT, 1 byte; wValue = the value, wIndex = the argument (Rx888Arg)
    kTunerStandby = 0xB8, // OUT, 1 byte
    kEzUsbRw = 0xA0       // the FX3 boot loader: write / read RAM (wValue, wIndex = address low, high), jump to the entry point
};
enum Rx888Arg : uint16_t { kArgR82xxLna = 1, kArgR82xxVga = 2, kArgDat31 = 10, kArgAd8340 = 11 };
// the GPIO word (Interface.h: GPIOPin)
constexpr uint32_t kGpioDither = 1u << 6, kGpioRand = 1u << 7, kGpioBiasHf = 1u << 8, kGpioBiasVhf = 1u << 9, kGpioVhfEn = 1u << 15, kGpioPgaEn = 1u << 16;
constexpr uint8_t kModelRx888Mk2 = 4;   // what TESTFX3 answers in byte 0 (RadioModel: RX888r2)

// The product string of the application firmware: "RX888mk2" (SDDC's USBdescriptor.c). 04b4:00f1 is also what a LimeSDR-USB shows, so this is
// what says it is an RX888 MkII; the other boards of the SDDC family (HF103, BBRF103, RX888, RX999) have their own strings and are not driven.
inline bool rx888ProductIsMk2(const std::string& product) { return product.find("RX888mk2") != std::string::npos; }

struct Rx888UsbDevice {
    uint16_t vid = 0, pid = 0;
    int bus = 0, address = 0;
    std::string manufacturer, product, serial;   // app mode only (the boot loader has none)
    // 0: the name was read (a boot loader has nothing to read). Otherwise the libusb error from opening the device to read it.
    // An application-mode FX3 that could not be opened is still listed, so a radio that just re-enumerated is not invisible.
    int nameError = 0;
    bool bootloader() const { return pid == kRx888PidBoot; }
};

// an open FX3 (the interface is claimed)
class Rx888Link {
public:
    virtual ~Rx888Link() = default;
    // a vendor request to the device; the bytes moved, or a negative libusb error code
    virtual int control(bool in, uint8_t request, uint16_t value, uint16_t index, uint8_t* data, uint16_t length) = 0;
    // false when the radio is on a USB 2 (or slower) link: it cannot carry the samples
    virtual bool superSpeed() const = 0;
    // Keeps `queued` bulk transfers of `bytes` each in flight and hands every finished one to onData (any thread of libusb's event loop:
    // return quickly). onStarted runs once they are all submitted. Blocks until keepGoing() is false (returns 0) or a transfer fails (the negative
    // libusb error code); every transfer is back before it returns.
    virtual int stream(size_t bytes, int queued, const std::function<void(const uint8_t*, size_t)>& onData, const std::function<bool()>& keepGoing,
                       const std::function<void()>& onStarted) = 0;
    // makes a blocked stream() look at keepGoing again
    virtual void interrupt() = 0;
    // The FX3 has left the bus (the firmware jump). release of the interface then can stall on the dead device; close is enough.
    virtual void noteDisconnected() {}
};

class Rx888Usb {
public:
    virtual ~Rx888Usb() = default;
    // every FX3 (04b4:00f1 and 04b4:00f3) in the order of the system. An app-mode one whose name is not "RX888mk2" is left out.
    // One whose name could not be read is included with nameError set (a LimeSDR-USB shares 04b4:00f1, so a name that was read still decides).
    virtual std::vector<Rx888UsbDevice> list() = 0;
    virtual std::unique_ptr<Rx888Link> open(const Rx888UsbDevice& d, int& libusbError) = 0;
    // After the boot loader drops off the bus: forget cached names and, for libusb, the context, so the next list() sees the new device.
    virtual void rescan() {}
};

Rx888Usb& rx888Usb();
void rx888UseUsb(Rx888Usb* usb);   // tests: nullptr goes back to libusb
// Tests: how long upload waits for the re-enumerated radio, and how often it looks. 0 restores the default.
void rx888SetReenumWaitMs(int ms);
void rx888SetReenumPollMs(int ms);

// The firmware image of the SDDC project (SDDC_FX3.img). Not shipped (the Cypress SDK code inside it has a licence of its own): found in
// this order: ONAIR_RX888_FIRMWARE (a file), SDDC_FX3.img in the OnAir codecs folders (codecFolders()).
std::vector<std::string> rx888FirmwarePaths();
// "" when the image has the format the FX3 boot loader takes (Cypress "CY" header, normal binary image with checksum, sections within the
// file, checksum correct), else what is wrong
std::string rx888FirmwareProblem(const std::vector<uint8_t>& image);
// Writes the image into the RAM of the FX3 in boot loader mode (each 4 KiB read back and compared) and jumps to it; the radio then drops off
// the bus and comes back as 04b4:00f1.
bool rx888LoadFirmware(Rx888Link& link, const std::vector<uint8_t>& image, std::string& err);

// the plain conversions (unit test): the gain slider in dB -> the settings of the radio
struct Rx888Gains {
    int dat31 = 0;      // HF attenuator: 0..63 half dB (kArgDat31)
    int ad8340 = 0;     // HF IF gain as sent in kArgAd8340: 0x00 | 1..19 (low range) or 0x80 | 3..111 (high range)
    int ifIndex = 0;    // the position in the HF IF gain table (0..126) that ad8340 stands for
    int lna = 0;        // VHF R828D LNA + mixer step 0..28
    int vga = 0;        // VHF R828D VGA step 0..15
    double hfIfDb = 0, vhfRfDb = 0, vhfIfDb = 0;   // what those steps give
};
constexpr double kRx888GainMaxDb = 90;

// The clock and the rates (4.1). The LTC2208 is rated for 130 Msps; ExtIO_sddc accepts ADC clocks of 50 to 140 MHz (MAX_ADC_FREQ), so the
// driver sets up to 140 MHz when asked (above 130 is overclocking, with a note). The HF input sees 0 to half the clock without aliasing.
constexpr double kRx888AdcRatedHz = 130e6, kRx888AdcMaxHz = 140e6;
constexpr double kRx888MinOutHz = 0.25e6;
constexpr double kRx888MaxOutHz = kRx888AdcRatedHz;   // reported to the UI: decimation 1 at the rated clock (only half of it is unique spectrum)
struct Rx888RatePlan {
    uint32_t adcHz = 0;   // the ADC clock sent with STARTADC
    int decim = 2;        // output rate = adcHz / decim; 1 = no decimation (the real band and its mirror image)
    double outHz = 0;
};
// wantHz: the rate asked for (0: 8 Msps); adcMode: the "adc" setting ("auto", "64", "128", "exact")
Rx888RatePlan rx888RatePlan(double wantHz, const std::string& adcMode);
// The bulk transfers for an ADC clock: SDDC's 128 KiB each; enough in flight for ~32 ms of samples (32 at 64 Msps, 64 at 130), and the
// queue to the conversion thread holds ~250 ms before the oldest blocks are dropped (and counted).
struct Rx888TransferPlan {
    size_t bytes = 131072;
    int queued = 32;
    size_t maxBlocks = 256;
};
Rx888TransferPlan rx888TransferPlan(double adcHz);
Rx888Gains rx888Gains(double sliderDb, double hfAttenuatorDb);
double rx888HfIfGainDb(int index);

} // namespace native
} // namespace dect2
