// Why a USB radio that is plugged in does not show up (Linux): the radio is on the bus, but this user may not open its device file, or the
// kernel's TV driver holds an RTL-SDR. Read from sysfs, which needs no root. The paths go under DECT2_SYSFS_ROOT when it is set (the tests
// build a fake /sys and /dev tree in a temp folder), so this code is compiled and tested on every platform.
#include "dect2/usb_diag.h"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace dect2 {
namespace {

namespace fs = std::filesystem;

struct UsbId { uint16_t vid, pid; const char* radio; };   // pid 0: every product of that vendor

// The radios OnAir drives. The RTL ids are the ones of the osmocom rtl-sdr udev rules (rtl-sdr.rules).
const UsbId kRadios[] = {
    {0x1d50, 0x6089, "HackRF"}, {0x1d50, 0xcc15, "HackRF (rad1o)"}, {0x1d50, 0x604b, "HackRF Jawbreaker"},
    {0x0bda, 0x2832, "RTL-SDR"}, {0x0bda, 0x2838, "RTL-SDR"}, {0x0413, 0x6680, "RTL-SDR"}, {0x0413, 0x6f0f, "RTL-SDR"},
    {0x0458, 0x707f, "RTL-SDR"}, {0x0ccd, 0x00a9, "RTL-SDR"}, {0x0ccd, 0x00b3, "RTL-SDR"}, {0x0ccd, 0x00b4, "RTL-SDR"},
    {0x0ccd, 0x00b5, "RTL-SDR"}, {0x0ccd, 0x00b7, "RTL-SDR"}, {0x0ccd, 0x00b8, "RTL-SDR"}, {0x0ccd, 0x00b9, "RTL-SDR"},
    {0x0ccd, 0x00c0, "RTL-SDR"}, {0x0ccd, 0x00c6, "RTL-SDR"}, {0x0ccd, 0x00d3, "RTL-SDR"}, {0x0ccd, 0x00d7, "RTL-SDR"},
    {0x0ccd, 0x00e0, "RTL-SDR"}, {0x1554, 0x5020, "RTL-SDR"}, {0x15f4, 0x0131, "RTL-SDR"}, {0x15f4, 0x0133, "RTL-SDR"},
    {0x185b, 0x0620, "RTL-SDR"}, {0x185b, 0x0650, "RTL-SDR"}, {0x185b, 0x0680, "RTL-SDR"}, {0x1b80, 0xd393, "RTL-SDR"},
    {0x1b80, 0xd394, "RTL-SDR"}, {0x1b80, 0xd395, "RTL-SDR"}, {0x1b80, 0xd397, "RTL-SDR"}, {0x1b80, 0xd398, "RTL-SDR"},
    {0x1b80, 0xd39d, "RTL-SDR"}, {0x1b80, 0xd3a4, "RTL-SDR"}, {0x1b80, 0xd3a8, "RTL-SDR"}, {0x1b80, 0xd3af, "RTL-SDR"},
    {0x1b80, 0xd3b0, "RTL-SDR"}, {0x1d19, 0x1101, "RTL-SDR"}, {0x1d19, 0x1102, "RTL-SDR"}, {0x1d19, 0x1103, "RTL-SDR"},
    {0x1d19, 0x1104, "RTL-SDR"}, {0x1f4d, 0xa803, "RTL-SDR"}, {0x1f4d, 0xb803, "RTL-SDR"}, {0x1f4d, 0xc803, "RTL-SDR"},
    {0x1f4d, 0xd286, "RTL-SDR"}, {0x1f4d, 0xd803, "RTL-SDR"},
    {0x1d50, 0x60a1, "Airspy"}, {0x03eb, 0x800c, "Airspy HF+"},
    {0x2cf0, 0x5246, "bladeRF"}, {0x2cf0, 0x5250, "bladeRF 2.0"}, {0x1d50, 0x6066, "bladeRF"},
    {0x0403, 0x601f, "LimeSDR Mini"}, {0x1d50, 0x6108, "LimeSDR"},
    {0x0456, 0xb673, "PlutoSDR"}, {0x0456, 0xb674, "PlutoSDR"},
    {0x2500, 0x0020, "USRP B2xx"}, {0x2500, 0x0021, "USRP B2xx"}, {0x2500, 0x0022, "USRP B2xx"},
    {0x3923, 0x7813, "USRP B2xx"}, {0x3923, 0x7814, "USRP B2xx"},
    {0x1df7, 0, "SDRplay RSP"},
};

const char* radioOf(uint16_t vid, uint16_t pid) {
    for (const auto& r : kRadios) if (r.vid == vid && (r.pid == 0 || r.pid == pid)) return r.radio;
    return nullptr;
}

bool readHex(const fs::path& p, uint16_t& v) {
    std::ifstream f(p);
    std::string s;
    if (!(f >> s)) return false;
    v = (uint16_t)strtoul(s.c_str(), nullptr, 16);
    return true;
}
bool readInt(const fs::path& p, int& v) {
    std::ifstream f(p);
    return (bool)(f >> v);
}

bool canOpen(const std::string& path) {
#ifdef _WIN32
    return _access(path.c_str(), 6) == 0;   // read and write
#else
    return access(path.c_str(), R_OK | W_OK) == 0;
#endif
}

// one of the device's interfaces (<dev>:<config>.<interface>) is bound to that kernel driver
bool boundTo(const fs::path& devices, const std::string& dev, const char* driver) {
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(devices, ec)) {
        const std::string n = e.path().filename().string();
        if (n.compare(0, dev.size() + 1, dev + ":") != 0) continue;
        std::error_code ec2;
        const fs::path link = fs::read_symlink(e.path() / "driver", ec2);
        if (!ec2 && link.filename().string() == driver) return true;
    }
    return false;
}

