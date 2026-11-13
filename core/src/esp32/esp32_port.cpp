// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/esp32/esp32_port.h"
#include <algorithm>
#include <cstring>
#ifndef _WIN32
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <termios.h>
#include <unistd.h>
#endif

namespace dect2 {
namespace esp32 {

#ifndef _WIN32
namespace {
class PosixSerial : public SerialPort {
public:
    PosixSerial(int fd, std::string n) : fd_(fd), name_(std::move(n)) {}
    ~PosixSerial() override { close(); }
    bool write(const uint8_t* p, size_t n) override {
        while (n) {
            const ssize_t w = ::write(fd_, p, n);
            if (w < 0) { if (errno == EAGAIN || errno == EINTR) continue; return false; }
            p += w; n -= (size_t)w;
        }
        return true;
    }
    int read(uint8_t* p, size_t n, int timeoutMs) override {
        if (fd_ < 0) return -1;
        pollfd pf{fd_, POLLIN, 0};
        const int r = ::poll(&pf, 1, timeoutMs);
        if (r == 0) return 0;
        if (r < 0) return errno == EINTR ? 0 : -1;
        if (pf.revents & (POLLERR | POLLHUP | POLLNVAL)) return -1;
        const ssize_t k = ::read(fd_, p, n);
        if (k < 0) return (errno == EAGAIN || errno == EINTR) ? 0 : -1;
        return k == 0 ? -1 : (int)k;
    }
    void close() override { if (fd_ >= 0) { ::close(fd_); fd_ = -1; } }
    std::string name() const override { return name_; }
private:
    int fd_;
    std::string name_;
};
speed_t baudConst(int b) {
    switch (b) { case 9600: return B9600; case 19200: return B19200; case 38400: return B38400; case 57600: return B57600; case 230400: return B230400;
        default: return B115200; }
}
} // namespace

std::vector<std::string> candidatePorts() {
    std::vector<std::string> out;
    if (DIR* d = opendir("/dev")) {
        while (dirent* e = readdir(d)) {
            const std::string n = e->d_name;
            if (n.rfind("cu.usbserial", 0) == 0 || n.rfind("cu.usbmodem", 0) == 0 || n.rfind("cu.SLAB_USBtoUART", 0) == 0 || n.rfind("cu.wchusbserial", 0) == 0 ||
                n.rfind("ttyUSB", 0) == 0 || n.rfind("ttyACM", 0) == 0) out.push_back("/dev/" + n);
        }
        closedir(d);
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::unique_ptr<SerialPort> openSerial(const std::string& path, int baud, std::string& err) {
    const int fd = ::open(path.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) { err = std::string("cannot open ") + path + ": " + std::strerror(errno); return nullptr; }
    termios t;
    if (tcgetattr(fd, &t) == 0) {
        cfmakeraw(&t); cfsetspeed(&t, baudConst(baud));
        t.c_cflag |= CLOCAL | CREAD; t.c_cflag &= ~(CSTOPB | CRTSCTS);
        tcsetattr(fd, TCSANOW, &t);
    }
    return std::unique_ptr<SerialPort>(new PosixSerial(fd, path));
}
#else
std::vector<std::string> candidatePorts() { return {}; }
std::unique_ptr<SerialPort> openSerial(const std::string&, int, std::string& err) { err = "serial ports are not supported on Windows in this version"; return nullptr; }
#endif

} // namespace esp32
} // namespace dect2
