// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Transfers on a usbfs descriptor (Linux, Android): control and bulk requests through ioctl, mapped to libusb's error codes. The system calls are
// behind UsbFdSys so the chunking and error mapping can be tested with a fake. Used by the RTL-SDR driver on Android (rtl_fd_shim.cpp).
#pragma once
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <string>

#ifndef ESHUTDOWN
#define ESHUTDOWN 108   // not defined by MinGW (usbfs is Linux-only, the value is only compared)
#endif

namespace dect2 {

// libusb error codes (the Rust protocol layer reports these)
enum { kUsbOk = 0, kUsbIo = -1, kUsbInvalid = -2, kUsbAccess = -3, kUsbNoDevice = -4, kUsbBusy = -6, kUsbTimeout = -7, kUsbOverflow = -8,
       kUsbPipe = -9, kUsbOther = -99 };

inline int usbErrnoToLibusb(int e) {
    switch (e) {
    case 0: return kUsbOk;
    case ETIMEDOUT: return kUsbTimeout;
    case ENODEV: case ESHUTDOWN: case ENOENT: case EPROTO: return kUsbNoDevice;   // unplugged: usbfs answers ENODEV, or ESHUTDOWN / EPROTO on the URB
    case EPIPE: return kUsbPipe;
    case EOVERFLOW: return kUsbOverflow;
    case EBUSY: return kUsbBusy;
    case EACCES: case EPERM: return kUsbAccess;
    case EINVAL: return kUsbInvalid;
    case EIO: return kUsbIo;
    default: return kUsbOther;
    }
}

// The system calls. ret >= 0: bytes done; < 0: -errno.
struct UsbFdSys {
    virtual ~UsbFdSys() = default;
    virtual int control(int fd, uint8_t type, uint8_t req, uint16_t val, uint16_t idx, void* data, uint16_t len, uint32_t timeoutMs) = 0;
    virtual int bulk(int fd, uint8_t ep, void* data, int len, uint32_t timeoutMs) = 0;
    virtual int claim(int fd, unsigned iface) = 0;      // 0 or -errno
    virtual int release(int fd, unsigned iface) = 0;
    virtual int reset(int fd) = 0;
    virtual int clearHalt(int fd, uint8_t ep) = 0;
    virtual int detachKernel(int fd, unsigned iface) = 0;   // best effort (Android has no kernel driver bound to a radio it handed over)
    // A queue of bulk reads kept in flight on the endpoint (usbfs URBs: SUBMITURB / REAPURB), the way librtlsdr's rtlsdr_read_async does. A
    // read one 16 KiB request at a time leaves the radio's small FIFO unserved between two requests and it overruns at megasample rates.
    // streamStart: 0 and a handle, or -errno (-ENOSYS = not supported: the caller reads block by block). streamRead: 0 with the oldest completed
    // buffer (valid until streamRecycle), -ETIMEDOUT if none within timeoutMs, or -errno (a failed transfer; the buffer is recycled by the
    // callee). streamStop cancels what is in flight and frees the queue.
    virtual int streamStart(int, uint8_t, int, int, void**) { return -ENOSYS; }
    virtual int streamRead(void*, uint8_t**, int*, uint32_t) { return -ENOSYS; }
    virtual int streamRecycle(void*, uint8_t*) { return -ENOSYS; }
    virtual void streamStop(void*) {}
};
UsbFdSys& usbFdLinuxSys();   // ioctl on usbfs (usb_fd_io.cpp, Linux and Android only)

// One radio on a descriptor. The descriptor is not closed here.
class UsbFdDevice {
public:
    // usbfs takes at most 16 KiB per bulk request on older kernels: larger reads are made in pieces of this size
    static constexpr int kBulkChunk = 16384;
    UsbFdDevice(UsbFdSys& sys, int fd) : sys_(sys), fd_(fd) {}
    ~UsbFdDevice() { releaseInterface(); }
    int claimInterface(unsigned iface = 0) {
        sys_.detachKernel(fd_, iface);
        const int r = sys_.claim(fd_, iface);
        if (r < 0) return usbErrnoToLibusb(-r);
        iface_ = (int)iface;
        return 0;
    }
    void releaseInterface() { if (iface_ >= 0) { sys_.release(fd_, (unsigned)iface_); iface_ = -1; } }
    int controlIn(uint8_t type, uint8_t req, uint16_t val, uint16_t idx, uint8_t* d, uint16_t len, uint32_t ms) {
        const int r = sys_.control(fd_, (uint8_t)(type | 0x80), req, val, idx, d, len, ms);
        return r < 0 ? usbErrnoToLibusb(-r) : r;
    }
    int controlOut(uint8_t type, uint8_t req, uint16_t val, uint16_t idx, const uint8_t* d, uint16_t len, uint32_t ms) {
        const int r = sys_.control(fd_, (uint8_t)(type & ~0x80), req, val, idx, const_cast<uint8_t*>(d), len, ms);
        return r < 0 ? usbErrnoToLibusb(-r) : r;
    }
    // Reads up to len bytes: pieces of kBulkChunk until len is filled or a piece comes up short. Bytes read before an error are returned
    // (the error comes on the next call).
    int bulkRead(uint8_t ep, uint8_t* d, int len, uint32_t ms) {
        int got = 0;
        while (got < len) {
            const int want = len - got < kBulkChunk ? len - got : kBulkChunk;
            const int r = sys_.bulk(fd_, (uint8_t)(ep | 0x80), d + got, want, ms);
            if (r < 0) return got > 0 ? got : usbErrnoToLibusb(-r);
            got += r;
            if (r < want) break;
        }
        return got;
    }
    int resetDevice() { const int r = sys_.reset(fd_); return r < 0 ? usbErrnoToLibusb(-r) : 0; }
    // String descriptor `index` in the first language, as ASCII (non-ASCII UTF-16 units become '?'). Length or a libusb error.
    int stringDescriptor(uint8_t index, uint8_t* out, int cap) {
        if (index == 0 || cap <= 0) return kUsbInvalid;
        uint8_t buf[256];
        int r = controlIn(0x00, 0x06, (uint16_t)(0x0300 | 0), 0, buf, sizeof buf, 1000);   // GET_DESCRIPTOR string 0: the language ids
        if (r < 4) return r < 0 ? r : kUsbIo;
        const uint16_t lang = (uint16_t)(buf[2] | (buf[3] << 8));
        r = controlIn(0x00, 0x06, (uint16_t)(0x0300 | index), lang, buf, sizeof buf, 1000);
        if (r < 2) return r < 0 ? r : kUsbIo;
        return parseStringDescriptor(buf, r, out, cap);
    }
    static int parseStringDescriptor(const uint8_t* b, int n, uint8_t* out, int cap) {
        if (n < 2 || b[1] != 0x03) return kUsbIo;
        const int len = b[0] < n ? b[0] : n;
        int o = 0;
        for (int i = 2; i + 1 < len && o < cap - 1; i += 2) out[o++] = b[i + 1] == 0 && b[i] < 0x80 ? b[i] : '?';
        out[o] = 0;
        return o;
    }
private:
    UsbFdSys& sys_;
    int fd_;
    int iface_ = -1;
};

} // namespace dect2
