// Why a USB radio that is plugged in does not show up: permissions, a kernel driver that holds it (Linux).
#pragma once
#include <string>
#include <vector>

namespace dect2 {

// One plain sentence per radio that is on the USB bus but cannot be opened, with what to do about it. Empty when nothing is wrong
// or the platform has no such check. Cheap enough to call when the radio list is refreshed.
// Linux only; with DECT2_SYSFS_ROOT set, the /sys and /dev paths are read under that folder instead (tests, on every platform).
std::vector<std::string> usbRadioHints();

} // namespace dect2
