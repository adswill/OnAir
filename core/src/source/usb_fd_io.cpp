// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#if defined(__linux__)
#include "usb_fd_io.h"
#include <linux/usbdevice_fs.h>
#include <sys/ioctl.h>
#include <poll.h>
#include <chrono>
#include <vector>

namespace dect2 {
namespace {
struct Urb { std::vector<uint8_t> buf; bool inFlight = false; usbdevfs_urb u{}; };   // usbdevfs_urb ends in a flexible array: it must be the last member
struct Stream { int fd = -1; uint8_t ep = 0; std::vector<Urb> urbs; int inFlight = 0; };

struct LinuxSys : UsbFdSys {
    int control(int fd, uint8_t type, uint8_t req, uint16_t val, uint16_t idx, void* data, uint16_t len, uint32_t ms) override {
        usbdevfs_ctrltransfer c{};
        c.bRequestType = type; c.bRequest = req; c.wValue = val; c.wIndex = idx; c.wLength = len; c.timeout = ms; c.data = data;
        const int r = ioctl(fd, USBDEVFS_CONTROL, &c);
        return r < 0 ? -errno : r;
    }
    int bulk(int fd, uint8_t ep, void* data, int len, uint32_t ms) override {
        usbdevfs_bulktransfer b{};
        b.ep = ep; b.len = (unsigned)len; b.timeout = ms; b.data = data;
        const int r = ioctl(fd, USBDEVFS_BULK, &b);
        return r < 0 ? -errno : r;
    }
    int claim(int fd, unsigned iface) override { return ioctl(fd, USBDEVFS_CLAIMINTERFACE, &iface) < 0 ? -errno : 0; }
    int release(int fd, unsigned iface) override { return ioctl(fd, USBDEVFS_RELEASEINTERFACE, &iface) < 0 ? -errno : 0; }
    int reset(int fd) override { return ioctl(fd, USBDEVFS_RESET, 0) < 0 ? -errno : 0; }
    int clearHalt(int fd, uint8_t ep) override { unsigned e = ep; return ioctl(fd, USBDEVFS_CLEAR_HALT, &e) < 0 ? -errno : 0; }
    int detachKernel(int fd, unsigned iface) override {
        usbdevfs_ioctl io{};
        io.ifno = (int)iface; io.ioctl_code = USBDEVFS_DISCONNECT; io.data = nullptr;
        return ioctl(fd, USBDEVFS_IOCTL, &io) < 0 ? -errno : 0;
    }

    static bool submitOne(Stream& st, Urb& u) {
        u.u = usbdevfs_urb{};
        u.u.type = USBDEVFS_URB_TYPE_BULK;
        u.u.endpoint = st.ep;
        u.u.buffer = u.buf.data();
        u.u.buffer_length = (int)u.buf.size();
        u.u.usercontext = &u;
        if (ioctl(st.fd, USBDEVFS_SUBMITURB, &u.u) < 0) return false;
        u.inFlight = true; st.inFlight++;
        return true;
    }
    int streamStart(int fd, uint8_t ep, int n, int len, void** h) override {
        auto* st = new Stream;
        st->fd = fd; st->ep = (uint8_t)(ep | 0x80);
        st->urbs.resize((size_t)n);
        for (auto& u : st->urbs) u.buf.resize((size_t)len);
        for (auto& u : st->urbs) {
            if (!submitOne(*st, u)) {
                const int e = errno;
                streamStop(st);
                return -e;
            }
        }
        *h = st;
        return 0;
    }
    int streamRead(void* h, uint8_t** data, int* len, uint32_t ms) override {
        auto* st = static_cast<Stream*>(h);
        if (st->inFlight <= 0) return -ENODEV;
        const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        for (;;) {
            usbdevfs_urb* done = nullptr;
            if (ioctl(st->fd, USBDEVFS_REAPURBNDELAY, &done) == 0 && done) {
                auto* u = static_cast<Urb*>(done->usercontext);
                u->inFlight = false; st->inFlight--;
                if (done->status != 0) {
                    const int status = done->status;
                    submitOne(*st, *u);   // keep the queue full; a dead device refuses and the next read reports it
                    return status < 0 ? status : -EIO;
                }
                *data = u->buf.data(); *len = done->actual_length;
                return 0;
            }
            if (errno != EAGAIN) return -errno;
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(end - std::chrono::steady_clock::now()).count();
            if (left <= 0) return -ETIMEDOUT;
            pollfd p{st->fd, POLLOUT, 0};   // usbfs signals a completed URB as writable
            poll(&p, 1, (int)left);
        }
    }
    int streamRecycle(void* h, uint8_t* data) override {
        auto* st = static_cast<Stream*>(h);
        for (auto& u : st->urbs) if (u.buf.data() == data && !u.inFlight) return submitOne(*st, u) ? 0 : -errno;
        return -EINVAL;
    }
    void streamStop(void* h) override {
        auto* st = static_cast<Stream*>(h);
        if (!st) return;
        for (auto& u : st->urbs) if (u.inFlight) ioctl(st->fd, USBDEVFS_DISCARDURB, &u.u);
        // the kernel owns the buffers until each URB is reaped
        for (int tries = 0; st->inFlight > 0 && tries < 100; tries++) {
            usbdevfs_urb* done = nullptr;
            if (ioctl(st->fd, USBDEVFS_REAPURBNDELAY, &done) == 0 && done) {
                static_cast<Urb*>(done->usercontext)->inFlight = false; st->inFlight--;
                continue;
            }
            if (errno != EAGAIN) break;
            pollfd p{st->fd, POLLOUT, 0};
            poll(&p, 1, 10);
        }
        if (st->inFlight > 0) return;   // the kernel still holds them (the device went away mid-cancel): leaking is safer than freeing
        delete st;
    }
};
}
UsbFdSys& usbFdLinuxSys() { static LinuxSys s; return s; }
} // namespace dect2
#endif