std::vector<std::string> hintsUnder(const std::string& root) {
    std::vector<std::string> out;
    const fs::path devices = fs::path(root + "/sys/bus/usb/devices");
    std::error_code ec;
    const bool rtlTvDriver = fs::exists(fs::path(root + "/sys/module/dvb_usb_rtl28xxu"), ec);
    for (const auto& e : fs::directory_iterator(devices, ec)) {
        const std::string dev = e.path().filename().string();
        if (dev.find(':') != std::string::npos) continue;   // an interface, not a device
        uint16_t vid = 0, pid = 0;
        if (!readHex(e.path() / "idVendor", vid) || !readHex(e.path() / "idProduct", pid)) continue;
        const char* radio = radioOf(vid, pid);
        if (!radio) continue;
        if (rtlTvDriver && std::string(radio) == "RTL-SDR" && boundTo(devices, dev, "dvb_usb_rtl28xxu"))
            out.push_back("The Linux TV driver dvb_usb_rtl28xxu holds the RTL-SDR: run sudo modprobe -r dvb_usb_rtl28xxu, or install OnAir's blacklist file (the .deb does it)");
        int bus = 0, num = 0;
        if (!readInt(e.path() / "busnum", bus) || !readInt(e.path() / "devnum", num)) continue;
        char node[64];
        snprintf(node, sizeof node, "/dev/bus/usb/%03d/%03d", bus, num);
        if (!canOpen(root + node))
            out.push_back(std::string(radio) + " is plugged in but this user may not open it: install OnAir's udev rules (the .deb does it, or run install-udev-rules.sh from the portable folder), then unplug and replug it");
    }
    return out;
}

} // namespace

std::vector<std::string> usbRadioHints() {
    const char* root = getenv("DECT2_SYSFS_ROOT");   // tests: a fake tree; on Linux the real one otherwise
#ifdef __linux__
    return hintsUnder(root ? root : "");
#else
    if (root && *root) return hintsUnder(root);
    return {};
#endif
}

} // namespace dect2
