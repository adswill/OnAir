// Serial port for the ESP32 radio: an interface (tests use an in-memory pair), the POSIX implementation, and the search for ESP32 boards.
#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace dect2 {
namespace esp32 {

class SerialPort {
public:
    virtual ~SerialPort() {}
    virtual bool write(const uint8_t* p, size_t n) = 0;
    virtual int read(uint8_t* p, size_t n, int timeoutMs) = 0;   // bytes read (0 on timeout), -1 when the port is gone
    virtual void close() = 0;
    virtual std::string name() const = 0;
};

// serial device files that may be an ESP32 board (USB bridge or native USB): /dev/cu.usbserial*, /dev/cu.usbmodem*, /dev/ttyUSB*, /dev/ttyACM*; empty on Windows
std::vector<std::string> candidatePorts();
// opens a device file raw, 8N1 (the baud is ignored by native USB boards); null with err filled on failure
std::unique_ptr<SerialPort> openSerial(const std::string& path, int baud, std::string& err);

} // namespace esp32
} // namespace dect2
